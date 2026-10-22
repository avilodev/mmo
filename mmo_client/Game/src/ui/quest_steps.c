/**
 * @file
 * Hold which step of a quest the player is on, and say so in words.
 */

#include "ui/quest_log.h"

#include <stdio.h>
#include <string.h>

void quest_log_set_step(QuestLogState* ql, uint32_t id, uint8_t step, uint8_t step_count,
                        const char* step_title) {
	for(int i = 0; i < ql->count; i++) {
		QuestEntry* e = &ql->entries[i];

		if(e->id != id)
			continue;

		// progress belongs to a step, so a new step starts from nothing
		if(e->step != step)
			for(int j = 0; j < MAX_QUEST_OBJ; j++)
				e->objectives[j].current = 0;
		e->step = step;
		e->step_count = step_count;
		snprintf(e->step_title, sizeof(e->step_title), "%s", step_title);

		return;
	}
}

int quest_step_label(const QuestEntry* e, char* out, size_t size) {
	if(size > 0)
		out[0] = '\0';
	if(e->step_count <= 1)
		return 0;
	if(e->step_title[0] != '\0')
		snprintf(out, size, "Step %u of %u: %s", (unsigned)e->step, (unsigned)e->step_count,
		         e->step_title);
	else
		snprintf(out, size, "Step %u of %u", (unsigned)e->step, (unsigned)e->step_count);

	return 1;
}

int quest_step_tag(const QuestEntry* e, char* out, size_t size) {
	if(size > 0)
		out[0] = '\0';
	if(e->step_count <= 1)
		return 0;
	snprintf(out, size, "%u/%u", (unsigned)e->step, (unsigned)e->step_count);

	return 1;
}
