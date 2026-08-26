/**
 * @file
 * Keep one character's quest tables in memory and move them to and from disk.
 */

#include "quest_storage.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>

/* --- The active log ------------------------------------------------------ */

int quest_log_reserve_one(PlayerQuestLog* log) {
    if (log->count < log->capacity) return 1;

    int wanted = log->capacity ? log->capacity * 2 : 8;
    struct PlayerQuestSlot* grown = realloc(log->slots,
                                            (size_t)wanted * sizeof(*grown));
    if (!grown) return 0;

    memset(grown + log->capacity, 0,
           (size_t)(wanted - log->capacity) * sizeof(*grown));
    log->slots    = grown;
    log->capacity = wanted;
    return 1;
}

struct PlayerQuestSlot* quest_log_find(PlayerQuestLog* log, uint32_t quest_id) {
    if (!log) return NULL;
    for (int i = 0; i < log->count; i++)
        if (log->slots[i].quest_id == quest_id) return &log->slots[i];
    return NULL;
}

/**
 * Drop an entry from the active log.
 *
 * The last entry moves into the gap: order carries no meaning here, and a
 * memmove per hand-in would cost something for nothing.
 */
static void log_remove_at(PlayerQuestLog* log, int index) {
    if (index < 0 || index >= log->count) return;
    log->slots[index] = log->slots[log->count - 1];
    memset(&log->slots[log->count - 1], 0, sizeof(log->slots[0]));
    log->count--;
}

int quest_log_remove(PlayerQuestLog* log, uint32_t quest_id) {
    struct PlayerQuestSlot* slot = quest_log_find(log, quest_id);
    if (!slot) return 0;
    log_remove_at(log, (int)(slot - log->slots));
    return 1;
}

/* --- The completion history ---------------------------------------------- */

/** Scatter a quest identifier across buckets.
 *
 * Shared by the history set and the counter table: both are keyed by quest id,
 * both are open-addressed, and both would otherwise cluster on the sequential
 * numbering quest files naturally use.
 */
static inline uint32_t quest_id_hash(uint32_t key) {
    key ^= key >> 16;
    key *= 0x7feb352dU;
    key ^= key >> 15;
    key *= 0x846ca68bU;
    key ^= key >> 16;
    return key;
}

static int history_insert(QuestHistory* history, uint32_t quest_id);

/**
 * Double the history set and reinsert everything.
 *
 * @return 1 on success, or 0 when the allocation fails.
 */
static int history_grow(QuestHistory* history) {
    uint32_t old_capacity = history->ids ? history->mask + 1 : 0;
    uint32_t new_capacity = old_capacity ? old_capacity * 2 : 32;

    uint32_t* grown = calloc(new_capacity, sizeof(*grown));
    if (!grown) return 0;

    uint32_t* old_ids = history->ids;
    history->ids   = grown;
    history->mask  = new_capacity - 1;
    history->count = 0;

    for (uint32_t i = 0; i < old_capacity; i++)
        if (old_ids[i] != 0) history_insert(history, old_ids[i]);

    free(old_ids);
    return 1;
}

/**
 * Record a finished quest.
 *
 * @return 1 when the set holds the identifier afterwards, or 0 on allocation failure.
 */
static int history_insert(QuestHistory* history, uint32_t quest_id) {
    if (!history || quest_id == 0) return 0;

    /* Kept under half full so probes stay short. */
    if (!history->ids || (history->count + 1) * 2 > (int)(history->mask + 1)) {
        if (!history_grow(history)) return 0;
    }

    uint32_t pos = quest_id_hash(quest_id) & history->mask;
    for (uint32_t probe = 0; probe <= history->mask; probe++) {
        if (history->ids[pos] == quest_id) return 1;   /* already recorded */
        if (history->ids[pos] == 0) {
            history->ids[pos] = quest_id;
            history->count++;
            return 1;
        }
        pos = (pos + 1) & history->mask;
    }
    return 0;
}

int quest_history_add(QuestHistory* history, uint32_t quest_id) {
    return history_insert(history, quest_id);
}

int quest_history_contains(const QuestHistory* history, uint32_t quest_id) {
    if (!history || !history->ids || quest_id == 0) return 0;

    uint32_t pos = quest_id_hash(quest_id) & history->mask;
    for (uint32_t probe = 0; probe <= history->mask; probe++) {
        if (history->ids[pos] == 0) return 0;
        if (history->ids[pos] == quest_id) return 1;
        pos = (pos + 1) & history->mask;
    }
    return 0;
}

int quest_history_count(const QuestHistory* history) {
    return history ? history->count : 0;
}

/* --- Repeat counters ----------------------------------------------------- */

/**
 * Locate a quest's bucket, whether or not it is occupied.
 *
 * @return The bucket to read or claim, or NULL when the table is full or absent.
 */
static QuestCounter* counters_bucket(const QuestCounters* counters, uint32_t quest_id) {
    if (!counters->entries) return NULL;

    uint32_t pos = quest_id_hash(quest_id) & counters->mask;
    for (uint32_t probe = 0; probe <= counters->mask; probe++) {
        QuestCounter* entry = &counters->entries[pos];
        if (entry->quest_id == quest_id || entry->quest_id == 0) return entry;
        pos = (pos + 1) & counters->mask;
    }
    return NULL;
}

/**
 * Double the counter table and reinsert everything.
 *
 * @return 1 on success, or 0 when the allocation fails.
 */
static int counters_grow(QuestCounters* counters) {
    uint32_t old_capacity = counters->entries ? counters->mask + 1 : 0;
    uint32_t new_capacity = old_capacity ? old_capacity * 2 : 8;

    QuestCounter* grown = calloc(new_capacity, sizeof(*grown));
    if (!grown) return 0;

    QuestCounter* old_entries = counters->entries;
    counters->entries = grown;
    counters->mask    = new_capacity - 1;

    for (uint32_t i = 0; i < old_capacity; i++) {
        if (old_entries[i].quest_id == 0) continue;
        QuestCounter* bucket = counters_bucket(counters, old_entries[i].quest_id);
        *bucket = old_entries[i];
    }

    free(old_entries);
    return 1;
}

const QuestCounter* quest_counters_find(const QuestCounters* counters, uint32_t quest_id) {
    if (!counters || quest_id == 0) return NULL;

    const QuestCounter* bucket = counters_bucket(counters, quest_id);
    return (bucket && bucket->quest_id == quest_id) ? bucket : NULL;
}

int quest_counters_record(QuestCounters* counters, uint32_t quest_id, uint16_t window) {
    if (!counters || quest_id == 0) return 0;

    /* Kept under half full so probes stay short, as the history set is. */
    if (!counters->entries || (counters->count + 1) * 2 > (int)(counters->mask + 1)) {
        if (!counters_grow(counters)) return 0;
    }

    QuestCounter* bucket = counters_bucket(counters, quest_id);
    if (!bucket) return 0;

    if (bucket->quest_id == 0) {
        bucket->quest_id = quest_id;
        bucket->count    = 0;
        counters->count++;
    }

    /* Saturates: a wrapped count would silently reopen a capped quest. */
    if (bucket->count < UINT16_MAX) bucket->count++;
    bucket->window = window;
    return 1;
}

int quest_counters_count(const QuestCounters* counters) {
    return counters ? counters->count : 0;
}

void quest_counters_release(QuestCounters* counters) {
    if (!counters) return;
    free(counters->entries);
    counters->entries = NULL;
    counters->mask    = 0;
    counters->count   = 0;
}

/* --- Lifetime ------------------------------------------------------------ */

void quest_state_release(PlayerQuestState* state) {
    if (!state) return;

    free(state->log.slots);
    state->log.slots    = NULL;
    state->log.count    = 0;
    state->log.capacity = 0;

    free(state->history.ids);
    state->history.ids   = NULL;
    state->history.mask  = 0;
    state->history.count = 0;

    quest_counters_release(&state->counters);
}

int quest_state_copy(PlayerQuestState* out, const PlayerQuestState* src) {
    if (!out || !src) return 0;
    memset(out, 0, sizeof(*out));

    if (src->log.count > 0) {
        size_t bytes = (size_t)src->log.count * sizeof(*out->log.slots);
        out->log.slots = malloc(bytes);
        if (!out->log.slots) { quest_state_release(out); return 0; }
        memcpy(out->log.slots, src->log.slots, bytes);
        out->log.count    = src->log.count;
        out->log.capacity = src->log.count;
    }

    /* Reinserted rather than memcpy'd: the set's bucket layout is an
     * implementation detail even between two copies of it. */
    for (uint32_t i = 0; src->history.ids && i <= src->history.mask; i++) {
        if (src->history.ids[i] == 0) continue;
        if (!history_insert(&out->history, src->history.ids[i])) {
            quest_state_release(out);
            return 0;
        }
    }

    for (uint32_t i = 0; src->counters.entries && i <= src->counters.mask; i++) {
        const QuestCounter* entry = &src->counters.entries[i];
        if (entry->quest_id == 0) continue;
        if (!quest_counters_record(&out->counters, entry->quest_id, entry->window)) {
            quest_state_release(out);
            return 0;
        }
        /* record() counts one turn-in; the copy carries the whole lifetime. */
        QuestCounter* copied = counters_bucket(&out->counters, entry->quest_id);
        copied->count = entry->count;
    }

    return 1;
}

/* --- Recording a turn-in ------------------------------------------------- */

int quest_state_record_turnin(PlayerQuestState* state, uint32_t quest_id,
                              QuestRepeatMode mode, time_t now) {
    if (!state || quest_id == 0) return 0;

    if (!history_insert(&state->history, quest_id)) return 0;
    if (mode == QUEST_REPEAT_ONCE) return 1;

    return quest_counters_record(&state->counters, quest_id,
                                 quest_repeat_window_of(mode, now));
}

/* --- Persistence --------------------------------------------------------- */

/**
 * Identify the quest-state file format.
 *
 * Version 1 had no magic: it opened with a native int count and held one fixed
 * array in which a turned-in quest stayed behind as an inactive record. A file
 * that does not start with this magic is read as version 1 and those records
 * become history, which is what they always meant.
 *
 * Version 2 is version 3 without the counter section and without its count in
 * the header. It is read as-is and loads with an empty counter table, which is
 * exactly correct: nobody who wrote one had a repeatable quest. There is no
 * rewrite pass over existing characters -- the next save writes version 3.
 *
 * The history is written as a plain list of identifiers in every version, so
 * the set's bucket layout has never been part of the format and the section
 * did not change shape when the counters arrived.
 */
#define QUEST_FILE_MAGIC   0x5153414DU   /* "MASQ" little-endian */
#define QUEST_FILE_VERSION 3u

/** Name the oldest magic-bearing version this build still reads. */
#define QUEST_FILE_MIN_VERSION 2u

/** Refuse a file claiming more entries than any character could hold.
 *
 * Not a design ceiling -- neither the log nor the history has one -- but a
 * corrupt or hostile length field must not become an allocation of that size.
 */
#define QUEST_FILE_SANE_MAX 1000000u

static char g_quest_dir[512] = {0};

/**
 * Make the quest directory's own metadata durable.
 *
 * rename() is only as durable as the directory that records it; without this
 * the file survives a power loss under its temporary name, or under no name at
 * all. Best-effort: a directory that cannot be opened is logged once by the
 * caller's failure path, not turned into a save failure, because the data is
 * already on disk by this point.
 */
static void quest_dir_sync(void) {
    int dfd = open(g_quest_dir, O_RDONLY | O_DIRECTORY);
    if (dfd < 0) return;
    (void)fsync(dfd);
    close(dfd);
}

void quest_storage_set_dir(const char* dir) {
    snprintf(g_quest_dir, sizeof(g_quest_dir), "%s", dir);
    mkdir(g_quest_dir, 0755);
}

/**
 * Atomically write a character's quests through a temporary file.
 *
 * @return 1 when the temporary file is written and renamed, or 0 on failure.
 */
int quest_player_save(uint32_t character_id, const PlayerQuestState* state) {
    if (g_quest_dir[0] == '\0' || !state) return 0;

    const PlayerQuestLog* log     = &state->log;
    const QuestHistory*   history = &state->history;

    /* The temporary name carries the writing thread and a counter, not just
     * the character id.
     *
     * A fixed "<id>.bin.tmp" is one shared file per character: two concurrent
     * saves for the same character would interleave their writes into it and
     * the second rename would publish the mixture. Today the dirty flag is
     * cleared under the slot lock, so that does not happen -- but the file
     * format's safety should not depend on a lock held somewhere else, and a
     * unique name costs nothing. */
    static _Atomic unsigned long s_temp_seq;
    char path[600], temp_path[700];
    snprintf(path, sizeof(path), "%s/%u.bin", g_quest_dir, character_id);
    snprintf(temp_path, sizeof(temp_path), "%s.%lu.%lu.tmp", path,
             (unsigned long)getpid(),
             atomic_fetch_add(&s_temp_seq, 1UL));

    FILE* f = fopen(temp_path, "wb");
    if (!f) return 0;

    const QuestCounters* counters = &state->counters;

    uint32_t magic       = QUEST_FILE_MAGIC;
    uint32_t version     = QUEST_FILE_VERSION;
    uint32_t active      = (uint32_t)log->count;
    uint32_t done        = (uint32_t)history->count;
    uint32_t repeat_used = (uint32_t)counters->count;

    /* Every length in the header, so the loader can refuse an absurd one before
     * it becomes an allocation of that size. */
    int ok = fwrite(&magic,       sizeof(magic),       1, f) == 1
          && fwrite(&version,     sizeof(version),     1, f) == 1
          && fwrite(&active,      sizeof(active),      1, f) == 1
          && fwrite(&done,        sizeof(done),        1, f) == 1
          && fwrite(&repeat_used, sizeof(repeat_used), 1, f) == 1;

    if (ok && active > 0)
        ok = fwrite(log->slots, sizeof(log->slots[0]), active, f) == active;

    /* Written as a plain list; the set is rebuilt on load, so its bucket
     * layout is never part of the format. */
    for (uint32_t i = 0; ok && history->ids && i <= history->mask; i++) {
        if (history->ids[i] == 0) continue;
        ok = fwrite(&history->ids[i], sizeof(uint32_t), 1, f) == 1;
    }

    /* Likewise a plain list, for the same reason. */
    for (uint32_t i = 0; ok && counters->entries && i <= counters->mask; i++) {
        if (counters->entries[i].quest_id == 0) continue;
        ok = fwrite(&counters->entries[i], sizeof(QuestCounter), 1, f) == 1;
    }

    /* fflush moves the bytes from the FILE buffer into the kernel; it does not
     * put them on the disk. rename() is atomic with respect to *other
     * processes*, not with respect to power loss: without an fsync of the data
     * first, a crash could leave the directory entry pointing at a file whose
     * contents were never written, which is worse than the stale file the
     * rename replaced -- a zero-length or torn quest log loads as "no quests".
     *
     * So: flush, fsync the file, close, rename, then fsync the directory to
     * make the rename itself durable. */
    if (ok) ok = fflush(f) == 0;
    if (ok) ok = fsync(fileno(f)) == 0;
    if (fclose(f) != 0) ok = 0;
    if (ok) ok = rename(temp_path, path) == 0;
    if (ok) quest_dir_sync();
    if (!ok) remove(temp_path);
    return ok;
}

/**
 * Read a version-1 file, splitting its records into a log and a history.
 *
 * @return 1 when the file was understood, or 0 otherwise.
 */
static int load_legacy(FILE* f, PlayerQuestLog* log, QuestHistory* history) {
    rewind(f);

    int count = 0;
    if (fread(&count, sizeof(int), 1, f) != 1) return 0;
    if (count < 0 || (uint32_t)count > QUEST_FILE_SANE_MAX) return 0;

    for (int i = 0; i < count; i++) {
        struct PlayerQuestSlot slot;
        if (fread(&slot, sizeof(slot), 1, f) != 1) return 0;

        if (slot.is_active) {
            if (!quest_log_reserve_one(log)) return 0;
            log->slots[log->count++] = slot;
        } else if (slot.quest_id != 0) {
            /* An inactive record was the old way of saying "finished". */
            history_insert(history, slot.quest_id);
        }
    }

    LOG_INFO("[QUEST] migrated a version 1 quest file: %d active, %d completed",
             log->count, history->count);
    return 1;
}

/**
 * Restore a character's active quests and completion history.
 *
 * @return 1 when something was loaded, or 0 when absent or unusable.
 */
int quest_player_load(uint32_t character_id, PlayerQuestState* state) {
    if (g_quest_dir[0] == '\0' || !state) return 0;

    PlayerQuestLog* log     = &state->log;
    QuestHistory*   history = &state->history;

    char path[600];
    snprintf(path, sizeof(path), "%s/%u.bin", g_quest_dir, character_id);

    FILE* f = fopen(path, "rb");
    if (!f) return 0;

    uint32_t magic = 0;
    if (fread(&magic, sizeof(magic), 1, f) != 1) { fclose(f); return 0; }

    if (magic != QUEST_FILE_MAGIC) {
        int ok = load_legacy(f, log, history);
        fclose(f);
        if (!ok) quest_state_release(state);
        return ok;
    }

    uint32_t version = 0, active = 0, done = 0, repeat_used = 0;
    int ok = fread(&version, sizeof(version), 1, f) == 1
          && fread(&active,  sizeof(active),  1, f) == 1
          && fread(&done,    sizeof(done),    1, f) == 1;

    if (ok && (version < QUEST_FILE_MIN_VERSION || version > QUEST_FILE_VERSION)) {
        LOG_ERROR("[QUEST] character %u has a version %u quest file; this build "
                  "reads versions %u to %u", character_id, version,
                  QUEST_FILE_MIN_VERSION, QUEST_FILE_VERSION);
        ok = 0;
    }

    /* Version 2 has no counter section and no count for one; it loads with an
     * empty table, which is what it always meant. */
    if (ok && version >= 3u)
        ok = fread(&repeat_used, sizeof(repeat_used), 1, f) == 1;

    if (ok && (active > QUEST_FILE_SANE_MAX || done > QUEST_FILE_SANE_MAX ||
               repeat_used > QUEST_FILE_SANE_MAX)) {
        LOG_ERROR("[QUEST] character %u has a quest file claiming %u active, "
                  "%u completed and %u repeated quests; refusing it",
                  character_id, active, done, repeat_used);
        ok = 0;
    }

    for (uint32_t i = 0; ok && i < active; i++) {
        if (!quest_log_reserve_one(log)) { ok = 0; break; }
        if (fread(&log->slots[log->count], sizeof(log->slots[0]), 1, f) != 1) {
            ok = 0;
            break;
        }
        log->count++;
    }

    for (uint32_t i = 0; ok && i < done; i++) {
        uint32_t quest_id = 0;
        if (fread(&quest_id, sizeof(quest_id), 1, f) != 1) { ok = 0; break; }
        if (!history_insert(history, quest_id)) { ok = 0; break; }
    }

    for (uint32_t i = 0; ok && i < repeat_used; i++) {
        QuestCounter entry;
        if (fread(&entry, sizeof(entry), 1, f) != 1) { ok = 0; break; }
        if (entry.quest_id == 0) continue;   /* never valid; skip rather than store */

        if (!quest_counters_record(&state->counters, entry.quest_id, entry.window)) {
            ok = 0;
            break;
        }
        /* record() counts one turn-in; the file carries the whole lifetime. */
        QuestCounter* stored = counters_bucket(&state->counters, entry.quest_id);
        stored->count = entry.count;
    }

    fclose(f);
    if (!ok) quest_state_release(state);
    return ok;
}
