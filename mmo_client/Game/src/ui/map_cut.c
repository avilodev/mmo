/**
 * @file
 * Decide whether the map shows roofs or a floor plan.
 */
#include "ui/map_cut.h"
#include "ui/map_terrain.h"
#include "world_spans.h"
#include "world_spans_format.h"

// How far under the ceiling the cut falls, world units.
#define CUT_BELOW_CEIL 2.0f

map_cut_info map_cut_compute(const GameState* game) {
	map_cut_info cut = { 0, MAP_TERRAIN_NO_CLIP };
	world_span spans[WORLD_SPANS_MAX_PER_TILE];
	float z = game->player.visual_z;
	int best = -1;

	if(!world_spans_loaded())
		return cut;

	int count = world_spans_at(game->player.x, game->player.y, spans, WORLD_SPANS_MAX_PER_TILE);

	for(int i = 0; i < count; i++)
		if(spans[i].floor <= z + 12.0f && (best < 0 || spans[i].floor > spans[best].floor))
			best = i;
	if(best >= 0 && spans[best].ceil < WORLD_SPAN_OPEN_CEIL * 0.5f) {
		cut.indoors = 1;
		cut.cut_height = spans[best].ceil - CUT_BELOW_CEIL;
	}

	return cut;
}
