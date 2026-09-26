/**
 * @file
 * Own the dialogue registry, option visibility, and active conversations.
 *
 * Two tables live here, and neither has a compiled ceiling. Definitions sit in
 * an array indexed by dialogue id that grows to whatever the data directory
 * declares. Conversations sit in an open-addressed map keyed by character id --
 * a map rather than an array because character ids come from the database and
 * are nothing like small: the array this replaced indexed sessions[character_id]
 * against MAX_PLAYERS, so every character past the thousandth simply could not
 * hold a conversation.
 */

#include "dialogue_system.h"
#include "player_data.h"
#include "quest_system.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <dirent.h>
#include <pthread.h>

/** Close a conversation left untouched for this long, in seconds. */
#define DIALOGUE_SESSION_TIMEOUT 60.0

/* --- Definition registry ------------------------------------------------- */

static DialogueDef** g_dialogues     = NULL;   /**< Indexed by dialogue id. */
static int           g_dialogue_cap  = 0;      /**< Allocated slots, including the unused id 0. */
static int           g_dialogue_count = 0;

/**
 * Grow the registry so `id` is a valid index.
 *
 * @return 1 when the slot exists afterwards, or 0 on allocation failure.
 */
static int registry_reserve(uint32_t id) {
    if ((int)id < g_dialogue_cap) return 1;

    int wanted = g_dialogue_cap ? g_dialogue_cap : 16;
    while (wanted <= (int)id) {
        if (wanted > (1 << 30)) return 0;
        wanted *= 2;
    }

    DialogueDef** grown = realloc(g_dialogues, (size_t)wanted * sizeof(*grown));
    if (!grown) return 0;

    memset(grown + g_dialogue_cap, 0,
           (size_t)(wanted - g_dialogue_cap) * sizeof(*grown));
    g_dialogues    = grown;
    g_dialogue_cap = wanted;
    return 1;
}

/**
 * Install a parsed definition, replacing any dialogue with the same identifier.
 */
int dialogue_registry_install(DialogueDef* dialogue) {
    if (!dialogue) return 0;

    if (dialogue->dialogue_id == 0 || !registry_reserve(dialogue->dialogue_id)) {
        LOG_ERROR("[DIALOGUE] Cannot store dialogue %u", dialogue->dialogue_id);
        dialogue_def_free(dialogue);
        return 0;
    }

    DialogueDef** slot = &g_dialogues[dialogue->dialogue_id];
    if (*slot) {
        LOG_ERROR("[DIALOGUE] dialogue id %u is defined more than once; "
                  "replacing '%s' with '%s'",
                  dialogue->dialogue_id, (*slot)->name, dialogue->name);
        dialogue_def_free(*slot);
        g_dialogue_count--;
    }

    *slot = dialogue;
    g_dialogue_count++;
    LOG_INFO("[DIALOGUE] Loaded '%s' (id=%u, %d pages)",
             dialogue->name, dialogue->dialogue_id, dialogue->page_count);
    return 1;
}

const DialogueDef* dialogue_get(uint32_t dialogue_id) {
    if (dialogue_id == 0 || (int)dialogue_id >= g_dialogue_cap) return NULL;
    return g_dialogues[dialogue_id];
}

int dialogues_get_count(void) {
    return g_dialogue_count;
}

const DialoguePageDef* dialogue_find_page(const DialogueDef* dialogue, int32_t page_num) {
    if (!dialogue) return NULL;
    for (int i = 0; i < dialogue->page_count; i++) {
        if (dialogue->pages[i].page_num == page_num) return &dialogue->pages[i];
    }
    return NULL;
}

const DialogueOptionDef* dialogue_page_find_option(const DialoguePageDef* page, uint8_t option_id) {
    if (!page) return NULL;
    for (int i = 0; i < page->option_count; i++) {
        if (page->options[i].option_id == option_id) return &page->options[i];
    }
    return NULL;
}

/* --- Option visibility --------------------------------------------------- */

/** Describe what one character's record says about a single quest. */
typedef struct {
    uint8_t known;      /**< In progress or already finished. */
    uint8_t active;     /**< In progress, objectives outstanding. */
    uint8_t complete;   /**< Objectives met, not yet handed in. */
    uint8_t done;       /**< Handed in. */
    uint8_t available;  /**< Eligible to take it right now. */
} QuestStanding;

/**
 * Read a character's race, level, and standing on one quest under one lock.
 *
 * @param quest_id  Quest to report on; zero skips the quest lookup.
 * @return          1 when the character is loaded, or 0 otherwise.
 */
static int read_player_facts(uint32_t character_id, uint32_t quest_id,
                             uint32_t* out_race, int* out_level,
                             QuestStanding* out_standing) {
    memset(out_standing, 0, sizeof(*out_standing));

    ActivePlayer* p = player_acquire(character_id);
    if (!p) return 0;

    *out_race  = p->race_id;
    *out_level = p->level;

    if (quest_id != 0) {
        const struct PlayerQuestSlot* slot = quest_log_find(&p->quests.log, quest_id);
        if (slot) {
            out_standing->known    = 1;
            out_standing->active   = slot->is_active && !slot->is_complete;
            out_standing->complete = slot->is_active && slot->is_complete;
        } else if (quest_history_contains(&p->quests.history, quest_id)) {
            out_standing->known = 1;
            out_standing->done  = 1;
        }

        /* Asked of the quest rather than restated here. A chain is declared
         * once, on the quest, and both the offer and the grant read it from
         * there -- which is the only way they cannot disagree. */
        out_standing->available = quest_is_available_for(quest_get(quest_id), p);
    }

    player_release(p);
    return 1;
}

/**
 * Report whether one condition holds.
 */
static int condition_holds(const DialogueCondition* condition, uint32_t race_id,
                           int level, const QuestStanding* standing) {
    switch ((DialogueConditionKind)condition->kind) {
        case DIALOGUE_COND_RACE:              return race_id == condition->value;
        case DIALOGUE_COND_NOT_RACE:          return race_id != condition->value;
        case DIALOGUE_COND_MIN_LEVEL:         return level >= (int)condition->value;
        case DIALOGUE_COND_MAX_LEVEL:         return level <= (int)condition->value;
        case DIALOGUE_COND_QUEST_NOT_STARTED: return !standing->known;
        case DIALOGUE_COND_QUEST_ACTIVE:      return standing->active;
        case DIALOGUE_COND_QUEST_COMPLETE:    return standing->complete;
        case DIALOGUE_COND_QUEST_TURNED_IN:   return standing->done;
        case DIALOGUE_COND_QUEST_AVAILABLE:   return standing->available;
        case DIALOGUE_COND_KIND_COUNT:        break;
    }
    return 0;
}

/**
 * Report whether every condition on an option holds for a character.
 */
int dialogue_option_available(const DialogueOptionDef* option, uint32_t character_id) {
    if (!option) return 0;
    if (option->condition_count <= 0) return 1;

    for (int i = 0; i < option->condition_count; i++) {
        const DialogueCondition* condition = &option->conditions[i];

        /* Only quest conditions need the log, and each names its own quest, so
         * the facts are read per condition rather than once for the option. */
        uint32_t quest_id = 0;
        switch ((DialogueConditionKind)condition->kind) {
            case DIALOGUE_COND_QUEST_NOT_STARTED:
            case DIALOGUE_COND_QUEST_ACTIVE:
            case DIALOGUE_COND_QUEST_COMPLETE:
            case DIALOGUE_COND_QUEST_TURNED_IN:
            case DIALOGUE_COND_QUEST_AVAILABLE:
                quest_id = condition->value;
                break;
            default:
                break;
        }

        uint32_t race_id = 0;
        int      level   = 0;
        QuestStanding standing;
        if (!read_player_facts(character_id, quest_id, &race_id, &level, &standing))
            return 0;

        if (!condition_holds(condition, race_id, level, &standing)) return 0;
    }

    return 1;
}

/**
 * Collect the option identifiers a character may currently be offered.
 */
int dialogue_page_visible_options(const DialoguePageDef* page, uint32_t character_id,
                                  uint8_t* out_ids, int max_ids) {
    if (!page || !out_ids || max_ids <= 0) return 0;

    int written = 0;
    int dropped = 0;

    for (int i = 0; i < page->option_count; i++) {
        if (!dialogue_option_available(&page->options[i], character_id)) continue;
        if (written >= max_ids) { dropped++; continue; }
        out_ids[written++] = page->options[i].option_id;
    }

    if (dropped > 0) {
        LOG_ERROR("[DIALOGUE] page %d offers %d options at once but a packet carries "
                  "%d; %d were dropped and cannot be chosen. Split the page or add "
                  "conditions so fewer pass together.",
                  page->page_num, written + dropped, max_ids, dropped);
    }

    return written;
}

/* --- Sessions ------------------------------------------------------------ */

/** Hold one map bucket. character_id zero marks an empty bucket. */
typedef struct {
    DialogueSession session;
} SessionBucket;

static SessionBucket*  g_sessions      = NULL;
static uint32_t        g_session_mask  = 0;    /**< Capacity - 1; capacity is a power of two. */
static int             g_session_count = 0;
static pthread_mutex_t g_sessions_lock = PTHREAD_MUTEX_INITIALIZER;

static double now_seconds(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1000000.0;
}

static inline uint32_t session_hash(uint32_t key) {
    key ^= key >> 16;
    key *= 0x7feb352dU;
    key ^= key >> 15;
    key *= 0x846ca68bU;
    key ^= key >> 16;
    return key;
}

/** Find a character's bucket, or NULL when it holds no conversation. Caller holds the lock. */
static DialogueSession* session_lookup(uint32_t character_id) {
    if (!g_sessions || character_id == 0) return NULL;

    uint32_t pos = session_hash(character_id) & g_session_mask;
    for (uint32_t probe = 0; probe <= g_session_mask; probe++) {
        DialogueSession* s = &g_sessions[pos].session;
        if (s->character_id == 0) return NULL;
        if (s->character_id == character_id) return s;
        pos = (pos + 1) & g_session_mask;
    }
    return NULL;
}

static int session_table_grow(void);

/**
 * Claim a bucket for a character, growing the map when it is over half full.
 *
 * Caller holds the lock.
 *
 * @return The bucket, or NULL when the map cannot grow.
 */
static DialogueSession* session_claim(uint32_t character_id) {
    if (character_id == 0) return NULL;

    DialogueSession* existing = session_lookup(character_id);
    if (existing) return existing;

    if (!g_sessions || (g_session_count + 1) * 2 > (int)(g_session_mask + 1)) {
        if (!session_table_grow()) return NULL;
    }

    uint32_t pos = session_hash(character_id) & g_session_mask;
    for (uint32_t probe = 0; probe <= g_session_mask; probe++) {
        DialogueSession* s = &g_sessions[pos].session;
        if (s->character_id == 0) {
            memset(s, 0, sizeof(*s));
            s->character_id = character_id;
            g_session_count++;
            return s;
        }
        pos = (pos + 1) & g_session_mask;
    }
    return NULL;
}

/**
 * Double the session map and reinsert every conversation. Caller holds the lock.
 *
 * @return 1 on success, or 0 when the allocation fails.
 */
static int session_table_grow(void) {
    uint32_t old_capacity = g_sessions ? g_session_mask + 1 : 0;
    uint32_t new_capacity = old_capacity ? old_capacity * 2 : 64;

    SessionBucket* grown = calloc(new_capacity, sizeof(*grown));
    if (!grown) return 0;

    SessionBucket* old      = g_sessions;
    uint32_t       old_mask = g_session_mask;

    g_sessions     = grown;
    g_session_mask = new_capacity - 1;
    g_session_count = 0;

    if (old) {
        for (uint32_t i = 0; i < old_capacity; i++) {
            if (old[i].session.character_id == 0) continue;
            DialogueSession* slot = session_claim(old[i].session.character_id);
            if (slot) *slot = old[i].session;
        }
        (void)old_mask;
        free(old);
    }
    return 1;
}

/**
 * Erase a bucket and repair the probe chain that ran through it.
 *
 * Caller holds the lock. Tombstones are avoided by reinserting the run that
 * follows, which keeps lookups terminating on the first empty bucket.
 */
static void session_erase(DialogueSession* victim) {
    if (!victim) return;

    memset(victim, 0, sizeof(*victim));
    g_session_count--;

    uint32_t start = (uint32_t)((SessionBucket*)victim - g_sessions);
    uint32_t pos   = (start + 1) & g_session_mask;

    while (g_sessions[pos].session.character_id != 0) {
        DialogueSession moved = g_sessions[pos].session;
        memset(&g_sessions[pos].session, 0, sizeof(g_sessions[pos].session));
        g_session_count--;

        DialogueSession* slot = session_claim(moved.character_id);
        if (slot) *slot = moved;

        pos = (pos + 1) & g_session_mask;
    }
}

int dialogue_session_open(uint32_t character_id, uint32_t npc_id, uint32_t dialogue_id) {
    pthread_mutex_lock(&g_sessions_lock);

    DialogueSession* session = session_claim(character_id);
    if (session) {
        session->character_id          = character_id;
        session->npc_id                = npc_id;
        session->dialogue_id           = dialogue_id;
        session->current_page          = 0;
        session->is_active             = 1;
        session->last_interaction_time = now_seconds();
    }

    pthread_mutex_unlock(&g_sessions_lock);

    if (!session)
        LOG_ERROR("[DIALOGUE] Could not store a conversation for character %u", character_id);

    /* The pointer stays inside the lock deliberately; only the outcome leaves. */
    return session != NULL;
}

int dialogue_session_snapshot(uint32_t character_id, DialogueSession* out) {
    pthread_mutex_lock(&g_sessions_lock);

    DialogueSession* session = session_lookup(character_id);
    int active = (session && session->is_active);
    if (active && out) *out = *session;

    pthread_mutex_unlock(&g_sessions_lock);
    return active;
}

void dialogue_session_close(uint32_t character_id) {
    pthread_mutex_lock(&g_sessions_lock);
    session_erase(session_lookup(character_id));
    pthread_mutex_unlock(&g_sessions_lock);
}

void dialogue_session_update_page(uint32_t character_id, int32_t new_page) {
    pthread_mutex_lock(&g_sessions_lock);

    DialogueSession* session = session_lookup(character_id);
    if (session && session->is_active) {
        session->current_page          = new_page;
        session->last_interaction_time = now_seconds();
    }

    pthread_mutex_unlock(&g_sessions_lock);
}

void dialogue_check_timeouts(void) {
    double now = now_seconds();

    pthread_mutex_lock(&g_sessions_lock);

    /* Erasing repairs the probe chain by moving later entries backwards, so the
     * sweep restarts rather than walking past a bucket that just moved. */
    int swept;
    do {
        swept = 0;
        for (uint32_t i = 0; g_sessions && i <= g_session_mask; i++) {
            DialogueSession* s = &g_sessions[i].session;
            if (s->character_id == 0) continue;
            if (now - s->last_interaction_time <= DIALOGUE_SESSION_TIMEOUT) continue;

            LOG_DEBUG("[DIALOGUE] Conversation timed out for character %u", s->character_id);
            session_erase(s);
            swept = 1;
            break;
        }
    } while (swept);

    pthread_mutex_unlock(&g_sessions_lock);
}

/* --- Lifecycle ----------------------------------------------------------- */

/**
 * Read an entire file into a terminated buffer. The caller frees it.
 */
static char* read_file(const char* filepath) {
    FILE* f = fopen(filepath, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) { fclose(f); return NULL; }

    char* buffer = malloc((size_t)size + 1);
    if (!buffer) { fclose(f); return NULL; }

    size_t got = fread(buffer, 1, (size_t)size, f);
    buffer[got] = '\0';
    fclose(f);
    return buffer;
}

int dialogue_system_init(const char* dir_path) {
    dialogue_system_cleanup();

    LOG_INFO("[DIALOGUE] Loading dialogues from directory: %s", dir_path);

    DIR* dir = opendir(dir_path);
    if (!dir) {
        LOG_ERROR("[DIALOGUE] Failed to open dialogue directory: %s", dir_path);
        return 0;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        const char* fname = entry->d_name;
        size_t len = strlen(fname);
        if (len < 6 || strcmp(fname + len - 5, ".json") != 0) continue;

        char filepath[512];
        snprintf(filepath, sizeof(filepath), "%s/%s", dir_path, fname);

        char* json_text = read_file(filepath);
        if (!json_text) {
            LOG_ERROR("[DIALOGUE] Could not read file: %s", filepath);
            continue;
        }

        dialogue_load_document(json_text, fname);
        free(json_text);
    }

    closedir(dir);
    LOG_INFO("[DIALOGUE] Successfully loaded %d dialogues", g_dialogue_count);
    return 1;
}

void dialogue_system_cleanup(void) {
    for (int i = 0; i < g_dialogue_cap; i++) {
        dialogue_def_free(g_dialogues[i]);
        g_dialogues[i] = NULL;
    }
    free(g_dialogues);
    g_dialogues      = NULL;
    g_dialogue_cap   = 0;
    g_dialogue_count = 0;

    pthread_mutex_lock(&g_sessions_lock);
    free(g_sessions);
    g_sessions      = NULL;
    g_session_mask  = 0;
    g_session_count = 0;
    pthread_mutex_unlock(&g_sessions_lock);
}
