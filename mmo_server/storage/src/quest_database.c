// Store one character's quests in PostgreSQL, and read the old quest files.

#include "quest_database.h"
#include "log.h"

#include <libpq-fe.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// text size of one decimal 32-bit value plus a separator
#define NUMBER_TEXT_SIZE 13

// run one statement with text parameters, return 1 when it succeeded
static int exec_params(PGconn* conn, const char* sql, int count,
                       const char* const* values) {
	PGresult* res = PQexecParams(conn, sql, count, NULL, values, NULL, NULL, 0);
	ExecStatusType status = PQresultStatus(res);
	int ok = (status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK);

	if(!ok)
		LOG_ERROR("[QUEST] statement failed: %s", PQerrorMessage(conn));

	PQclear(res);
	return ok;
}

// build a PostgreSQL array literal from a slot's progress counters
static void progress_text(const struct PlayerQuestSlot* slot, char* out,
                          size_t size) {
	size_t used = (size_t)snprintf(out, size, "{");

	for(int i = 0; i < MAX_QUEST_OBJECTIVES && used < size; i++)
		used += (size_t)snprintf(out + used, size - used, "%s%d", i ? "," : "",
		                         (int)slot->progress[i]);

	if(used < size)
		snprintf(out + used, size - used, "}");
}

// write the log, replacing whatever the database held for the character
static int save_log(PGconn* conn, const char* id_text, const PlayerQuestLog* log) {
	const char* clear_values[1] = {id_text};

	if(!exec_params(conn, "DELETE FROM character_quest_log WHERE character_id = $1",
	                1, clear_values))
		return 0;

	for(int i = 0; i < log->count; i++) {
		const struct PlayerQuestSlot* slot = &log->slots[i];
		char quest_text[NUMBER_TEXT_SIZE];
		char progress[MAX_QUEST_OBJECTIVES * NUMBER_TEXT_SIZE + 3];
		char step_text[NUMBER_TEXT_SIZE];
		const char* values[6];

		snprintf(quest_text, sizeof(quest_text), "%u", slot->quest_id);
		snprintf(step_text, sizeof(step_text), "%u", (unsigned)slot->step);
		progress_text(slot, progress, sizeof(progress));

		values[0] = id_text;
		values[1] = quest_text;
		values[2] = slot->is_active ? "t" : "f";
		values[3] = slot->is_complete ? "t" : "f";
		values[4] = progress;
		values[5] = step_text;

		if(!exec_params(conn,
		                "INSERT INTO character_quest_log (character_id, quest_id, "
		                "is_active, is_complete, progress, step) "
		                "VALUES ($1, $2, $3, $4, $5::integer[], $6)",
		                6, values))
			return 0;
	}

	return 1;
}

// add every finished quest, which only ever grows so existing rows are kept
static int save_history(PGconn* conn, const char* id_text,
                        const QuestHistory* history) {
	char* list;
	size_t used = 0;
	int ok;

	if(history->count <= 0 || !history->ids)
		return 1;

	list = malloc((size_t)history->count * NUMBER_TEXT_SIZE + 3);

	if(!list) {
		LOG_ERROR("[QUEST] out of memory building the completion history");
		return 0;
	}

	used += (size_t)sprintf(list, "{");

	for(uint32_t i = 0; i <= history->mask; i++) {
		if(history->ids[i] == 0)
			continue;

		used += (size_t)sprintf(list + used, "%s%u", used > 1 ? "," : "",
		                        history->ids[i]);
	}

	sprintf(list + used, "}");

	const char* values[2] = {id_text, list};
	ok = exec_params(conn,
	                 "INSERT INTO character_quest_history (character_id, quest_id) "
	                 "SELECT $1, unnest($2::bigint[]) ON CONFLICT DO NOTHING",
	                 2, values);

	free(list);
	return ok;
}

// write one row per repeatable quest the character has turned in
static int save_counters(PGconn* conn, const char* id_text,
                         const QuestCounters* counters) {
	if(!counters->entries)
		return 1;

	for(uint32_t i = 0; i <= counters->mask; i++) {
		const QuestCounter* entry = &counters->entries[i];
		char quest_text[NUMBER_TEXT_SIZE];
		char count_text[NUMBER_TEXT_SIZE];
		char window_text[NUMBER_TEXT_SIZE];
		const char* values[4];

		if(entry->quest_id == 0)
			continue;

		snprintf(quest_text, sizeof(quest_text), "%u", entry->quest_id);
		snprintf(count_text, sizeof(count_text), "%u", (unsigned)entry->count);
		snprintf(window_text, sizeof(window_text), "%u", (unsigned)entry->window);

		values[0] = id_text;
		values[1] = quest_text;
		values[2] = count_text;
		values[3] = window_text;

		if(!exec_params(conn,
		                "INSERT INTO character_quest_counters (character_id, "
		                "quest_id, turn_ins, reset_window) VALUES ($1, $2, $3, $4) "
		                "ON CONFLICT (character_id, quest_id) DO UPDATE SET "
		                "turn_ins = EXCLUDED.turn_ins, "
		                "reset_window = EXCLUDED.reset_window",
		                4, values))
			return 0;
	}

	return 1;
}

int quest_database_save_tx(PGconn* conn, uint32_t character_id,
                           const PlayerQuestState* state) {
	char id_text[NUMBER_TEXT_SIZE];
	const char* id_values[1];

	if(!conn || !state)
		return 0;

	snprintf(id_text, sizeof(id_text), "%u", character_id);
	id_values[0] = id_text;

	// the marker row is what tells a load that the database owns these quests
	if(!exec_params(conn,
	                "INSERT INTO character_quest_state (character_id) VALUES ($1) "
	                "ON CONFLICT (character_id) DO UPDATE SET "
	                "saved_at = CURRENT_TIMESTAMP",
	                1, id_values))
		return 0;

	return save_log(conn, id_text, &state->log) &&
	       save_history(conn, id_text, &state->history) &&
	       save_counters(conn, id_text, &state->counters);
}

// read progress counters out of an array literal such as {1,0,2,0}
static void parse_progress(const char* text, struct PlayerQuestSlot* slot) {
	const char* cursor = text;

	for(int i = 0; i < MAX_QUEST_OBJECTIVES && cursor && *cursor; i++) {
		char* end;

		while(*cursor == '{' || *cursor == ',')
			cursor++;

		slot->progress[i] = (int32_t)strtol(cursor, &end, 10);

		if(end == cursor)
			break;

		cursor = end;
	}
}

// run a query that selects rows for one character
static PGresult* select_rows(PGconn* conn, const char* sql, const char* id_text) {
	const char* values[1] = {id_text};
	PGresult* res = PQexecParams(conn, sql, 1, NULL, values, NULL, NULL, 0);

	if(PQresultStatus(res) != PGRES_TUPLES_OK) {
		LOG_ERROR("[QUEST] query failed: %s", PQerrorMessage(conn));
		PQclear(res);
		return NULL;
	}

	return res;
}

static int load_log(PGconn* conn, const char* id_text, quest_records* out) {
	PGresult* res = select_rows(conn,
	                            "SELECT quest_id, is_active::int, is_complete::int, "
	                            "progress, step FROM character_quest_log "
	                            "WHERE character_id = $1 ORDER BY quest_id",
	                            id_text);
	int rows;

	if(!res)
		return 0;

	rows = PQntuples(res);

	if(rows > 0) {
		out->slots = calloc((size_t)rows, sizeof(out->slots[0]));

		if(!out->slots) {
			PQclear(res);
			return 0;
		}
	}

	for(int i = 0; i < rows; i++) {
		struct PlayerQuestSlot* slot = &out->slots[i];

		slot->quest_id = (uint32_t)strtoul(PQgetvalue(res, i, 0), NULL, 10);
		slot->is_active = (uint8_t)atoi(PQgetvalue(res, i, 1));
		slot->is_complete = (uint8_t)atoi(PQgetvalue(res, i, 2));
		parse_progress(PQgetvalue(res, i, 3), slot);
		slot->step = (uint8_t)atoi(PQgetvalue(res, i, 4));
	}

	out->slot_count = rows;
	PQclear(res);
	return 1;
}

static int load_history(PGconn* conn, const char* id_text, quest_records* out) {
	PGresult* res = select_rows(conn,
	                            "SELECT quest_id FROM character_quest_history "
	                            "WHERE character_id = $1",
	                            id_text);
	int rows;

	if(!res)
		return 0;

	rows = PQntuples(res);

	if(rows > 0) {
		out->history = calloc((size_t)rows, sizeof(out->history[0]));

		if(!out->history) {
			PQclear(res);
			return 0;
		}
	}

	for(int i = 0; i < rows; i++)
		out->history[i] = (uint32_t)strtoul(PQgetvalue(res, i, 0), NULL, 10);

	out->history_count = rows;
	PQclear(res);
	return 1;
}

static int load_counters(PGconn* conn, const char* id_text, quest_records* out) {
	PGresult* res = select_rows(conn,
	                            "SELECT quest_id, turn_ins, reset_window "
	                            "FROM character_quest_counters "
	                            "WHERE character_id = $1",
	                            id_text);
	int rows;

	if(!res)
		return 0;

	rows = PQntuples(res);

	if(rows > 0) {
		out->counters = calloc((size_t)rows, sizeof(out->counters[0]));

		if(!out->counters) {
			PQclear(res);
			return 0;
		}
	}

	for(int i = 0; i < rows; i++) {
		unsigned long turn_ins = strtoul(PQgetvalue(res, i, 1), NULL, 10);
		unsigned long window = strtoul(PQgetvalue(res, i, 2), NULL, 10);

		out->counters[i].quest_id = (uint32_t)strtoul(PQgetvalue(res, i, 0), NULL, 10);
		out->counters[i].count = (uint16_t)(turn_ins > UINT16_MAX ? UINT16_MAX : turn_ins);
		out->counters[i].window = (uint16_t)(window > UINT16_MAX ? UINT16_MAX : window);
	}

	out->counter_count = rows;
	PQclear(res);
	return 1;
}

int quest_database_load(PGconn* conn, uint32_t character_id, quest_records* out) {
	char id_text[NUMBER_TEXT_SIZE];
	PGresult* marker;
	int found;

	if(!conn || !out)
		return -1;

	memset(out, 0, sizeof(*out));
	snprintf(id_text, sizeof(id_text), "%u", character_id);

	marker = select_rows(conn,
	                     "SELECT 1 FROM character_quest_state WHERE character_id = $1",
	                     id_text);

	if(!marker)
		return -1;

	found = PQntuples(marker) > 0;
	PQclear(marker);

	// no marker means the quests were never saved here, so the caller falls back
	if(!found)
		return 0;

	if(!load_log(conn, id_text, out) || !load_history(conn, id_text, out) ||
	   !load_counters(conn, id_text, out)) {
		quest_records_free(out);
		return -1;
	}

	return 1;
}
