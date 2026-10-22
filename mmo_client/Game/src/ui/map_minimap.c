/**
 * @file
 * Draw the minimap: the nearby city and what stands in it.
 */
#include "ui/map_minimap.h"
#include "ui/map_cut.h"
#include "ui/map_draw.h"
#include "ui/map_entities.h"
#include "ui/map_markers.h"
#include "ui/map_terrain.h"
#include "render/renderer.h"

// The player's pointer, 1.5 times its last size
#define POINTER_SIZE 2.55f

static map_entity_list    g_entities;
static map_terrain_target g_terrain;

void map_minimap_render(const GameState* game, const map_rect* box) {
	map_projection proj;
	map_cut_info cut = map_cut_compute(game);
	float zoom = MAP_BASE_RADIUS / (MINIMAP_RADIUS_M * CITY_UNITS_PER_METRE);
	float viewport_w = (float)game->camera.viewport_width;
	unsigned int terrain;

	map_projection_make(&proj, box, game->player.x, game->player.y, zoom);
	terrain = map_terrain_render_area(&g_terrain, &proj, viewport_w, cut.cut_height);
	if(terrain)
		renderer_draw_sprite_uv(box->x, box->y, box->w, box->h, terrain, 0.0f, 1.0f, 1.0f, 0.0f);

	map_entities_gather(game, &g_entities);
	map_clip_begin(box, viewport_w);
	map_markers_begin();
	map_markers_draw_hazards(game, &proj);
	map_markers_draw_entities(&g_entities, &proj, -1, 0);
	map_markers_draw_self_sized(game, &proj, POINTER_SIZE, MAP_POINTER_DEFAULT);
	map_clip_end();
}

void map_minimap_release(void) {
	map_terrain_release(&g_terrain);
}
