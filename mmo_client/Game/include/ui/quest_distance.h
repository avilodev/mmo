#ifndef QUEST_DISTANCE_H
#define QUEST_DISTANCE_H

// How far a quest is from the player, for the quest list. No GL.

#include "ui/quest_log.h"

#include <stddef.h>

// World units in one metre: the characters are 40 units for 1.83 metres
#define QUEST_UNITS_PER_METRE (40.0f / 1.83f)

// Where an active quest points: its first unfinished objective with a place
int quest_active_target(const QuestEntry* quest, float* out_x, float* out_y);

// Where an available quest is taken: its giver. Nonzero when that is known.
int quest_available_target(const available_quest_entry* quest, float* out_x, float* out_y);

// The distance between two world positions in metres
float quest_distance_m(float from_x, float from_y, float to_x, float to_y);

// "85 m", or "1.2 km" from a thousand metres on
void quest_distance_text(float metres, char* out, size_t size);

#endif /* QUEST_DISTANCE_H */
