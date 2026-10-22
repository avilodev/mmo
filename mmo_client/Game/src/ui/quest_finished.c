/**
 * @file
 * Hold the quests the server says the character has already finished.
 */

#include "ui/quest_log.h"

#include <stdio.h>
#include <string.h>

int quest_log_make_room(QuestLogState* ql) {
	if(ql->count < MAX_QUESTS)
		return 1;

	// a finished quest is only history; one in progress is never dropped
	for(int i = 0; i < ql->count; i++) {
		if(!ql->entries[i].completed)
			continue;
		for(int j = i; j < ql->count - 1; j++)
			ql->entries[j] = ql->entries[j + 1];
		ql->count--;
		if(ql->selected == i)
			ql->selected = -1;
		else if(ql->selected > i)
			ql->selected--;

		return 1;
	}

	return 0;
}

void quest_log_finished_add(QuestLogState* ql, uint32_t id, const char* title) {
	for(int i = 0; i < ql->count; i++)
		if(ql->entries[i].id == id)
			return;
	if(ql->count >= MAX_QUESTS)
		return;

	QuestEntry* e = &ql->entries[ql->count++];

	memset(e, 0, sizeof(*e));
	e->id = id;
	snprintf(e->title, sizeof(e->title), "%s", title);
	e->completed = 1;
}
