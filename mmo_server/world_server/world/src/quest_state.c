/**
 * @file
 * Keep one character's quest tables in memory and load them from storage.
 */

#include "quest_state.h"
#include "log.h"
#include "players_database.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* --- Loading ------------------------------------------------------------- */

// rebuild a quest state from the flat records the storage layer returns
static int quest_state_from_records(PlayerQuestState* state,
                                    const quest_records* records) {
	for(int i = 0; i < records->slot_count; i++) {
		if(!quest_log_reserve_one(&state->log))
			return 0;

		state->log.slots[state->log.count++] = records->slots[i];
	}

	for(int i = 0; i < records->history_count; i++) {
		if(!history_insert(&state->history, records->history[i]))
			return 0;
	}

	for(int i = 0; i < records->counter_count; i++) {
		const QuestCounter* entry = &records->counters[i];
		QuestCounter* stored;

		if(entry->quest_id == 0)
			continue;

		if(!quest_counters_record(&state->counters, entry->quest_id, entry->window))
			return 0;

		// record() counts one turn-in, and the stored count is the whole lifetime
		stored = counters_bucket(&state->counters, entry->quest_id);
		stored->count = entry->count;
	}

	return 1;
}

int quest_player_load(uint32_t character_id, PlayerQuestState* state) {
	quest_records records;
	int found;
	int built;

	if(!state)
		return -1;

	found = character_quests_load(character_id, &records);

	if(found < 0)
		return -1;

	// never saved to the database, so read the file an older build wrote
	if(found == 0 && !quest_legacy_load(character_id, &records))
		return 0;

	built = quest_state_from_records(state, &records);
	quest_records_free(&records);

	if(!built) {
		quest_state_release(state);
		return -1;
	}

	return 1;
}
