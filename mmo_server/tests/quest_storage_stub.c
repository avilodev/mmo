// Stand in for the database quest store in tests that have no database.

#include "players_database.h"

#include <string.h>

// report that the database holds no quests for any character
int character_quests_load(uint32_t character_id, quest_records* out) {
	(void)character_id;

	memset(out, 0, sizeof(*out));
	return 0;
}
