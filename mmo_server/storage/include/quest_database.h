#ifndef QUEST_DATABASE_H
#define QUEST_DATABASE_H

// Store one character's quests in PostgreSQL, and read the old quest files.
//
// The game keeps quests in memory as a PlayerQuestState. This module only moves
// them to and from the database as flat arrays, so it never calls game code.

#include "server_types.h"

#include <stdint.h>

struct pg_conn;

// flat copy of what is stored for one character, owned by the caller
typedef struct {
	struct PlayerQuestSlot* slots;
	int slot_count;
	uint32_t* history;
	int history_count;
	QuestCounter* counters;
	int counter_count;
} quest_records;

// free the arrays in the records and leave them empty
void quest_records_free(quest_records* records);

// write the character's quests inside the caller's transaction, 1 on success
int quest_database_save_tx(struct pg_conn* conn, uint32_t character_id,
                           const PlayerQuestState* state);

// read the character's quests: 1 when found, 0 when none stored, -1 on failure
int quest_database_load(struct pg_conn* conn, uint32_t character_id,
                        quest_records* out);

// set the folder that held quest files before quests moved to the database
void quest_legacy_set_dir(const char* dir);

// read a quest file written by an older build: 1 when read, 0 when absent or bad
int quest_legacy_load(uint32_t character_id, quest_records* out);

#endif /* QUEST_DATABASE_H */
