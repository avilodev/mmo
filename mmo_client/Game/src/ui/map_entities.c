/**
 * @file
 * Gather the NPCs, players, loot and quest objectives the map marks.
 */
#include "ui/map_entities.h"
#include "ui/quest_log.h"
#include "core/race_registry.h"
#include "player/item_db.h"
#include "world/npc_types.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static const map_color COLORS[MAP_KIND_COUNT] = {
	{ 0.92f, 0.24f, 0.24f, 1.0f },   // hostile
	{ 0.30f, 0.62f, 0.96f, 1.0f },   // npc
	{ 0.98f, 0.62f, 0.20f, 1.0f },   // merchant
	{ 0.45f, 0.92f, 0.60f, 1.0f },   // player
	{ 0.35f, 1.00f, 0.55f, 1.0f },   // party
	{ 1.00f, 0.84f, 0.22f, 1.0f },   // quest objective
	{ 1.00f, 0.86f, 0.30f, 1.0f }    // loot
};

static const char* const LABELS[MAP_KIND_COUNT] = {
	"Hostile", "NPC", "Merchant", "Player", "Party", "Quest objective", "Loot"
};

const char* map_kind_label(map_kind kind) {
	return LABELS[kind];
}

map_color map_kind_color(map_kind kind) {
	return COLORS[kind];
}

// Add one entity and fill what every kind shares. Returns it, or NULL when full.
static map_entity* add(map_entity_list* list, map_kind kind, const GameState* game,
                       float x, float y, float z, const char* name) {
	if(list->count >= MAP_MAX_ENTITIES)
		return NULL;

	map_entity* e = &list->items[list->count++];
	float dx = x - game->player.x;
	float dy = y - game->player.y;

	memset(e, 0, sizeof(*e));
	e->kind = kind;
	e->x = x;
	e->y = y;
	e->z = z;
	e->dist = sqrtf(dx * dx + dy * dy);
	e->color = COLORS[kind];
	e->other_floor = fabsf(z - game->player.visual_z) > MAP_OTHER_FLOOR_UNITS;
	snprintf(e->name, sizeof(e->name), "%s", name);
	list->counts[kind]++;

	return e;
}

// The NPC type the tracked objective points at, or 0 when it points at none.
static uint32_t tracked_npc_type(const GameState* game) {
	QuestTrackedStep step = quest_log_tracked_step(&game->playing->quest_log);

	if(!step.valid || (step.objective_type != QUEST_OBJECTIVE_KILL &&
	                   step.objective_type != QUEST_OBJECTIVE_TALK))
		return 0;

	return step.target_id;
}

// Name the logged quests whose unfinished objectives are this kind of NPC, to
// talk to or to kill. The server's marker is one fixed spot picked when the
// quest arrived, so the NPCs themselves are marked too wherever they now are.
static void name_quests(const GameState* game, uint8_t npc_type, map_entity* e) {
	const QuestLogState* log = &game->playing->quest_log;

	for(int i = 0; i < log->count && e->quest_count < 2; i++) {
		const QuestEntry* quest = &log->entries[i];

		if(!quest->active)
			continue;
		for(int j = 0; j < quest->obj_count; j++) {
			const QuestObjEntry* o = &quest->objectives[j];

			int is_npc_objective = o->objective_type == QUEST_OBJECTIVE_TALK ||
			                       o->objective_type == QUEST_OBJECTIVE_KILL;

			if(!is_npc_objective || o->target_id != npc_type || o->current >= o->required)
				continue;
			snprintf(e->quests[e->quest_count++], sizeof(e->quests[0]), "%s", quest->title);
			break;
		}
	}
}

// What an NPC is, from its category and the roles its type declares.
static map_kind npc_kind(const VisibleNPC* npc) {
	if(npc->category == 1)
		return MAP_KIND_HOSTILE;
	if(npc_type_has_role(npc->npc_type_id, NPC_ROLE_MERCHANT))
		return MAP_KIND_MERCHANT;

	return MAP_KIND_NPC;
}

// Copy what the server says this NPC type has for the player onto its entity.
static void take_quest_mark(const GameState* game, uint8_t npc_type, map_entity* e) {
	const PlayingState* ps = game->playing;

	for(int i = 0; i < ps->npc_quest_mark_count; i++) {
		const NPCQuestMarkEntry* mark = &ps->npc_quest_marks[i];

		if(mark->npc_type_id != npc_type || mark->mark == NPC_QUEST_MARK_NONE)
			continue;
		e->quest_mark = mark->mark;
		snprintf(e->mark_title, sizeof(e->mark_title), "%s", mark->title);
		return;
	}
}

// Whether the log has an available quest expanded, and which NPC type gives it.
static int selected_giver(const GameState* game, uint8_t* npc_type) {
	const QuestLogState* log = &game->playing->quest_log;

	if(log->available_selected < 0 || log->available_selected >= log->available_count)
		return 0;
	*npc_type = log->available[log->available_selected].giver_npc_type;

	return 1;
}

static void gather_npcs(const GameState* game, map_entity_list* list) {
	uint32_t tracked = tracked_npc_type(game);
	uint8_t chosen = 0;
	int has_chosen = selected_giver(game, &chosen);
	float outline[3];

	for(int i = 0; i < game->playing->visible_npc_count; i++) {
		const VisibleNPC* npc = &game->playing->visible_npcs[i];
		map_kind kind = npc_kind(npc);
		const char* name = npc->name[0] ? npc->name : npc_type_get_name(npc->npc_type_id);
		map_entity* e = npc->is_alive
			? add(list, kind, game, npc->pos_x, npc->pos_y, npc->pos_z, name ? name : "NPC")
			: NULL;

		if(!e)
			continue;
		name_quests(game, npc->npc_type_id, e);
		e->elite = npc_type_get_outline(npc->npc_type_id, outline);
		take_quest_mark(game, npc->npc_type_id, e);
		e->has_npc_type = 1;
		e->npc_type = (int)npc->npc_type_id;
		e->tracked = (tracked != 0 && tracked == npc->npc_type_id) ||
		             (has_chosen && chosen == npc->npc_type_id);
		e->targeted = npc->npc_id == game->playing->target_npc_id;
	}
}

// Whether a nearby player is in the party.
static int in_party(const GameState* game, uint32_t player_id) {
	const PartyState* party = &game->playing->party;

	if(!party->has_party)
		return 0;
	for(int i = 0; i < party->member_count; i++)
		if(party->members[i].id == player_id)
			return 1;

	return 0;
}

static void gather_players(const GameState* game, map_entity_list* list) {
	for(int i = 0; i < game->playing->nearby_player_count; i++) {
		const NearbyPlayer* p = &game->playing->nearby_players[i];
		map_kind kind = in_party(game, p->player_id) ? MAP_KIND_PARTY : MAP_KIND_PLAYER;
		map_entity* e = add(list, kind, game, p->pos_x, p->pos_y, p->pos_z,
		                    p->name[0] ? p->name : "Adventurer");

		if(!e)
			continue;
		e->level = p->level;
		e->health = p->health;
		e->max_health = p->max_health;
		e->has_health = p->max_health > 0;
		e->dead = p->is_dead;
		e->targeted = p->player_id == game->playing->target_player_id;
		if(kind == MAP_KIND_PLAYER)
			client_race_color(p->player_race, &e->color.r, &e->color.g, &e->color.b);
	}
}

static void gather_loot(const GameState* game, map_entity_list* list) {
	for(int i = 0; i < MAX_GROUND_ITEMS; i++) {
		const GroundItem* item = &game->playing->ground_items[i];
		char name[40];

		if(!item->active)
			continue;
		if(item->quantity > 1)
			snprintf(name, sizeof(name), "%s x%d", item_db_get_name(item->item_id),
			         item->quantity);
		else
			snprintf(name, sizeof(name), "%s", item_db_get_name(item->item_id));
		add(list, MAP_KIND_LOOT, game, item->pos_x, item->pos_y, item->pos_z, name);
	}
}

// Mark where each active quest's unfinished objectives lead.
static void gather_objectives(const GameState* game, map_entity_list* list) {
	const QuestLogState* log = &game->playing->quest_log;
	QuestTrackedStep step = quest_log_tracked_step(log);

	for(int i = 0; i < log->count; i++) {
		const QuestEntry* quest = &log->entries[i];

		if(!quest->active)
			continue;
		for(int j = 0; j < quest->obj_count; j++) {
			const QuestObjEntry* o = &quest->objectives[j];

			if(o->current >= o->required || !o->has_marker)
				continue;

			map_entity* e = add(list, MAP_KIND_OBJECTIVE, game, o->marker_x, o->marker_y,
			                    game->player.visual_z, quest->title);

			if(!e)
				return;
			snprintf(e->detail, sizeof(e->detail), "%s  %d / %d", o->description,
			         (int)o->current, (int)o->required);
			e->tracked = step.valid && step.quest_id == quest->id && step.has_marker &&
			             step.marker_x == o->marker_x && step.marker_y == o->marker_y;
		}
	}
}

// Whether the list already holds an NPC of this type.
static int has_npc_of_type(const map_entity_list* list, int npc_type) {
	for(int i = 0; i < list->count; i++)
		if(list->items[i].has_npc_type && list->items[i].npc_type == npc_type)
			return 1;

	return 0;
}

// Mark givers of quests the character may take who are too far away to be seen.
static void gather_far_givers(const GameState* game, map_entity_list* list) {
	const QuestLogState* log = &game->playing->quest_log;
	uint8_t chosen = 0;
	int has_chosen = selected_giver(game, &chosen);

	for(int i = 0; i < log->available_count; i++) {
		const available_quest_entry* q = &log->available[i];
		const char* name = npc_type_get_name(q->giver_npc_type);
		map_kind kind = npc_type_has_role(q->giver_npc_type, NPC_ROLE_MERCHANT)
			? MAP_KIND_MERCHANT : MAP_KIND_NPC;

		if(!q->has_giver_pos || has_npc_of_type(list, q->giver_npc_type))
			continue;

		map_entity* e = add(list, kind, game, q->giver_x, q->giver_y,
		                    game->player.visual_z, name ? name : "NPC");

		if(!e)
			return;
		e->has_npc_type = 1;
		e->npc_type = q->giver_npc_type;
		e->quest_mark = NPC_QUEST_MARK_AVAILABLE;
		snprintf(e->mark_title, sizeof(e->mark_title), "%s", q->title);
		e->tracked = has_chosen && chosen == q->giver_npc_type;
	}
}

void map_entities_gather(const GameState* game, map_entity_list* out) {
	memset(out, 0, sizeof(*out));
	if(!game || !game->playing)
		return;
	out->player_x = game->player.x;
	out->player_y = game->player.y;
	gather_npcs(game, out);
	gather_far_givers(game, out);
	gather_players(game, out);
	gather_loot(game, out);
	gather_objectives(game, out);
}
