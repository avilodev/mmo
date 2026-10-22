/**
 * @file
 * Draw the entities, hazards and the player on the map.
 */
#include "ui/map_markers.h"
#include "ui/map_pointer.h"
#include "ui/hud.h"
#include "render/renderer.h"

#include <GLFW/glfw3.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define MAX_LABELS      96
#define HIT_RADIUS      11.0f
#define LABEL_ZOOM_PX_M 7.0f
#define PI_F            3.14159265f

static map_rect g_labels[MAX_LABELS];
static int      g_label_count;

void map_markers_begin(void) {
	g_label_count = 0;
}

// Record a label's box if it overlaps no earlier one. Returns 1 when it may draw.
static int claim_label(const map_rect* r) {
	for(int i = 0; i < g_label_count; i++) {
		const map_rect* o = &g_labels[i];

		if(r->x < o->x + o->w && r->x + r->w > o->x &&
		   r->y < o->y + o->h && r->y + r->h > o->y)
			return 0;
	}
	if(g_label_count < MAX_LABELS)
		g_labels[g_label_count++] = *r;

	return 1;
}

static float pulse(void) {
	return 0.5f + 0.5f * sinf((float)glfwGetTime() * 4.0f);
}

// Draw a name (and level) beside a marker, unless it would sit on another label.
static void draw_label(const map_entity* e, float sx, float sy, map_color color) {
	char text[64];

	if(e->level > 0) {
		snprintf(text, sizeof(text), "%s  Lv %d", e->name, e->level);
	} else {
		snprintf(text, sizeof(text), "%s", e->name);
	}

	map_rect box = { sx + 9.0f, sy - 8.0f, map_text_width(MAP_TEXT_SMALL, text) + 4.0f, 13.0f };

	if(claim_label(&box))
		map_text_shadowed(box.x + 2.0f, box.y + 10.0f, MAP_TEXT_SMALL, text, color);
}

static void draw_health(const map_entity* e, float sx, float sy, float width) {
	if(!e->has_health || e->dead || e->health >= e->max_health)
		return;

	map_rect bar = { sx - width * 0.5f, sy + 9.0f, width, 3.0f };

	map_bar(&bar, (float)e->health / (float)e->max_health,
	        (map_color){ 0.35f, 0.9f, 0.4f, 1.0f });
}

// A bullseye: where a quest objective leads.
static void draw_objective_glyph(const map_entity* e, float sx, float sy) {
	float beat = e->tracked ? pulse() * 3.0f : 0.0f;

	renderer_draw_ring(sx, sy, 8.0f + beat, 10.0f + beat, 0.0f, 0.0f, 0.0f, 0.85f, 24);
	renderer_draw_ring(sx, sy, 8.5f + beat, 9.5f + beat, 1.0f, 0.84f, 0.22f, 1.0f, 24);
	renderer_draw_circle(sx, sy, 3.5f, 1.0f, 0.84f, 0.22f, 1.0f, 12);
}

// A gold "!" over an NPC with a quest to give, a "?" over one to hand in to.
static void draw_quest_badge(const map_entity* e, float sx, float sy, float alpha) {
	float bx = sx + 8.0f;
	float by = sy - 9.0f;

	map_disc(bx, by, 5.5f, (map_color){ 0.98f, 0.82f, 0.18f, alpha });
	renderer_draw_rect(bx - 0.75f, by - 3.5f, 1.5f, e->quest_mark == NPC_QUEST_MARK_READY ? 2.5f : 4.0f,
	                   0.12f, 0.1f, 0.02f, alpha);
	renderer_draw_rect(bx - 0.75f, by + 1.5f, 1.5f, 1.5f, 0.12f, 0.1f, 0.02f, alpha);
}

static void draw_one(const map_entity* e, float sx, float sy) {
	map_color c = e->color;

	c.a = e->other_floor ? 0.38f : 1.0f;
	if(e->dead)
		c = (map_color){ 0.5f, 0.5f, 0.5f, c.a };
	switch(e->kind) {
	case MAP_KIND_HOSTILE:
		map_diamond(sx, sy, 6.0f, c);
		if(e->elite)
			renderer_draw_ring(sx, sy, 8.0f, 9.5f, 1.0f, 0.82f, 0.25f, c.a, 4);
		break;
	case MAP_KIND_NPC:
	case MAP_KIND_MERCHANT:
		renderer_draw_rect(sx - 5.5f, sy - 5.5f, 11.0f, 11.0f, 0.0f, 0.0f, 0.0f, 0.85f * c.a);
		renderer_draw_rect(sx - 4.0f, sy - 4.0f, 8.0f, 8.0f, c.r, c.g, c.b, c.a);
		if(e->quest_mark != NPC_QUEST_MARK_NONE)
			draw_quest_badge(e, sx, sy, c.a);
		break;
	case MAP_KIND_PLAYER:
		map_disc(sx, sy, 5.0f, c);
		break;
	case MAP_KIND_PARTY:
		renderer_draw_ring(sx, sy, 8.0f, 10.0f, 0.4f, 1.0f, 0.6f, 0.7f * c.a, 24);
		map_disc(sx, sy, 5.5f, c);
		break;
	case MAP_KIND_OBJECTIVE:
		draw_objective_glyph(e, sx, sy);
		break;
	case MAP_KIND_LOOT:
		map_diamond(sx, sy, 4.5f, c);
		break;
	default:
		break;
	}
}

static void draw_rings(const map_entity* e, float sx, float sy) {
	if(e->quest_count > 0)
		renderer_draw_ring(sx, sy, 8.0f, 9.5f, 1.0f, 0.84f, 0.22f, 0.95f, 24);
	if(e->targeted)
		renderer_draw_ring(sx, sy, 11.0f, 13.0f, 1.0f, 1.0f, 1.0f, 0.95f, 24);
	if(e->tracked)
		renderer_draw_ring(sx, sy, 13.0f + pulse() * 3.0f, 15.0f + pulse() * 3.0f,
		                   1.0f, 0.85f, 0.2f, 0.9f, 24);
}

// Whether this entity is named on the map without the cursor on it.
static int always_labelled(const map_entity* e, float px_per_metre) {
	if(e->quest_mark != NPC_QUEST_MARK_NONE)
		return 1;
	if(e->kind == MAP_KIND_LOOT || e->kind == MAP_KIND_NPC)
		return px_per_metre >= LABEL_ZOOM_PX_M * 0.6f;

	return 1;
}

static void draw_entity_pass(const map_entity_list* list, const map_projection* proj,
                             int labels, map_kind kind) {
	float px_per_metre = proj->k * CITY_UNITS_PER_METRE;

	for(int i = 0; i < list->count; i++) {
		const map_entity* e = &list->items[i];
		float sx, sy;

		if(e->kind != kind)
			continue;
		map_project(proj, e->x, e->y, &sx, &sy);
		if(!map_rect_has(&proj->area, sx, sy))
			continue;
		draw_one(e, sx, sy);
		draw_rings(e, sx, sy);
		draw_health(e, sx, sy, e->kind == MAP_KIND_PARTY ? 26.0f : 16.0f);
		if(labels && (e->targeted || e->tracked || always_labelled(e, px_per_metre))) {
			map_color text = e->other_floor ? (map_color){ 0.7f, 0.7f, 0.75f, 1.0f }
			                                : (map_color){ 0.95f, 0.96f, 1.0f, 1.0f };

			draw_label(e, sx, sy, text);
		}
	}
}

// Where a tracked objective lies when it is off the map: an arrow on the edge.
static void draw_edge_arrow(const map_entity* e, const map_entity_list* list,
                            const map_projection* proj) {
	float sx, sy;
	float inset = 22.0f;
	const map_rect* a = &proj->area;

	map_project(proj, e->x, e->y, &sx, &sy);
	if(map_rect_has(a, sx, sy))
		return;

	float cx = a->x + a->w * 0.5f;
	float cy = a->y + a->h * 0.5f;
	float dx = sx - cx;
	float dy = sy - cy;
	float scale = fminf((a->w * 0.5f - inset) / fmaxf(fabsf(dx), 0.001f),
	                    (a->h * 0.5f - inset) / fmaxf(fabsf(dy), 0.001f));
	float ex = cx + dx * scale;
	float ey = cy + dy * scale;
	float len = sqrtf(dx * dx + dy * dy);
	char text[64];

	map_dart(ex, ey, dx / len, dy / len, 10.0f, (map_color){ 1.0f, 0.84f, 0.22f, 1.0f });
	map_describe_offset(text, sizeof(text), e->x - list->player_x, e->y - list->player_y, 0.0f);

	float label_x = ex - map_text_width(MAP_TEXT_BODY, text) * 0.5f;

	label_x = fminf(fmaxf(label_x, a->x + 6.0f),
	                a->x + a->w - map_text_width(MAP_TEXT_BODY, text) - 6.0f);
	map_text_shadowed(label_x, ey + (dy > 0.0f ? -16.0f : 24.0f), MAP_TEXT_BODY, text,
	                  (map_color){ 1.0f, 0.9f, 0.4f, 1.0f });
}

void map_markers_draw_entities(const map_entity_list* list, const map_projection* proj,
                               int hovered, int labels) {
	// Quiet marks first so the ones that matter are never covered.
	static const map_kind ORDER[MAP_KIND_COUNT] = {
		MAP_KIND_LOOT, MAP_KIND_NPC, MAP_KIND_MERCHANT, MAP_KIND_HOSTILE, MAP_KIND_PLAYER,
		MAP_KIND_PARTY, MAP_KIND_OBJECTIVE
	};

	for(int pass = 0; pass < MAP_KIND_COUNT; pass++)
		draw_entity_pass(list, proj, labels, ORDER[pass]);
	for(int i = 0; i < list->count; i++)
		if(list->items[i].kind == MAP_KIND_OBJECTIVE && list->items[i].tracked)
			draw_edge_arrow(&list->items[i], list, proj);
	if(hovered >= 0 && hovered < list->count) {
		const map_entity* e = &list->items[hovered];
		float sx, sy;

		map_project(proj, e->x, e->y, &sx, &sy);
		renderer_draw_ring(sx, sy, 12.0f, 13.5f, 1.0f, 1.0f, 1.0f, 0.9f, 20);
		draw_label(e, sx, sy, (map_color){ 1.0f, 1.0f, 0.7f, 1.0f });
	}
}

int map_markers_hit(const map_entity_list* list, const map_projection* proj,
                    float screen_x, float screen_y) {
	int best = -1;
	float best_d2 = HIT_RADIUS * HIT_RADIUS;

	for(int i = 0; i < list->count; i++) {
		float sx, sy;

		map_project(proj, list->items[i].x, list->items[i].y, &sx, &sy);

		float d2 = (sx - screen_x) * (sx - screen_x) + (sy - screen_y) * (sy - screen_y);

		// Later entries are the kinds drawn on top, so a tie goes to them.
		if(d2 <= best_d2) {
			best = i;
			best_d2 = d2;
		}
	}

	return best;
}

static void draw_telegraph(const VisibleTelegraph* t, const map_projection* proj) {
	float sx, sy;
	float reach = t->length > 0.0f ? t->length : t->radius;
	float fill = t->cast_time > 0.0f ? t->elapsed / t->cast_time : 1.0f;

	map_project(proj, t->pos_x, t->pos_y, &sx, &sy);
	if(fill > 1.0f)
		fill = 1.0f;
	if(t->shape == 1) {
		renderer_draw_cone(sx, sy, t->dir_x, t->dir_y, t->radius * proj->k, t->angle,
		                   1.0f, 0.25f, 0.2f, 0.22f, 20);
		renderer_draw_cone(sx, sy, t->dir_x, t->dir_y, t->radius * proj->k * fill,
		                   t->angle, 1.0f, 0.3f, 0.2f, 0.4f, 20);
	} else if(t->shape == 0) {
		renderer_draw_circle(sx, sy, t->radius * proj->k, 1.0f, 0.25f, 0.2f, 0.22f, 32);
		renderer_draw_circle(sx, sy, t->radius * proj->k * fill, 1.0f, 0.3f, 0.2f, 0.4f, 32);
		renderer_draw_ring(sx, sy, t->radius * proj->k - 1.5f, t->radius * proj->k,
		                   1.0f, 0.4f, 0.3f, 0.9f, 32);
	} else {
		map_dotted_line(sx, sy, sx + t->dir_x * reach * proj->k * fill,
		                sy + t->dir_y * reach * proj->k * fill, 3.0f,
		                (map_color){ 1.0f, 0.35f, 0.25f, 0.9f });
	}
}

void map_markers_draw_hazards(const GameState* game, const map_projection* proj) {
	const PlayingState* ps = game->playing;

	for(int i = 0; i < MAX_ZONES; i++) {
		const VisibleZone* z = &ps->zones[i];
		float sx, sy;

		if(!z->active)
			continue;
		map_project(proj, z->pos_x, z->pos_y, &sx, &sy);
		renderer_draw_circle(sx, sy, z->radius * proj->k, 0.6f, 0.35f, 0.95f, 0.2f, 32);
		renderer_draw_ring(sx, sy, z->radius * proj->k - 1.5f, z->radius * proj->k,
		                   0.75f, 0.5f, 1.0f, 0.8f, 32);
	}
	for(int i = 0; i < MAX_TELEGRAPHS; i++)
		if(ps->telegraphs[i].active)
			draw_telegraph(&ps->telegraphs[i], proj);
	for(int i = 0; i < ps->projectile_count; i++) {
		float sx, sy;

		if(!ps->projectiles[i].active)
			continue;
		map_project(proj, ps->projectiles[i].pos_x, ps->projectiles[i].pos_y, &sx, &sy);
		renderer_draw_rect(sx - 1.5f, sy - 1.5f, 3.0f, 3.0f, 1.0f, 1.0f, 0.85f, 1.0f);
	}
}

void map_markers_draw_self_sized(const GameState* game, const map_projection* proj, float size,
                                 map_pointer_style style) {
	float sx, sy;
	float fx = game->player.facing_x;
	float fy = game->player.facing_y;
	float len = sqrtf(fx * fx + fy * fy);

	map_project(proj, game->player.x, game->player.y, &sx, &sy);
	if(!map_rect_has(&proj->area, sx, sy))
		return;
	if(len < 0.001f) {
		fx = 0.0f;
		fy = 1.0f;
	} else {
		fx /= len;
		fy /= len;
	}
	map_pointer_draw(style, sx, sy, fx, fy, size, (map_color){ 1.0f, 0.95f, 0.3f, 1.0f });
}
