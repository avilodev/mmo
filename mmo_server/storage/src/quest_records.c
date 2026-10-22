// Release the flat quest records the storage layer hands to the game.

#include "quest_database.h"

#include <stdlib.h>
#include <string.h>

void quest_records_free(quest_records* records) {
	if(!records)
		return;

	free(records->slots);
	free(records->history);
	free(records->counters);
	memset(records, 0, sizeof(*records));
}
