#ifndef MAP_ENTITIES_H
#define MAP_ENTITIES_H

// Everything the map can mark, gathered into one list the screen draws from.

#include "core/game_types.h"
#include "ui/map_draw.h"

/** A floor this far above or below the player (3.5 m) is another level. */
#define MAP_OTHER_FLOOR_UNITS 77.0f

#define MAP_MAX_ENTITIES 288

typedef enum {
	MAP_KIND_HOSTILE,
	MAP_KIND_NPC,
	MAP_KIND_MERCHANT,
	MAP_KIND_PLAYER,
	MAP_KIND_PARTY,
	MAP_KIND_OBJECTIVE,
	MAP_KIND_LOOT,
	MAP_KIND_COUNT
} map_kind;

/** One thing on the map. */
typedef struct {
	map_kind  kind;
	float     x, y, z;          // world units
	float     dist;             // from the player on the ground, world units
	char      name[40];
	char      detail[72];       // a second line: a quest objective and its progress
	int       level;            // 0 when not known
	int       health, max_health;
	int       has_health;
	int       dead, elite, tracked, targeted;
	int       quest_count;      // how many of this NPC's quests the log can name
	char      quests[2][48];
	int       quest_mark;       // NPCQuestMark: a quest to take or to hand in
	char      mark_title[48];   // that quest's name
	int       has_npc_type;     // nonzero when npc_type says which kind of NPC this is
	int       npc_type;
	int       other_floor;      // far enough above or below to be another level
	map_color color;
} map_entity;

typedef struct {
	map_entity items[MAP_MAX_ENTITIES];
	int        count;
	int        counts[MAP_KIND_COUNT];
	float      player_x, player_y;   // where the player stands, world units
} map_entity_list;

void map_entities_gather(const GameState* game, map_entity_list* out);

/** The label a kind goes by in the key. */
const char* map_kind_label(map_kind kind);

/** The colour a kind is drawn in, for the key. */
map_color map_kind_color(map_kind kind);

#endif /* MAP_ENTITIES_H */
