// How far a quest is from the player, for the quest list.

#include "ui/quest_distance.h"

#include <math.h>
#include <stdio.h>

// An exact thousand metres reads as 1.0 km, so metres stop at 999.5
#define KM_FROM_M 999.5f

// From 10 km on, a decimal is noise
#define WHOLE_KM_FROM_M 9950.0f

int quest_active_target(const QuestEntry* quest, float* out_x, float* out_y) {
	for(int i = 0; i < quest->obj_count; i++) {
		const QuestObjEntry* obj = &quest->objectives[i];

		if(obj->current >= obj->required || !obj->has_marker)
			continue;

		*out_x = obj->marker_x;
		*out_y = obj->marker_y;
		return 1;
	}
	return 0;
}

int quest_available_target(const available_quest_entry* quest, float* out_x, float* out_y) {
	if(!quest->has_giver_pos)
		return 0;

	*out_x = quest->giver_x;
	*out_y = quest->giver_y;
	return 1;
}

float quest_distance_m(float from_x, float from_y, float to_x, float to_y) {
	float dx = to_x - from_x;
	float dy = to_y - from_y;

	return sqrtf(dx * dx + dy * dy) / QUEST_UNITS_PER_METRE;
}

void quest_distance_text(float metres, char* out, size_t size) {
	if(metres < 0.0f)
		metres = 0.0f;

	if(metres < KM_FROM_M)
		snprintf(out, size, "%d m", (int)lroundf(metres));
	else if(metres < WHOLE_KM_FROM_M)
		snprintf(out, size, "%.1f km", metres / 1000.0f);
	else
		snprintf(out, size, "%d km", (int)lroundf(metres / 1000.0f));
}
