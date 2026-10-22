// Read the per-character quest files that older builds wrote.
//
// Quests now live in PostgreSQL. These files are only read, once, for a
// character whose quests have never been saved to the database. Nothing here
// writes or deletes a file, so the old data stays where it was.

#include "quest_database.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// the magic that opens every file from version 2 on, "MASQ" little-endian
#define QUEST_FILE_MAGIC 0x5153414DU

// newest and oldest magic-bearing versions this build reads
#define QUEST_FILE_VERSION 3u
#define QUEST_FILE_MIN_VERSION 2u

// refuse a file claiming more entries than any character could hold
#define QUEST_FILE_SANE_MAX 1000000u

_Static_assert(sizeof(QuestCounter) == 8,
               "a counter is read from the file verbatim and must stay 8 bytes");

static char g_legacy_dir[512] = {0};

void quest_legacy_set_dir(const char* dir) {
	snprintf(g_legacy_dir, sizeof(g_legacy_dir), "%s", dir ? dir : "");
}

// grow an array by one element and return the new slot, or NULL on failure
static void* append_slot(void** array, int* count, size_t element_size) {
	void* grown = realloc(*array, ((size_t)*count + 1) * element_size);

	if(!grown)
		return NULL;

	*array = grown;
	memset((char*)grown + (size_t)*count * element_size, 0, element_size);
	return (char*)grown + (size_t)(*count)++ * element_size;
}

// add an identifier to the history list
static int add_history(quest_records* out, uint32_t quest_id) {
	uint32_t* slot = append_slot((void**)&out->history, &out->history_count,
	                             sizeof(uint32_t));

	if(!slot)
		return 0;

	*slot = quest_id;
	return 1;
}

// read a version 1 file: one fixed array where a finished quest stayed inactive
static int read_version_one(FILE* f, quest_records* out) {
	int count = 0;

	rewind(f);

	if(fread(&count, sizeof(int), 1, f) != 1)
		return 0;

	if(count < 0 || (uint32_t)count > QUEST_FILE_SANE_MAX)
		return 0;

	for(int i = 0; i < count; i++) {
		struct PlayerQuestSlot slot;

		if(fread(&slot, sizeof(slot), 1, f) != 1)
			return 0;

		if(slot.is_active) {
			struct PlayerQuestSlot* added = append_slot((void**)&out->slots,
			                                            &out->slot_count,
			                                            sizeof(*added));

			if(!added)
				return 0;

			*added = slot;
		} else if(slot.quest_id != 0 && !add_history(out, slot.quest_id)) {
			return 0;
		}
	}

	return 1;
}

// read the header counts of a version 2 or 3 file
static int read_header(FILE* f, uint32_t character_id, uint32_t* active,
                       uint32_t* done, uint32_t* repeated) {
	uint32_t version = 0;

	if(fread(&version, sizeof(version), 1, f) != 1 ||
	   fread(active, sizeof(*active), 1, f) != 1 ||
	   fread(done, sizeof(*done), 1, f) != 1)
		return 0;

	if(version < QUEST_FILE_MIN_VERSION || version > QUEST_FILE_VERSION) {
		LOG_ERROR("[QUEST] character %u has a version %u quest file; this build "
		          "reads versions %u to %u", character_id, version,
		          QUEST_FILE_MIN_VERSION, QUEST_FILE_VERSION);
		return 0;
	}

	// version 2 had no counter section, which loads as an empty table
	if(version >= 3u && fread(repeated, sizeof(*repeated), 1, f) != 1)
		return 0;

	if(*active > QUEST_FILE_SANE_MAX || *done > QUEST_FILE_SANE_MAX ||
	   *repeated > QUEST_FILE_SANE_MAX) {
		LOG_ERROR("[QUEST] character %u has a quest file claiming %u active, "
		          "%u completed and %u repeated quests; refusing it",
		          character_id, *active, *done, *repeated);
		return 0;
	}

	return 1;
}

// read a version 2 or 3 file after its magic number
static int read_current(FILE* f, uint32_t character_id, quest_records* out) {
	uint32_t active = 0;
	uint32_t done = 0;
	uint32_t repeated = 0;

	if(!read_header(f, character_id, &active, &done, &repeated))
		return 0;

	for(uint32_t i = 0; i < active; i++) {
		struct PlayerQuestSlot* slot = append_slot((void**)&out->slots,
		                                           &out->slot_count,
		                                           sizeof(*slot));

		if(!slot || fread(slot, sizeof(*slot), 1, f) != 1)
			return 0;
	}

	for(uint32_t i = 0; i < done; i++) {
		uint32_t quest_id = 0;

		if(fread(&quest_id, sizeof(quest_id), 1, f) != 1 ||
		   !add_history(out, quest_id))
			return 0;
	}

	for(uint32_t i = 0; i < repeated; i++) {
		QuestCounter entry;
		QuestCounter* slot;

		if(fread(&entry, sizeof(entry), 1, f) != 1)
			return 0;

		// a zero identifier is never valid, so skip it rather than store it
		if(entry.quest_id == 0)
			continue;

		slot = append_slot((void**)&out->counters, &out->counter_count,
		                   sizeof(*slot));

		if(!slot)
			return 0;

		*slot = entry;
	}

	return 1;
}

int quest_legacy_load(uint32_t character_id, quest_records* out) {
	char path[600];
	uint32_t magic = 0;
	FILE* f;
	int ok;

	if(g_legacy_dir[0] == '\0' || !out)
		return 0;

	memset(out, 0, sizeof(*out));
	snprintf(path, sizeof(path), "%s/%u.bin", g_legacy_dir, character_id);

	f = fopen(path, "rb");

	if(!f)
		return 0;

	if(fread(&magic, sizeof(magic), 1, f) != 1) {
		fclose(f);
		return 0;
	}

	ok = (magic == QUEST_FILE_MAGIC) ? read_current(f, character_id, out)
	                                 : read_version_one(f, out);
	fclose(f);

	if(!ok) {
		quest_records_free(out);
		return 0;
	}

	return 1;
}
