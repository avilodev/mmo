/**
 * @file
 * Load quest definitions from JSON and hold them in a registry that grows to fit.
 */

#include "quest_registry.h"
#include "str_fixed.h"
#include "log.h"
#include "json_util.h"
#include "race_registry.h"
#include "utils.h"

#include <stdlib.h>
#include <string.h>

/* --- Registry ------------------------------------------------------------ */

static QuestDef** g_quests    = NULL;   /**< Indexed by quest id; grows to fit. */
static int        g_quest_cap = 0;
static int        g_quest_count = 0;

/**
 * Grow the registry so `id` is a valid index.
 *
 * @return 1 when the slot exists afterwards, or 0 on allocation failure.
 */
static int registry_reserve(uint32_t id) {
    if ((int)id < g_quest_cap) return 1;

    int wanted = g_quest_cap ? g_quest_cap : 32;
    while (wanted <= (int)id) {
        if (wanted > (1 << 30)) return 0;
        wanted *= 2;
    }

    QuestDef** grown = realloc(g_quests, (size_t)wanted * sizeof(*grown));
    if (!grown) return 0;

    memset(grown + g_quest_cap, 0, (size_t)(wanted - g_quest_cap) * sizeof(*grown));
    g_quests    = grown;
    g_quest_cap = wanted;
    return 1;
}

const QuestDef* quest_get(uint32_t quest_id) {
    if (quest_id == 0 || (int)quest_id >= g_quest_cap) return NULL;
    return g_quests[quest_id];
}

int quest_get_count(void) {
    return g_quest_count;
}

/* --- Loading ------------------------------------------------------------- */

/** Map an objective's JSON spelling to its type. */
static const struct {
    const char*        key;
    QuestObjectiveType type;
} OBJECTIVE_SPELLINGS[] = {
    { "kill",    QUEST_OBJECTIVE_KILL    },
    { "collect", QUEST_OBJECTIVE_COLLECT },
    { "talk",    QUEST_OBJECTIVE_TALK    },
};

#define OBJECTIVE_SPELLING_COUNT \
    ((int)(sizeof(OBJECTIVE_SPELLINGS) / sizeof(OBJECTIVE_SPELLINGS[0])))

/**
 * Resolve an objective "type" string.
 *
 * @return The type, or QUEST_OBJECTIVE_TYPE_COUNT when the spelling is unknown.
 */
static QuestObjectiveType objective_type_from_key(const char* key) {
    for (int i = 0; i < OBJECTIVE_SPELLING_COUNT; i++)
        if (strcmp(OBJECTIVE_SPELLINGS[i].key, key) == 0)
            return OBJECTIVE_SPELLINGS[i].type;
    return QUEST_OBJECTIVE_TYPE_COUNT;
}

/**
 * Read one quest's objectives.
 */
static void parse_objectives(const JsonValue* quest_obj, QuestDef* quest) {
    const JsonValue* list = json_get(quest_obj, "objectives");
    if (json_type(list) != JSON_ARRAY) return;

    int authored = json_count(list);
    for (int i = 0; i < authored; i++) {
        if (quest->obj_count >= MAX_QUEST_OBJECTIVES) {
            LOG_ERROR("[QUEST] quest %u '%s' declares %d objectives; a packet carries "
                      "%d, so the rest were dropped",
                      quest->quest_id, quest->title, authored, MAX_QUEST_OBJECTIVES);
            break;
        }

        const JsonValue* entry = json_at(list, i);
        QuestObjectiveDef* objective = &quest->objectives[quest->obj_count];

        const char* type_key = json_get_string(entry, "type", "kill");
        QuestObjectiveType type = objective_type_from_key(type_key);
        if (type == QUEST_OBJECTIVE_TYPE_COUNT) {
            LOG_ERROR("[QUEST] quest %u objective %d has unknown type '%s'; skipped",
                      quest->quest_id, i, type_key);
            continue;
        }

        objective->type           = (uint8_t)type;
        objective->target_id      = (uint32_t)json_get_int(entry, "target_id", 0);
        objective->required_count = json_get_int(entry, "required", 1);
        STR_COPY_FIELD(objective->description, json_get_string(entry, "description", ""));

        const JsonValue* marker = json_get(entry, "marker");
        if (json_type(marker) == JSON_OBJECT) {
            objective->has_marker = 1;
            objective->marker_x   = (float)json_get_number(marker, "x", 0.0);
            objective->marker_y   = (float)json_get_number(marker, "y", 0.0);
        }

        quest->obj_count++;
    }
}

/**
 * Read one quest's item rewards.
 */
static void parse_item_rewards(const JsonValue* quest_obj, QuestDef* quest) {
    const JsonValue* list = json_get(quest_obj, "item_rewards");
    if (json_type(list) != JSON_ARRAY) return;

    int authored = json_count(list);
    for (int i = 0; i < authored && quest->item_reward_count < MAX_QUEST_OBJECTIVES; i++) {
        const JsonValue* entry = json_at(list, i);
        QuestItemReward* reward = &quest->item_rewards[quest->item_reward_count];
        reward->item_id  = (uint32_t)json_get_int(entry, "item_id", 0);
        reward->quantity = (uint8_t)json_get_int(entry, "quantity", 1);
        quest->item_reward_count++;
    }
}

/**
 * Read one list of prerequisite quest identifiers.
 *
 * @param out_ids  Receives an owned array, or NULL when the key is absent.
 * @return         The identifier count.
 */
static int parse_quest_id_list(const JsonValue* obj, const char* key, uint32_t** out_ids) {
    *out_ids = NULL;

    const JsonValue* list = json_get(obj, key);
    if (json_type(list) != JSON_ARRAY) return 0;

    int authored = json_count(list);
    if (authored <= 0) return 0;

    uint32_t* ids = calloc((size_t)authored, sizeof(*ids));
    if (!ids) return 0;

    int count = 0;
    for (int i = 0; i < authored; i++) {
        int id = json_as_int(json_at(list, i), 0);
        if (id > 0) ids[count++] = (uint32_t)id;
    }

    if (count == 0) { free(ids); return 0; }
    *out_ids = ids;
    return count;
}

/**
 * Read a quest's prerequisites.
 *
 * "require_quest" is sugar for a one-entry "require_quests_all", which is what
 * a plain chain step wants and what reads best in the data file.
 */
static void parse_prerequisites(const JsonValue* obj, QuestDef* quest) {
    quest->require_all_count = parse_quest_id_list(obj, "require_quests_all",
                                                   &quest->require_all);
    quest->require_any_count = parse_quest_id_list(obj, "require_quests_any",
                                                   &quest->require_any);

    int single = json_get_int(obj, "require_quest", 0);
    if (single > 0 && quest->require_all_count == 0) {
        quest->require_all = calloc(1, sizeof(*quest->require_all));
        if (quest->require_all) {
            quest->require_all[0]    = (uint32_t)single;
            quest->require_all_count = 1;
        }
    }
}

/**
 * Build one quest definition from a parsed JSON object.
 *
 * @return An owned definition, or NULL when it carries no usable identifier.
 */
static QuestDef* parse_quest(const JsonValue* obj) {
    int id = json_get_int(obj, "quest_id", 0);
    if (id <= 0) {
        LOG_ERROR("[QUEST] a quest object has no positive \"quest_id\"");
        return NULL;
    }

    QuestDef* quest = calloc(1, sizeof(*quest));
    if (!quest) return NULL;

    quest->quest_id = (uint32_t)id;
    STR_COPY_FIELD(quest->title, json_get_string(obj, "title", "Untitled"));

    quest->xp_reward       = (uint32_t)json_get_int(obj, "xp", 0);
    quest->currency_reward = (uint32_t)json_get_int(obj, "currency_reward", 0);

    /* "currency" names the paying kingdom by CurrencyId; a quest that omits it
     * pays in Ennara's coin, the starting city's.
     *
     * An out-of-range one is kept rather than quietly turned into Ennara's. It
     * used to be substituted, which meant an authoring mistake became a quest
     * that paid the wrong kingdom's coin and said nothing about it. Kept, the
     * content validator fails the build on it; and if one ever does reach a
     * running server, world_currency_credit() refuses it, so the reward pays
     * nothing loudly instead of the wrong thing silently. */
    int currency = json_get_int(obj, "currency", (int)CURRENCY_ENNARA);
    if (!world_currency_valid(currency)) {
        LOG_ERROR("[QUEST] quest %u pays in currency %d, which no kingdom mints",
                  quest->quest_id, currency);
        if (currency < 0 || currency > UINT8_MAX) currency = UINT8_MAX;
    }
    quest->currency_id = (uint8_t)currency;

    /* Absent means once, which is what every story quest already says, so no
     * existing quest file needs an edit to keep behaving as it does. */
    const char* repeat_key = json_get_string(obj, "repeat", NULL);
    quest->repeat_mode = QUEST_REPEAT_ONCE;
    if (repeat_key) {
        QuestRepeatMode mode = quest_repeat_mode_from_key(repeat_key);
        if (mode == QUEST_REPEAT_MODE_COUNT) {
            LOG_ERROR("[QUEST] quest %u has unknown \"repeat\" mode '%s'; "
                      "treating it as once-only", quest->quest_id, repeat_key);
        } else {
            quest->repeat_mode = mode;
        }
    }

    int max_count = json_get_int(obj, "max_count", 0);
    if (max_count < 0 || max_count > UINT16_MAX) {
        LOG_ERROR("[QUEST] quest %u has a \"max_count\" of %d, which is outside "
                  "1..%d; treating it as uncapped", quest->quest_id, max_count, UINT16_MAX);
        max_count = 0;
    }
    quest->max_count = (uint16_t)max_count;

    if (quest->max_count > 0 && quest->repeat_mode == QUEST_REPEAT_ONCE)
        LOG_ERROR("[QUEST] quest %u caps itself at %u turn-ins but does not repeat; "
                  "the cap has no effect", quest->quest_id, quest->max_count);

    quest->require_level = json_get_int(obj, "require_level", 0);
    parse_prerequisites(obj, quest);

    /* Races are named by key so a quest file never hardcodes a numeric id. */
    const char* race_key = json_get_string(obj, "require_race", NULL);
    if (race_key) {
        const RaceDef* race = race_get_by_key(race_key);
        if (race) {
            quest->require_race_id = race->id;
        } else {
            LOG_ERROR("[QUEST] quest %u requires race '%s', which is not in races.json; "
                      "no character will be able to take it", quest->quest_id, race_key);
            quest->require_race_id = UINT32_MAX;
        }
    }

    parse_objectives(obj, quest);
    parse_item_rewards(obj, quest);
    return quest;
}

/**
 * Install a parsed quest, replacing any quest with the same identifier.
 */
/** Release one definition and the lists it owns. */
static void quest_def_free(QuestDef* quest) {
    if (!quest) return;
    free(quest->require_all);
    free(quest->require_any);
    free(quest);
}

static void registry_install(QuestDef* quest) {
    if (!registry_reserve(quest->quest_id)) {
        LOG_ERROR("[QUEST] Cannot store quest %u", quest->quest_id);
        quest_def_free(quest);
        return;
    }

    QuestDef** slot = &g_quests[quest->quest_id];
    if (*slot) {
        LOG_ERROR("[QUEST] quest id %u is defined more than once; replacing '%s' with '%s'",
                  quest->quest_id, (*slot)->title, quest->title);
        quest_def_free(*slot);
        g_quest_count--;
    }

    *slot = quest;
    g_quest_count++;
    LOG_INFO("[QUEST] Loaded quest %u '%s' (%u objectives)",
             quest->quest_id, quest->title, quest->obj_count);
}

int quest_registry_load(const char* json_path) {
    quest_registry_clear();

    const char* err = NULL;
    JsonValue* root = json_parse_file(json_path, &err);
    if (!root) {
        /* Nonfatal: a world with no quest file is a world with no quests. */
        LOG_ERROR("[QUEST] Cannot load %s: %s", json_path, err ? err : "unreadable");
        return 1;
    }

    const JsonValue* list = json_get(root, "quests");
    if (json_type(list) != JSON_ARRAY) {
        LOG_ERROR("[QUEST] %s has no \"quests\" array", json_path);
        json_free(root);
        return 1;
    }

    int authored = json_count(list);
    for (int i = 0; i < authored; i++) {
        QuestDef* quest = parse_quest(json_at(list, i));
        if (quest) registry_install(quest);
    }

    json_free(root);
    LOG_INFO("[QUEST] %d quests loaded from %s", g_quest_count, json_path);
    return 1;
}

void quest_registry_clear(void) {
    for (int i = 0; i < g_quest_cap; i++) {
        quest_def_free(g_quests[i]);
        g_quests[i] = NULL;
    }
    free(g_quests);
    g_quests      = NULL;
    g_quest_cap   = 0;
    g_quest_count = 0;
}
