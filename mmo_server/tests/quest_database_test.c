// Round-trip quests through a real PostgreSQL.
// Set MMO_TEST_PGCONN to run it; an unset variable skips with success.

#include "players_database.h"
#include "log.h"

#include <libpq-fe.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// character identifier reserved above the database SERIAL range
#define TEST_CHARACTER_ID 2147483602u

static PGconn* g_fixture = NULL;

// run one statement on the test's own connection
static int fixture_exec(const char* sql) {
	PGresult* res = PQexec(g_fixture, sql);
	int ok = (PQresultStatus(res) == PGRES_COMMAND_OK ||
	          PQresultStatus(res) == PGRES_TUPLES_OK);

	if(!ok)
		fprintf(stderr, "fixture: %s\n  %s\n", sql, PQerrorMessage(g_fixture));

	PQclear(res);
	return ok;
}

static void delete_test_character(void) {
	char sql[256];

	snprintf(sql, sizeof(sql),
	         "DELETE FROM characters WHERE character_id = %u "
	         "   OR (name = 'quest_db_test' AND world_id = 1);",
	         TEST_CHARACTER_ID);
	assert(fixture_exec(sql));
}

static void create_test_character(void) {
	char sql[512];

	snprintf(sql, sizeof(sql),
	         "INSERT INTO characters (character_id, account_id, world_id, name, "
	         "                        class_id, race_id) "
	         "VALUES (%u, 1, 1, 'quest_db_test', 1, 1);",
	         TEST_CHARACTER_ID);
	assert(fixture_exec(sql));
}

// build a state whose tables are plain arrays, the way the game holds them
static void build_state(PlayerQuestState* state, int with_first_quest,
                        uint32_t history_size) {
	int slots = with_first_quest ? 2 : 1;

	memset(state, 0, sizeof(*state));

	state->log.slots = calloc((size_t)slots, sizeof(state->log.slots[0]));
	state->log.count = slots;
	state->log.capacity = slots;

	if(with_first_quest) {
		state->log.slots[0].quest_id = 10;
		state->log.slots[0].is_active = 1;
		state->log.slots[0].progress[0] = 4;
	}

	state->log.slots[slots - 1].quest_id = 20;
	state->log.slots[slots - 1].is_active = 1;
	state->log.slots[slots - 1].is_complete = 1;
	state->log.slots[slots - 1].step = 6;
	state->log.slots[slots - 1].progress[0] = 7;
	state->log.slots[slots - 1].progress[1] = 1;
	state->log.slots[slots - 1].progress[3] = 2;

	state->history.mask = history_size - 1;
	state->history.ids = calloc(history_size, sizeof(uint32_t));
	state->history.count = (int)(history_size - 1);

	// one bucket stays empty, which is how the set marks free space
	for(uint32_t i = 0; i + 1 < history_size; i++)
		state->history.ids[i] = 100 + i;

	state->counters.mask = 3;
	state->counters.entries = calloc(4, sizeof(QuestCounter));
	state->counters.count = 2;
	state->counters.entries[0].quest_id = 11;
	state->counters.entries[0].count = with_first_quest ? 3 : 4;
	state->counters.entries[0].window = 700;
	state->counters.entries[1].quest_id = 12;
	state->counters.entries[1].count = 1;
	state->counters.entries[1].window = 42;
}

static void release_state(PlayerQuestState* state) {
	free(state->log.slots);
	free(state->history.ids);
	free(state->counters.entries);
	memset(state, 0, sizeof(*state));
}

// save the character with the given quests, returning 1 on success
static int save_with(const PlayerQuestState* quests) {
	character_record info;
	ItemInstance inventory[INVENTORY_SLOTS];
	ItemInstance equipment[EQUIP_SLOTS];

	memset(&info, 0, sizeof(info));
	memset(inventory, 0, sizeof(inventory));
	memset(equipment, 0, sizeof(equipment));

	assert(character_get_full_data(TEST_CHARACTER_ID, &info));

	return character_save_all(&info, inventory, INVENTORY_SLOTS, equipment,
	                          EQUIP_SLOTS, quests);
}

static const struct PlayerQuestSlot* find_slot(const quest_records* records,
                                               uint32_t quest_id) {
	for(int i = 0; i < records->slot_count; i++)
		if(records->slots[i].quest_id == quest_id)
			return &records->slots[i];

	return NULL;
}

static int has_history(const quest_records* records, uint32_t quest_id) {
	for(int i = 0; i < records->history_count; i++)
		if(records->history[i] == quest_id)
			return 1;

	return 0;
}

static const QuestCounter* find_counter(const quest_records* records,
                                        uint32_t quest_id) {
	for(int i = 0; i < records->counter_count; i++)
		if(records->counters[i].quest_id == quest_id)
			return &records->counters[i];

	return NULL;
}

int main(void) {
	const char* conn = getenv("MMO_TEST_PGCONN");
	quest_records records;
	PlayerQuestState state;

	log_init();
	log_set_level(LOG_LEVEL_ERROR);

	if(!conn || !*conn) {
		printf("SKIPPED: set MMO_TEST_PGCONN to run the quest database test\n");
		return 0;
	}

	if(!character_database_init(conn)) {
		fprintf(stderr, "FAILED: could not connect using MMO_TEST_PGCONN\n");
		return 1;
	}

	g_fixture = PQconnectdb(conn);
	assert(PQstatus(g_fixture) == CONNECTION_OK);

	delete_test_character();
	create_test_character();

	printf("TEST 1: a character whose quests were never saved loads as none\n");
	assert(character_quests_load(TEST_CHARACTER_ID, &records) == 0);
	assert(records.slot_count == 0 && records.history_count == 0);

	printf("TEST 2: a save and load returns the same quests\n");
	build_state(&state, 1, 8);
	assert(save_with(&state));
	assert(character_quests_load(TEST_CHARACTER_ID, &records) == 1);
	assert(records.slot_count == 2);
	assert(find_slot(&records, 10) && find_slot(&records, 10)->progress[0] == 4);
	assert(find_slot(&records, 10)->is_complete == 0);
	assert(find_slot(&records, 20)->is_complete == 1);
	assert(find_slot(&records, 20)->progress[0] == 7);
	assert(find_slot(&records, 20)->progress[1] == 1);
	assert(find_slot(&records, 20)->progress[2] == 0);
	assert(find_slot(&records, 20)->progress[3] == 2);
	assert(find_slot(&records, 20)->step == 6);
	assert(find_slot(&records, 10)->step == 0);
	assert(records.history_count == 7);
	assert(has_history(&records, 100) && has_history(&records, 106));
	assert(records.counter_count == 2);
	assert(find_counter(&records, 11)->count == 3);
	assert(find_counter(&records, 11)->window == 700);
	assert(find_counter(&records, 12)->count == 1);
	quest_records_free(&records);
	release_state(&state);

	printf("TEST 3: a handed-in quest leaves the log and history only grows\n");
	build_state(&state, 0, 16);
	assert(save_with(&state));
	assert(character_quests_load(TEST_CHARACTER_ID, &records) == 1);
	assert(records.slot_count == 1);
	assert(find_slot(&records, 10) == NULL);
	assert(find_slot(&records, 20) != NULL);
	assert(records.history_count == 15);
	assert(has_history(&records, 114));
	assert(find_counter(&records, 11)->count == 4);
	quest_records_free(&records);
	release_state(&state);

	printf("TEST 4: a save with no quests leaves the stored ones alone\n");
	assert(save_with(NULL));
	assert(character_quests_load(TEST_CHARACTER_ID, &records) == 1);
	assert(records.slot_count == 1 && records.history_count == 15);
	quest_records_free(&records);

	printf("TEST 5: a long history saves in one statement\n");
	build_state(&state, 1, 8192);
	assert(save_with(&state));
	assert(character_quests_load(TEST_CHARACTER_ID, &records) == 1);
	assert(records.history_count == 8191);
	quest_records_free(&records);
	release_state(&state);

	printf("TEST 6: deleting the character takes its quests with it\n");
	delete_test_character();
	assert(character_quests_load(TEST_CHARACTER_ID, &records) == 0);

	PQfinish(g_fixture);
	character_database_close();
	printf("\nALL ASSERTIONS PASSED\n");
	return 0;
}
