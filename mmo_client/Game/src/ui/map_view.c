/**
 * @file
 * The full map screen: terrain, marks, tooltips and readouts.
 */
#include "ui/map_view.h"
#include "ui/map_cut.h"
#include "ui/map_draw.h"
#include "ui/map_entities.h"
#include "ui/map_grid.h"
#include "ui/map_layout.h"
#include "ui/map_markers.h"
#include "ui/map_minimap.h"
#include "ui/map_sidebar.h"
#include "ui/map_terrain.h"
#include "ui/map_weather.h"
#include "ui/ui_kit.h"
#include "ui/weather_layout.h"
#include "render/renderer.h"

#include <GLFW/glfw3.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define PAN_MARGIN      0.12f
#define TIP_LINES       16

static map_entity_list g_entities;
static map_terrain_target g_terrain;

static const map_color INK   = { 0.93f, 0.95f, 1.0f, 1.0f };
static const map_color MUTED = { 0.60f, 0.67f, 0.80f, 1.0f };
static const map_color GOLD  = { 1.0f, 0.84f, 0.28f, 1.0f };

static map_rect recenter_button(const map_rect* area) {
	// Bottom right, just above the scale bar
	return (map_rect){ area->x + area->w - 64.0f, area->y + area->h - 70.0f, 56.0f, 22.0f };
}

// The button in the title bar that puts the storm cover on and takes it off
static map_rect weather_button(const map_layout* layout) {
	return (map_rect){ layout->title.x + layout->title.w - 300.0f, layout->title.y + 6.0f, 110.0f,
	                   22.0f };
}

static int is_panned(const PlayingState* ps) {
	return fabsf(ps->map_pan_x) > 0.5f || fabsf(ps->map_pan_y) > 0.5f;
}

// How far out the map may zoom: far enough to hold the city, no further.
static float min_zoom(const map_rect* area) {
	float x0, y0, x1, y1;
	float floor_zoom = MAP_ZOOM_MIN;

	if(city_renderer_bounds(&x0, &y0, &x1, &y1)) {
		float width = x1 - x0;
		float height = (y1 - y0) * (area->w / area->h);
		float wanted = MAP_BASE_RADIUS / (0.5f * (width > height ? width : height) * 1.12f);

		if(wanted > floor_zoom)
			floor_zoom = wanted;
	}
	if(floor_zoom > MAP_ZOOM_MAX)
		floor_zoom = MAP_ZOOM_MAX;

	return floor_zoom;
}

// Keep the middle of the view inside the city and a margin around it.
static void clamp_pan(PlayingState* ps, float player_x, float player_y) {
	float x0, y0, x1, y1;

	if(!city_renderer_bounds(&x0, &y0, &x1, &y1))
		return;

	float mx = (x1 - x0) * PAN_MARGIN;
	float my = (y1 - y0) * PAN_MARGIN;
	float cam_x = player_x + ps->map_pan_x;
	float cam_y = player_y + ps->map_pan_y;

	cam_x = fminf(fmaxf(cam_x, x0 - mx), x1 + mx);
	cam_y = fminf(fmaxf(cam_y, y0 - my), y1 + my);
	ps->map_pan_x = cam_x - player_x;
	ps->map_pan_y = cam_y - player_y;
}

static void build_projection(const GameState* game, const map_rect* area, map_projection* out) {
	const PlayingState* ps = game->playing;

	map_projection_make(out, area, game->player.x + ps->map_pan_x,
	                    game->player.y + ps->map_pan_y, ps->map_zoom);
}

void map_view_update(GameState* game) {
	PlayingState* ps = game->playing;
	map_layout layout;
	map_rect button;

	if(!ps)
		return;
	if(!ps->show_map) {
		ps->map_pan_x = 0.0f;
		ps->map_pan_y = 0.0f;
		ps->map_dragging = 0;
		return;
	}
	map_layout_compute((float)game->camera.viewport_width,
	                   (float)game->camera.viewport_height, &layout);
	float lowest = min_zoom(&layout.area);

	if(ps->map_zoom < lowest)
		ps->map_zoom = lowest;
	button = recenter_button(&layout.area);
	if(game->input.mouse_left_clicked) {
		map_rect weather = weather_button(&layout);

		if(map_rect_has(&weather, game->input.mouse_x, game->input.mouse_y)) {
			ps->map_weather = !ps->map_weather;
			return;
		}
		if(is_panned(ps) && map_rect_has(&button, game->input.mouse_x, game->input.mouse_y)) {
			ps->map_pan_x = 0.0f;
			ps->map_pan_y = 0.0f;
		} else {
			ps->map_dragging = map_rect_has(&layout.area, game->input.mouse_x,
			                                game->input.mouse_y);
		}
	}
	if(!game->input.mouse_left_down)
		ps->map_dragging = 0;
	if(ps->map_dragging) {
		map_projection proj;

		build_projection(game, &layout.area, &proj);
		ps->map_pan_x -= game->input.mouse_dx / proj.k;
		ps->map_pan_y -= game->input.mouse_dy / proj.k;
		clamp_pan(ps, game->player.x, game->player.y);
	}
}

// Draw the terrain texture, or say why there is none.
static void draw_terrain(const map_rect* a, unsigned int texture) {
	if(texture) {
		renderer_draw_sprite_uv(a->x, a->y, a->w, a->h, texture, 0.0f, 1.0f, 1.0f, 0.0f);
		return;
	}
	map_fill(a, (map_color){ 0.04f, 0.05f, 0.07f, 1.0f });
	map_text(a->x + 16.0f, a->y + 30.0f, MAP_TEXT_HEAD, "The city could not be drawn.", MUTED);
}

// Fill the lines naming each quest this NPC gives, what it asks and what it pays.
static int describe_available(const GameState* game, const map_entity* e, char text[][96],
                              map_color* colors, int room) {
	const available_quest_entry* quests[2];
	int found = quest_log_available_from(&game->playing->quest_log, (uint8_t)e->npc_type,
	                                     quests, 2);
	int n = 0;

	if(found == 0 && e->quest_mark == NPC_QUEST_MARK_AVAILABLE && room > 0) {
		snprintf(text[n], 96, "Quest available: %s", e->mark_title);
		colors[n++] = GOLD;
	}
	for(int i = 0; i < found; i++) {
		char reward[90];
		const available_quest_entry* q = quests[i];

		if(n + 2 + q->obj_count > room)
			break;
		snprintf(text[n], 96, "Quest available: %s", q->title);
		colors[n++] = GOLD;
		for(int j = 0; j < q->obj_count; j++) {
			snprintf(text[n], 96, "  %s", q->objectives[j].description);
			colors[n++] = INK;
		}
		quest_log_available_reward_text(q, reward, sizeof(reward));
		snprintf(text[n], 96, "  %s", reward);
		colors[n++] = MUTED;
	}

	return n;
}

// Fill the tooltip lines for an entity; returns how many.
static int describe_entity(const GameState* game, const map_entity* e, char text[][96],
                           map_color* colors) {
	int n = 0;

	if(e->level > 0) {
		snprintf(text[n], 96, "%s  Lv %d", e->name, e->level);
	} else {
		snprintf(text[n], 96, "%s", e->name);
	}
	colors[n++] = map_kind_color(e->kind);
	snprintf(text[n], 96, "%s%s%s", map_kind_label(e->kind), e->elite ? ", elite" : "",
	         e->dead ? ", down" : "");
	colors[n++] = MUTED;
	if(e->has_health && !e->dead) {
		snprintf(text[n], 96, "Health  %d / %d  (%d%%)", e->health, e->max_health,
		         e->max_health > 0 ? e->health * 100 / e->max_health : 0);
		colors[n++] = INK;
	}
	if(e->quest_mark == NPC_QUEST_MARK_READY) {
		snprintf(text[n], 96, "Ready to hand in: %s", e->mark_title);
		colors[n++] = GOLD;
	}
	if(e->has_npc_type && e->quest_mark != NPC_QUEST_MARK_NONE)
		n += describe_available(game, e, text + n, colors + n, TIP_LINES - n - 3);
	for(int i = 0; i < e->quest_count; i++) {
		if(strcmp(e->quests[i], e->mark_title) == 0)
			continue;
		snprintf(text[n], 96, "Quest: %s", e->quests[i]);
		colors[n++] = GOLD;
	}
	map_describe_offset(text[n], 96, e->x - game->player.x, e->y - game->player.y,
	                    e->z - game->player.visual_z);
	colors[n++] = INK;
	if(e->detail[0]) {
		snprintf(text[n], 96, "%s", e->detail);
		colors[n++] = GOLD;
	}

	return n;
}

static void draw_tooltip(const GameState* game, const map_entity* e, const map_rect* keep) {
	char text[TIP_LINES][96];
	const char* lines[TIP_LINES];
	map_color colors[TIP_LINES];
	int n = describe_entity(game, e, text, colors);

	for(int i = 0; i < n; i++)
		lines[i] = text[i];
	map_text_box(game->input.mouse_x + 16.0f, game->input.mouse_y + 14.0f, lines, colors, n,
	             keep);
}

static void draw_button(const map_rect* r, const char* label, int hot) {
	map_fill(r, (map_color){ hot ? 0.24f : 0.15f, hot ? 0.34f : 0.22f, hot ? 0.5f : 0.32f, 0.96f });
	map_outline(r, 1.5f, (map_color){ 0.45f, 0.6f, 0.82f, 1.0f });
	map_text(r->x + 10.0f, r->y + 15.0f, MAP_TEXT_BODY, label, INK);
}

static void draw_title(const GameState* game, const map_layout* layout,
                       const map_cut_info* cut) {
	const map_rect* t = &layout->title;
	const char* zone = game->playing->current_zone_name;
	char text[96];

	map_fill(t, (map_color){ 0.10f, 0.12f, 0.19f, 1.0f });
	map_text(t->x + 12.0f, t->y + 23.0f, MAP_TEXT_HEAD, "MAP", INK);
	snprintf(text, sizeof(text), "%s", zone[0] ? zone : "Ennara");
	map_text(t->x + 62.0f, t->y + 23.0f, MAP_TEXT_BODY, text, MUTED);
	if(cut->indoors)
		snprintf(text, sizeof(text), "FLOOR PLAN  cut at %.1f m", map_metres(cut->cut_height));
	else
		snprintf(text, sizeof(text), "ROOFS ON");
	map_text(t->x + t->w * 0.5f - 90.0f, t->y + 23.0f, MAP_TEXT_BODY, text,
	         cut->indoors ? GOLD : INK);
	map_text(t->x + t->w - 150.0f, t->y + 23.0f, MAP_TEXT_BODY, "M / Esc to close", MUTED);
}

static void draw_status(const GameState* game, const map_layout* layout,
                        const map_projection* proj, int over_map) {
	const map_rect* s = &layout->status;
	char text[160];
	float wx, wy;
	float x = s->x + 12.0f;

	map_fill(s, (map_color){ 0.08f, 0.09f, 0.14f, 1.0f });
	snprintf(text, sizeof(text), "Zoom %.1fx   Grid %.0f m", game->playing->map_zoom,
	         map_grid_step_metres(proj));
	map_text(x, s->y + 17.0f, MAP_TEXT_BODY, text, MUTED);
	if(!over_map)
		return;
	map_unproject(proj, game->input.mouse_x, game->input.mouse_y, &wx, &wy);
	snprintf(text, sizeof(text), "Cursor  %.0f m %s, %.0f m %s of the courtyard",
	         fabsf(map_metres(wx - CITY_ORIGIN_X)), wx >= CITY_ORIGIN_X ? "E" : "W",
	         fabsf(map_metres(wy - CITY_ORIGIN_Y)), wy >= CITY_ORIGIN_Y ? "S" : "N");
	map_text(x + 250.0f, s->y + 17.0f, MAP_TEXT_BODY, text, INK);
	map_describe_offset(text, sizeof(text), wx - game->player.x, wy - game->player.y, 0.0f);
	map_text(x + 250.0f + 520.0f, s->y + 17.0f, MAP_TEXT_BODY, text, MUTED);
}

// The player's pointer scales with the window like the weather map's, 1.5 times as big
#define POINTER_GROWTH 1.5f

static float pointer_size(const GameState* game) {
	kit_frame frame = ui_kit_frame(game->camera.viewport_width, game->camera.viewport_height,
	                               game->settings.ui_scale);

	return WEATHER_POINTER_UNITS * frame.scale * WEATHER_FRAME_SHRINK * POINTER_GROWTH;
}

// Draw everything that lives on the map area, cut to its edges.
static void draw_area(const GameState* game, const map_layout* layout,
                      const map_projection* proj, const map_cut_info* cut, int hovered) {
	unsigned int terrain = map_terrain_render_area(&g_terrain, proj,
	                                               (float)game->camera.viewport_width,
	                                               cut->cut_height);

	map_clip_begin(&layout->area, (float)game->camera.viewport_width);
	draw_terrain(&layout->area, terrain);
	if(game->playing->map_weather)
		map_weather_draw(&layout->area, proj);
	map_grid_draw(proj);
	map_markers_begin();
	map_markers_draw_hazards(game, proj);
	map_markers_draw_entities(&g_entities, proj, hovered, 1);
	map_markers_draw_self_sized(game, proj, pointer_size(game), MAP_POINTER_DEFAULT);
	map_scale_bar_draw(proj);
	map_compass_draw(&layout->area);
	map_clip_end();
}

void map_view_render(GameState* game) {
	if(!game->playing->show_map)
		return;

	float vw = (float)game->camera.viewport_width;
	float vh = (float)game->camera.viewport_height;
	map_layout layout;
	map_projection proj;
	map_cut_info cut = map_cut_compute(game);
	int over_map;
	int hovered = -1;

	map_layout_compute(vw, vh, &layout);
	build_projection(game, &layout.area, &proj);

	map_entities_gather(game, &g_entities);
	over_map = map_rect_has(&layout.area, game->input.mouse_x, game->input.mouse_y);
	if(over_map && !game->playing->map_dragging)
		hovered = map_markers_hit(&g_entities, &proj, game->input.mouse_x, game->input.mouse_y);

	renderer_begin_screen_space();
	renderer_draw_rect(0, 0, vw, vh, 0.0f, 0.0f, 0.0f, 0.7f);
	map_fill(&layout.panel, (map_color){ 0.06f, 0.07f, 0.11f, 0.98f });
	draw_area(game, &layout, &proj, &cut, hovered);
	map_outline(&layout.area, 1.5f, (map_color){ 0.3f, 0.4f, 0.58f, 1.0f });
	if(is_panned(game->playing)) {
		map_rect button = recenter_button(&layout.area);

		draw_button(&button, "Reset", map_rect_has(&button, game->input.mouse_x,
		                                                  game->input.mouse_y - 2));
	}
	map_sidebar_draw(game, &layout.side, &g_entities, &cut);
	draw_title(game, &layout, &cut);
	{
		map_rect weather = weather_button(&layout);

		draw_button(&weather, "Weather", game->playing->map_weather);
	}
	draw_status(game, &layout, &proj, over_map);
	map_outline(&layout.panel, 2.0f, (map_color){ 0.4f, 0.5f, 0.65f, 1.0f });
	if(hovered >= 0)
		draw_tooltip(game, &g_entities.items[hovered], &layout.panel);
	renderer_end_screen_space();
}

void map_view_shutdown(void) {
	map_terrain_release(&g_terrain);
	map_weather_release();
	map_minimap_release();
}
