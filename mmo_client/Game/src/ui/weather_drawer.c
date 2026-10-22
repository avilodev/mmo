/**
 * @file
 * Run and draw the weather drawer: the city from above under the storm cover.
 */

#include "ui/weather_drawer.h"
#include "ui/map_draw.h"
#include "ui/map_markers.h"
#include "ui/map_terrain.h"
#include "ui/map_weather.h"
#include "ui/weather_drawer_view.h"
#include "render/city_renderer.h"
#include "render/gl_loader.h"
#include "render/renderer.h"

// The biggest the city image gets on either side. It is the size of the box, not
// of the world, and it is drawn again only when the view moves.
#define MAP_PX_MAX 2048


// A scroll notch zooms this much, the same as the full map
#define ZOOM_PER_NOTCH 0.15f
#define ZOOM_FLOOR 0.2f

// Reset from the whole-city view zooms in this far
#define NEAR_ZOOM 2.0f
#define CENTRE_ZOOM 3.0f

static map_terrain_target s_terrain;
static map_weather_cover s_cover;
static weather_cam s_cam;
static int s_have_cam;
static int s_follow;   // the view stays on the player
static int s_dragging;

void weather_drawer_release(void) {
	map_terrain_release(&s_terrain);
	map_weather_cover_release(&s_cover);
	s_have_cam = 0;
	s_follow = 0;
	s_dragging = 0;
}

int weather_drawer_dragging(void) {
	return s_dragging;
}

// The city's footprint, and where the map looks (the whole city until moved)
static int look(weather_bounds* bounds) {
	if(!city_renderer_bounds(&bounds->min_x, &bounds->min_y, &bounds->max_x, &bounds->max_y))
		return 0;

	if(!s_have_cam) {
		s_cam = weather_cam_whole(bounds);
		s_have_cam = 1;
	}

	return 1;
}

static void centre_on_player(const GameState* game, const weather_bounds* bounds,
                             const weather_rect* inner) {
	s_cam.cam_x = game->player.x;
	s_cam.cam_y = game->player.y;
	weather_drawer_clamp(bounds, inner, &s_cam);
}

void weather_drawer_update(GameState* game, const kit_frame* frame, const weather_rect* drawer,
                           float mx, float my) {
	InputState* in = &game->input;
	weather_rect inner = weather_drawer_inner(drawer);
	weather_rect button = weather_drawer_recenter_button(&inner);
	weather_bounds bounds;

	if(!look(&bounds))
		return;

	if(in->mouse_left_clicked) {
		if(!s_follow && weather_rect_has(&button, mx, my)) {
			s_follow = 1;
			s_dragging = 0;
			if(s_cam.zoom < NEAR_ZOOM)
				s_cam.zoom = CENTRE_ZOOM;
		} else {
			s_dragging = weather_rect_has(&inner, mx, my);
		}
	}
	if(!in->mouse_left_down)
		s_dragging = 0;

	if(s_dragging && (in->mouse_dx != 0.0f || in->mouse_dy != 0.0f)) {
		s_follow = 0;
		weather_drawer_pan(&bounds, &inner, &s_cam, in->mouse_dx / frame->scale,
		                   in->mouse_dy / frame->scale);
	}

	weather_drawer_clamp(&bounds, &inner, &s_cam);
	if(s_follow)
		centre_on_player(game, &bounds, &inner);
}

int weather_drawer_zoom(const weather_rect* drawer, float mx, float my, float notches) {
	weather_rect inner = weather_drawer_inner(drawer);
	weather_bounds bounds;
	float factor = 1.0f + notches * ZOOM_PER_NOTCH;

	if(!weather_rect_has(&inner, mx, my) || !look(&bounds))
		return 0;

	if(factor < ZOOM_FLOOR)
		factor = ZOOM_FLOOR;

	// Following the player keeps the player in the middle, so zoom about it
	if(s_follow) {
		mx = inner.x + inner.w * 0.5f;
		my = inner.y + inner.h * 0.5f;
	}
	weather_drawer_zoom_at(&bounds, &inner, &s_cam, factor, mx, my);

	return 1;
}

static void draw_chrome(const weather_rect* drawer) {
	renderer_draw_rect(drawer->x, drawer->y, drawer->w, drawer->h, 0.05f, 0.05f, 0.09f, 0.97f);
	ui_kit_panel(drawer->x, drawer->y, drawer->w, drawer->h);
}

void weather_drawer_draw(const GameState* game, const kit_frame* frame,
                         const weather_rect* drawer, float mx, float my) {
	weather_rect inner = weather_drawer_inner(drawer);
	weather_rect button = weather_drawer_recenter_button(&inner);
	map_rect box = { inner.x, inner.y, inner.w, inner.h };
	map_terrain_view view;
	map_projection proj;
	weather_bounds bounds;
	GLint viewport[4];
	unsigned int terrain;

	draw_chrome(drawer);

	if(!look(&bounds))
		return;

	glGetIntegerv(GL_VIEWPORT, viewport);
	if(!weather_drawer_view(&bounds, &inner, &s_cam, (float)viewport[2] / frame->w, MAP_PX_MAX,
	                        &view, &proj))
		return;

	terrain = map_terrain_render(&s_terrain, &view);
	if(!terrain)
		return;

	renderer_draw_sprite_uv(box.x, box.y, box.w, box.h, terrain, 0.0f, 1.0f, 1.0f, 0.0f);
	map_weather_cover_draw(&s_cover, &box, &proj);

	map_clip_begin(&box, frame->w);
	map_markers_draw_self_sized(game, &proj, WEATHER_POINTER_UNITS, MAP_POINTER_DEFAULT);
	map_clip_end();

	if(!s_follow)
		ui_kit_button(button.x, button.y - 2, button.w, button.h, "Reset", 0,
		              weather_rect_has(&button, mx, my) ? KIT_BTN_HOVER : KIT_BTN_NORMAL);
}
