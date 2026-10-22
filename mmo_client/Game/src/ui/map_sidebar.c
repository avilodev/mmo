/**
 * @file
 * Draw the map's information column.
 */
#include "ui/map_sidebar.h"
#include "ui/map_draw.h"
#include "ui/quest_log.h"
#include "combat/ability_bar.h"
#include "core/race_registry.h"
#include "render/renderer.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAD          10.0f
#define ROW          17.0f
#define CLOSEST_ROWS 7

static const map_color INK   = { 0.93f, 0.95f, 1.0f, 1.0f };
static const map_color MUTED = { 0.60f, 0.67f, 0.80f, 1.0f };
static const map_color GOLD  = { 1.0f, 0.84f, 0.28f, 1.0f };

// Where the next line goes, and the edge the column must not run past.
typedef struct {
	const map_rect* box;
	float           y;
} flow;

static float inner_width(const flow* f) {
	return f->box->w - PAD * 2.0f;
}

static int has_room(const flow* f, float needed) {
	return f->y + needed <= f->box->y + f->box->h - 30.0f;
}

static void heading(flow* f, const char* text) {
	f->y += 6.0f;
	map_text(f->box->x + PAD, f->y + 10.0f, MAP_TEXT_BODY, text, MUTED);
	renderer_draw_rect(f->box->x + PAD, f->y + 15.0f, inner_width(f), 1.0f,
	                   0.4f, 0.5f, 0.7f, 0.6f);
	f->y += 22.0f;
}

static void draw_line(flow* f, const char* text, map_color color) {
	char fitted[96];

	map_text_fit(fitted, sizeof(fitted), MAP_TEXT_BODY, inner_width(f), text);
	map_text(f->box->x + PAD, f->y + 10.0f, MAP_TEXT_BODY, fitted, color);
	f->y += ROW;
}

// A label on the left and a value pushed to the right edge.
static void draw_pair(flow* f, const char* label, const char* value, map_color color) {
	float right = f->box->x + f->box->w - PAD;

	map_text(f->box->x + PAD, f->y + 10.0f, MAP_TEXT_BODY, label, MUTED);
	map_text(right - map_text_width(MAP_TEXT_BODY, value), f->y + 10.0f, MAP_TEXT_BODY,
	         value, color);
	f->y += ROW;
}

static void draw_swatch(map_kind kind, float cx, float cy) {
	map_color c = map_kind_color(kind);

	if(kind == MAP_KIND_HOSTILE || kind == MAP_KIND_LOOT)
		map_diamond(cx, cy, 5.0f, c);
	else if(kind == MAP_KIND_PLAYER || kind == MAP_KIND_PARTY)
		map_disc(cx, cy, 4.5f, c);
	else if(kind == MAP_KIND_OBJECTIVE)
		renderer_draw_ring(cx, cy, 3.0f, 5.5f, c.r, c.g, c.b, c.a, 16);
	else
		renderer_draw_rect(cx - 4.0f, cy - 4.0f, 8.0f, 8.0f, c.r, c.g, c.b, c.a);
}

static void draw_resource_bar(const GameState* game, flow* f) {
	const AbilityBarState* bar = &game->playing->ability_bar;
	static const char* NAMES[RESOURCE_COUNT] = { "", "Mana", "Stamina", "Rage" };
	static const map_color FILLS[RESOURCE_COUNT] = {
		{ 0, 0, 0, 0 }, { 0.25f, 0.4f, 0.95f, 1 }, { 0.95f, 0.75f, 0.2f, 1 },
		{ 0.85f, 0.2f, 0.18f, 1 }
	};
	char text[48];

	if(!ability_bar_has_resource(bar) || bar->resource_type >= RESOURCE_COUNT)
		return;
	map_bar(&(map_rect){ f->box->x + PAD, f->y, inner_width(f), 9.0f },
	        (float)bar->resource / (float)bar->max_resource, FILLS[bar->resource_type]);
	snprintf(text, sizeof(text), "%s  %d / %d", NAMES[bar->resource_type], (int)bar->resource,
	         (int)bar->max_resource);
	map_text(f->box->x + PAD + 4.0f, f->y + 8.0f, MAP_TEXT_SMALL, text, INK);
	f->y += 17.0f;
}

static void section_you(const GameState* game, flow* f) {
	const CharacterInfo* info = &game->player.info;
	const RaceInfo* race = client_race_find(info->race_id);
	char text[96];
	float max_hp = info->max_health > 0 ? (float)info->max_health : 1.0f;

	heading(f, "YOU");
	snprintf(text, sizeof(text), "%s", info->name[0] ? info->name : "Adventurer");
	map_text(f->box->x + PAD, f->y + 11.0f, MAP_TEXT_HEAD, text, INK);
	snprintf(text, sizeof(text), "Lv %u", info->level);
	map_text(f->box->x + f->box->w - PAD - map_text_width(MAP_TEXT_BODY, text), f->y + 11.0f,
	         MAP_TEXT_BODY, text, GOLD);
	f->y += 21.0f;
	if(race)
		draw_line(f, race->name, MUTED);
	map_bar(&(map_rect){ f->box->x + PAD, f->y, inner_width(f), 9.0f },
	        (float)info->health / max_hp, (map_color){ 0.85f, 0.25f, 0.2f, 1.0f });
	snprintf(text, sizeof(text), "Health  %u / %u", info->health, info->max_health);
	map_text(f->box->x + PAD + 4.0f, f->y + 8.0f, MAP_TEXT_SMALL, text, INK);
	f->y += 17.0f;
	draw_resource_bar(game, f);
}

static void section_place(const GameState* game, const map_cut_info* cut, flow* f) {
	float east = map_metres(game->player.x - CITY_ORIGIN_X);
	float south = map_metres(game->player.y - CITY_ORIGIN_Y);
	char text[64];

	heading(f, "WHERE");
	draw_pair(f, "Area", game->playing->current_zone_name[0]
	                     ? game->playing->current_zone_name : "Ennara", INK);
	snprintf(text, sizeof(text), "%.0f m %s, %.0f m %s", fabsf(east), east >= 0.0f ? "E" : "W",
	         fabsf(south), south >= 0.0f ? "S" : "N");
	draw_pair(f, "From courtyard", text, INK);
	snprintf(text, sizeof(text), "%.1f m", map_metres(game->player.visual_z));
	draw_pair(f, "Elevation", text, INK);
	if(cut->indoors)
		draw_pair(f, "Map shows", "this floor", GOLD);
	else
		draw_pair(f, "Map shows", "roofs", INK);
}

// One unfinished objective: what it asks, how far along, and how far away.
static void draw_objective_row(const GameState* game, flow* f, const QuestObjEntry* o) {
	char progress[96];
	char text[64];
	char fitted[96];

	snprintf(progress, sizeof(progress), "%d / %d", (int)o->current, (int)o->required);
	if(o->has_marker) {
		map_describe_offset(text, sizeof(text), o->marker_x - game->player.x,
		                    o->marker_y - game->player.y, 0.0f);
		snprintf(progress + strlen(progress), sizeof(progress) - strlen(progress), "  %s", text);
	} else {
		snprintf(progress + strlen(progress), sizeof(progress) - strlen(progress), "  no marker");
	}

	float reach = map_text_width(MAP_TEXT_SMALL, progress);

	map_text_fit(fitted, sizeof(fitted), MAP_TEXT_BODY, inner_width(f) - reach - 22.0f,
	             o->description);
	map_text(f->box->x + PAD + 12.0f, f->y + 10.0f, MAP_TEXT_BODY, fitted, INK);
	map_text(f->box->x + f->box->w - PAD - reach, f->y + 10.0f, MAP_TEXT_SMALL, progress, MUTED);
	f->y += ROW;
}

static void section_quests(const GameState* game, flow* f) {
	const QuestLogState* log = &game->playing->quest_log;
	QuestTrackedStep step = quest_log_tracked_step(log);
	int shown = 0;

	for(int i = 0; i < log->count && shown < 5 && has_room(f, 60.0f); i++) {
		const QuestEntry* quest = &log->entries[i];

		if(!quest->active)
			continue;
		if(shown++ == 0)
			heading(f, "QUESTS");
		char line[96];
		char step_tag[16];

		if(quest_step_tag(quest, step_tag, sizeof(step_tag)))
			snprintf(line, sizeof(line), "%s  %s", quest->title, step_tag);
		else
			snprintf(line, sizeof(line), "%s", quest->title);
		draw_line(f, line, step.valid && step.quest_id == quest->id ? GOLD : INK);
		for(int j = 0; j < quest->obj_count && has_room(f, ROW); j++)
			if(quest->objectives[j].current < quest->objectives[j].required)
				draw_objective_row(game, f, &quest->objectives[j]);
		if(step.valid && step.quest_id == quest->id && step.required == 1 &&
		   strcmp(step.description, "Ready to turn in") == 0)
			draw_line(f, "  Ready to turn in", GOLD);
	}
}

static void section_target(const GameState* game, const map_entity_list* list, flow* f) {
	char text[96];

	for(int i = 0; i < list->count; i++) {
		const map_entity* e = &list->items[i];

		if(!e->targeted || !has_room(f, 70.0f))
			continue;
		heading(f, "TARGET");
		if(e->level > 0)
			snprintf(text, sizeof(text), "%s  Lv %d", e->name, e->level);
		else
			snprintf(text, sizeof(text), "%s", e->name);
		map_text(f->box->x + PAD, f->y + 11.0f, MAP_TEXT_HEAD, text, map_kind_color(e->kind));
		f->y += 21.0f;
		if(e->has_health && e->max_health > 0) {
			map_bar(&(map_rect){ f->box->x + PAD, f->y, inner_width(f), 9.0f },
			        (float)e->health / (float)e->max_health,
			        (map_color){ 0.85f, 0.25f, 0.2f, 1.0f });
			snprintf(text, sizeof(text), "%d / %d", e->health, e->max_health);
			map_text(f->box->x + PAD + 4.0f, f->y + 8.0f, MAP_TEXT_SMALL, text, INK);
			f->y += 17.0f;
		}
		map_describe_offset(text, sizeof(text), e->x - game->player.x, e->y - game->player.y,
		                    e->z - game->player.visual_z);
		draw_pair(f, map_kind_label(e->kind), text, INK);
		return;
	}
}

// Count what is about to hurt: danger shapes being cast and standing ground zones.
static void section_threats(const GameState* game, flow* f) {
	const PlayingState* ps = game->playing;
	int telegraphs = 0;
	int zones = 0;
	float nearest = -1.0f;
	char text[64];

	for(int i = 0; i < MAX_TELEGRAPHS; i++) {
		const VisibleTelegraph* t = &ps->telegraphs[i];
		float d = hypotf(t->pos_x - game->player.x, t->pos_y - game->player.y);

		if(!t->active)
			continue;
		telegraphs++;
		if(nearest < 0.0f || d < nearest)
			nearest = d;
	}
	for(int i = 0; i < MAX_ZONES; i++)
		zones += ps->zones[i].active;
	if(telegraphs + zones == 0 || !has_room(f, 60.0f))
		return;
	heading(f, "DANGER");
	if(telegraphs) {
		snprintf(text, sizeof(text), "%d, nearest %.0f m", telegraphs, map_metres(nearest));
		draw_pair(f, "Attacks being cast", text, (map_color){ 1.0f, 0.45f, 0.35f, 1.0f });
	}
	if(zones) {
		snprintf(text, sizeof(text), "%d", zones);
		draw_pair(f, "Ground zones", text, INK);
	}
}

// The party member whose id matches, among the entities on the map.
static const map_entity* find_party_entity(const map_entity_list* list, const char* name) {
	for(int i = 0; i < list->count; i++)
		if(list->items[i].kind == MAP_KIND_PARTY && strcmp(list->items[i].name, name) == 0)
			return &list->items[i];

	return NULL;
}

static void section_party(const GameState* game, const map_entity_list* list, flow* f) {
	const PartyState* party = &game->playing->party;
	char text[96];

	if(!party->has_party || !has_room(f, 60.0f))
		return;
	heading(f, "PARTY");
	for(int i = 0; i < party->member_count && has_room(f, 34.0f); i++) {
		const PartyMember* m = &party->members[i];
		const map_entity* e = find_party_entity(list, m->name);
		float hp = m->max_health > 0 ? (float)m->health / (float)m->max_health : 0.0f;

		snprintf(text, sizeof(text), "%s%s  Lv %d", m->name,
		         m->id == party->leader_id ? " *" : "", m->level);
		map_text(f->box->x + PAD, f->y + 10.0f, MAP_TEXT_BODY, text, INK);
		if(e)
			map_describe_offset(text, sizeof(text), e->x - game->player.x,
			                    e->y - game->player.y, e->z - game->player.visual_z);
		else
			snprintf(text, sizeof(text), "%s", strcmp(m->name, game->player.info.name) == 0
			                                  ? "you" : "out of range");
		map_text(f->box->x + f->box->w - PAD - map_text_width(MAP_TEXT_SMALL, text),
		         f->y + 10.0f, MAP_TEXT_SMALL, text, MUTED);
		map_bar(&(map_rect){ f->box->x + PAD, f->y + 14.0f, inner_width(f), 4.0f }, hp,
		        (map_color){ 0.3f, 0.85f, 0.4f, 1.0f });
		f->y += 26.0f;
	}
}

static void section_key(const map_entity_list* list, flow* f) {
	static const map_kind SHOWN[] = { MAP_KIND_HOSTILE, MAP_KIND_NPC, MAP_KIND_MERCHANT,
	                                  MAP_KIND_PLAYER, MAP_KIND_PARTY, MAP_KIND_OBJECTIVE, MAP_KIND_LOOT };
	char count[16];

	if(!has_room(f, 150.0f))
		return;
	heading(f, "ON THE MAP");
	for(int i = 0; i < (int)(sizeof(SHOWN) / sizeof(SHOWN[0])); i++) {
		map_kind kind = SHOWN[i];

		draw_swatch(kind, f->box->x + PAD + 6.0f, f->y + 6.0f);
		map_text(f->box->x + PAD + 20.0f, f->y + 10.0f, MAP_TEXT_BODY, map_kind_label(kind),
		         list->counts[kind] ? INK : MUTED);
		snprintf(count, sizeof(count), "%d", list->counts[kind]);
		map_text(f->box->x + f->box->w - PAD - map_text_width(MAP_TEXT_BODY, count),
		         f->y + 10.0f, MAP_TEXT_BODY, count, list->counts[kind] ? INK : MUTED);
		f->y += ROW;
	}
}

static int by_distance(const void* a, const void* b) {
	float da = (*(const map_entity* const*)a)->dist;
	float db = (*(const map_entity* const*)b)->dist;

	return da < db ? -1 : (da > db ? 1 : 0);
}

// Pick the nearest things on the map.
static int nearest(const map_entity_list* list, const map_entity** out, int max) {
	const map_entity* pool[MAP_MAX_ENTITIES];
	int n = 0;

	for(int i = 0; i < list->count; i++)
		pool[n++] = &list->items[i];
	qsort(pool, (size_t)n, sizeof(pool[0]), by_distance);
	if(n > max)
		n = max;
	memcpy(out, pool, (size_t)n * sizeof(pool[0]));

	return n;
}

static void section_closest(const GameState* game, const map_entity_list* list, flow* f) {
	const map_entity* closest[CLOSEST_ROWS];
	int n = nearest(list, closest, CLOSEST_ROWS);
	char text[64];

	if(n == 0 || !has_room(f, 60.0f))
		return;
	heading(f, "CLOSEST");
	for(int i = 0; i < n && has_room(f, ROW); i++) {
		const map_entity* e = closest[i];

		draw_swatch(e->kind, f->box->x + PAD + 6.0f, f->y + 6.0f);
		map_describe_offset(text, sizeof(text), e->x - game->player.x, e->y - game->player.y,
		                    e->z - game->player.visual_z);

		float reach = map_text_width(MAP_TEXT_SMALL, text);
		char name[48];

		map_text_fit(name, sizeof(name), MAP_TEXT_BODY, inner_width(f) - 26.0f - reach, e->name);
		map_text(f->box->x + PAD + 20.0f, f->y + 10.0f, MAP_TEXT_BODY, name, INK);
		map_text(f->box->x + f->box->w - PAD - reach, f->y + 10.0f, MAP_TEXT_SMALL, text, MUTED);
		f->y += ROW;
	}
}

void map_sidebar_draw(const GameState* game, const map_rect* side,
                      const map_entity_list* list, const map_cut_info* cut) {
	flow f = { side, side->y };

	map_fill(side, (map_color){ 0.06f, 0.07f, 0.11f, 0.97f });
	map_outline(side, 1.5f, (map_color){ 0.3f, 0.4f, 0.58f, 1.0f });
	map_clip_begin(side, (float)game->camera.viewport_width);
	section_you(game, &f);
	section_place(game, cut, &f);
	section_target(game, list, &f);
	section_quests(game, &f);
	section_threats(game, &f);
	section_party(game, list, &f);
	section_key(list, &f);
	section_closest(game, list, &f);
	map_clip_end();
	map_text(side->x + PAD, side->y + side->h - 9.0f, MAP_TEXT_SMALL,
	         "Scroll: zoom   Drag: pan   M / Esc: close", MUTED);
}
