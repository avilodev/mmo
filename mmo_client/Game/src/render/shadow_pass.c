// Draw the shadows of a frame into the shadow maps.

#include "render/shadow_pass.h"
#include "render/city_renderer.h"
#include "render/character_renderer.h"
#include "render/lamp_shadow.h"

#include <math.h>
#include <string.h>

// Half-width of each map's box, world units: a close map and a wide one that
// reaches far across the view
static const float RADIUS[SHADOW_CASCADES] = { 300.0f, 1000.0f };
// How far from the focus a body is still drawn into each map
static const float CASTER_RANGE[SHADOW_CASCADES] = { 480.0f, 1000.0f };
// Frames between drawings of each map; the wide one is kept a frame
static const int REDRAW_FRAMES[SHADOW_CASCADES] = { 1, 2 };
// Below this the shadows are too faint to be worth the drawing
#define SHADOW_MIN_STRENGTH 0.01f

static shadow_cascade g_cascade[SHADOW_CASCADES];
static int g_active;
// Per map: holds a drawing, its age in frames, and where it was centred
static int g_drawn[SHADOW_CASCADES];
static int g_age[SHADOW_CASCADES];
static float g_center[SHADOW_CASCADES][2];

// Whether this frame has to draw a map again
static int map_due(int cascade, float focus_x, float focus_y) {
	float dx = focus_x - g_center[cascade][0];
	float dy = focus_y - g_center[cascade][1];
	int age = g_drawn[cascade] ? g_age[cascade] : -1;

	return shadow_cascade_redraw_due(age, sqrtf(dx * dx + dy * dy), RADIUS[cascade],
	                                 REDRAW_FRAMES[cascade]);
}

void shadow_pass_run(const world_lighting* light, float focus_x, float focus_y, float focus_h) {
	const float center[3] = { focus_x, focus_h, focus_y };

	g_active = 0;
	// Collected every frame, so the list never goes stale
	character_renderer_shadows_begin();
	// The lamps shine at night, when the sun's own shadows may be gone
	lamp_shadow_run(light->lamp_amount, focus_x, focus_y);
	if(!shadow_map_ready() || light->shadow_strength < SHADOW_MIN_STRENGTH) {
		// The maps go stale while the shadows are off
		memset(g_drawn, 0, sizeof(g_drawn));
		return;
	}

	for(int c = 0; c < SHADOW_CASCADES; c++) {
		g_age[c]++;
		if(!map_due(c, focus_x, focus_y))
			continue;
		shadow_cascade_fit(&g_cascade[c], light->dir, center, RADIUS[c], SHADOW_MAP_SIZE);
		shadow_map_begin(c);
		city_renderer_draw_shadow(g_cascade[c].viewproj);
		character_renderer_draw_shadows_near(g_cascade[c].viewproj, focus_x, focus_y,
		                                     CASTER_RANGE[c]);
		shadow_map_end();
		g_drawn[c] = 1;
		g_age[c] = 0;
		g_center[c][0] = focus_x;
		g_center[c][1] = focus_y;
	}
	g_active = 1;
}

int shadow_pass_active(void) {
	return g_active;
}

const shadow_cascade* shadow_pass_cascade(int cascade) {
	return &g_cascade[cascade];
}
