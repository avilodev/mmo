/**
 * @file
 * Run the weather window: input and drawing.
 */

#include "ui/weather_window.h"
#include "ui/weather_drawer.h"
#include "ui/weather_icon.h"
#include "ui/weather_layout.h"
#include "ui/window_pos.h"
#include "ui/ui_kit.h"
#include "core/keybinds.h"
#include "input/input.h"
#include "renderer.h"
#include "world/weather_state.h"
#include "world/weather_now.h"
#include "world/world_clock.h"

#include <GLFW/glfw3.h>
#include <stdio.h>
#include <string.h>

#define TITLE_SIZE 30.0f
#define BIG_TEMP_SIZE 84.0f
#define CONDITION_SIZE 32.0f
#define DETAIL_SIZE 24.0f
#define DAY_SIZE 27.0f
#define NOTE_SIZE 19.0f
#define PLACE_SIZE 38.0f
#define TEMP_SIZE 26.0f
#define NOW_ICON_SIZE 130.0f
#define BAR_SLICES 24
#define SMALL_SIZE 19.0f


// Chance of rain or snow below this is not worth printing
#define PRECIP_SHOWN_FROM 20

static int s_open;
static int s_drawer_open; // the map drawer beside the window
static window_pos s_pos; // where the window sits; centred until dragged

static const kit_color PRECIP_BLUE = { 0.55f, 0.75f, 1.00f };
static const kit_color WIND_TINT = { 0.70f, 0.88f, 0.92f };

int weather_window_is_open(void) {
	return s_open;
}

void weather_window_open(void) {
	s_open = 1;
}

void weather_window_close(void) {
	s_open = 0;
	s_drawer_open = 0;
	weather_drawer_release();
}

void weather_window_reset(void) {
	weather_window_close();
	window_pos_reset(&s_pos);
}

static kit_frame weather_window_frame(const GameState* game) {
	kit_frame frame = ui_kit_frame(game->camera.viewport_width, game->camera.viewport_height,
	                               game->settings.ui_scale);

	frame.scale *= WEATHER_FRAME_SHRINK;
	frame.w /= WEATHER_FRAME_SHRINK;
	frame.h /= WEATHER_FRAME_SHRINK;

	return frame;
}

// The window's place: where it was dragged to, else centred in the frame
static weather_rect current_panel(const kit_frame* frame) {
	weather_rect panel = weather_panel(frame->w, frame->h);

	window_pos_origin(&s_pos, frame->w, frame->h, panel.w, panel.h, &panel.x, &panel.y);

	return panel;
}


// Take a key press so the game does not also act on it
static void take_key(InputState* in, int key) {
	if(key < 0 || key > GLFW_KEY_LAST)
		return;

	in->keys_just_pressed[key] = 0;
	in->keys_pressed[key] = 0;
}

// Take the mouse, for a click or drag on the window itself
static void take_mouse(InputState* in) {
	in->mouse_left_clicked = 0;
	in->mouse_left_down = 0;
	in->mouse_right_clicked = 0;
	in->mouse_right_down = 0;
	memset(in->mouse_buttons_down, 0, sizeof(in->mouse_buttons_down));
	memset(in->mouse_buttons_pressed, 0, sizeof(in->mouse_buttons_pressed));
}

// The window floats: it takes its own clicks and the closing key, nothing else
void weather_window_update(GameState* game) {
	InputState* in = &game->input;
	kit_frame frame = weather_window_frame(game);
	weather_rect panel = current_panel(&frame);
	weather_rect close = weather_close_button(&panel);
	weather_rect arrow = weather_arrow_button(&panel);
	weather_rect drawer = weather_drawer_panel(&panel);
	// Typing a Y into the chat box is not the key to close it
	int typing = game->playing && game->playing->chat.is_typing;
	int held = 0;
	float mx;
	float my;

	ui_kit_to_layout(&frame, in->mouse_x, in->mouse_y, &mx, &my);

	// With the map open Esc is the map's to close first
	if(input_key_just_pressed(in, GLFW_KEY_ESCAPE) && !(game->playing && game->playing->show_map)) {
		take_key(in, GLFW_KEY_ESCAPE);
		weather_window_close();
		return;
	}
	if(!typing && input_key_just_pressed(in, g_keybinds.toggle_weather)) {
		take_key(in, g_keybinds.toggle_weather);
		weather_window_close();
		return;
	}
	if(in->mouse_left_clicked && weather_rect_has(&close, mx, my)) {
		take_mouse(in);
		weather_window_close();
		return;
	}

	// The arrow slides the map drawer out, or in; the image is freed while it is in
	if(in->mouse_left_clicked && weather_rect_has(&arrow, mx, my)) {
		s_drawer_open = !s_drawer_open;
		if(!s_drawer_open)
			weather_drawer_release();
		take_mouse(in);
		return;
	}
	if(s_drawer_open)
		weather_drawer_update(game, &frame, &drawer, mx, my);

	held = window_pos_drag(&s_pos, frame.w, frame.h, panel.w, panel.h, panel.w - close.w - 30.0f,
	                       close.y - panel.y + close.h + 8.0f, mx, my, in->mouse_left_clicked,
	                       in->mouse_left_down);

	// A click or drag on the window is the window's, not the world's
	if(held || weather_drawer_dragging() || ((in->mouse_left_clicked || in->mouse_right_clicked) &&
	            (weather_rect_has(&panel, mx, my) || weather_rect_has(&arrow, mx, my) ||
		             (s_drawer_open && weather_rect_has(&drawer, mx, my)))))
		take_mouse(in);
}

// Blue when cold through green to orange when hot
static kit_color temp_color(int celsius) {
	static const struct {
		int degrees;
		kit_color color;
	} stops[] = {
		{ -10, { 0.35f, 0.55f, 1.00f } },
		{ 5, { 0.40f, 0.85f, 0.95f } },
		{ 15, { 0.55f, 0.90f, 0.45f } },
		{ 25, { 1.00f, 0.85f, 0.30f } },
		{ 35, { 1.00f, 0.50f, 0.20f } }
	};
	int last = (int)(sizeof(stops) / sizeof(stops[0])) - 1;

	if(celsius <= stops[0].degrees)
		return stops[0].color;
	if(celsius >= stops[last].degrees)
		return stops[last].color;

	for(int i = 1; i <= last; i++) {
		if(celsius <= stops[i].degrees) {
			float t = (float)(celsius - stops[i - 1].degrees) /
			          (float)(stops[i].degrees - stops[i - 1].degrees);
			kit_color from = stops[i - 1].color;
			kit_color to = stops[i].color;
			kit_color mixed = { from.r + (to.r - from.r) * t, from.g + (to.g - from.g) * t,
			                    from.b + (to.b - from.b) * t };

			return mixed;
		}
	}

	return stops[last].color;
}

// "1h 12m", "12m" or "40s": how long until the forecast moves on
static void format_wait(uint32_t ms, char* out, size_t size) {
	uint32_t seconds = (ms + 999u) / 1000u;

	if(seconds >= 3600u)
		snprintf(out, size, "%uh %um", seconds / 3600u, (seconds % 3600u) / 60u);
	else if(seconds >= 60u)
		snprintf(out, size, "%um", seconds / 60u);
	else
		snprintf(out, size, "%us", seconds);
}

// The bar a day's low and high stretch across, coloured by degrees
static void draw_range_bar(const weather_rect* bar, int scale_lo, int scale_hi,
		const WeatherDayData* day) {
	float x;
	float w;
	float slice;

	renderer_draw_rect(bar->x, bar->y, bar->w, bar->h, 0.0f, 0.0f, 0.0f, 0.45f);
	weather_bar_span(bar, scale_lo, scale_hi, day->temp_lo, day->temp_hi, &x, &w);

	slice = w / (float)BAR_SLICES;
	for(int i = 0; i < BAR_SLICES; i++) {
		float t = ((float)i + 0.5f) / (float)BAR_SLICES;
		kit_color c = temp_color(day->temp_lo + (int)((float)(day->temp_hi - day->temp_lo) * t));

		renderer_draw_rect(x + slice * (float)i, bar->y, slice + 0.5f, bar->h, c.r, c.g, c.b,
		                   1.0f);
	}
}

// The coldest low and warmest high of the days that are known
static void temperature_scale(int region, int* lo, int* hi) {
	WeatherDayData day;
	int found = 0;

	*lo = 0;
	*hi = 0;
	for(int ahead = 0; ahead < WEATHER_FORECAST_DAYS; ahead++) {
		if(!weather_state_day_now(region, ahead, &day))
			continue;
		if(!found || day.temp_lo < *lo)
			*lo = day.temp_lo;
		if(!found || day.temp_hi > *hi)
			*hi = day.temp_hi;
		found = 1;
	}
}

static void draw_temp(float x, float cy, int celsius, int use_celsius, kit_align align) {
	char text[16];

	weather_temp_text(celsius, use_celsius, text, sizeof(text));
	ui_kit_text(x, cy, text, TEMP_SIZE, KIT_WHITE, align);
}

// The block at the top: what it is like right now
static void draw_now(const GameState* game, const weather_rect* panel, int region,
		int use_celsius) {
	weather_rect block = weather_now_block(panel);
	WeatherDayData today;
	char name[40];
	char overhead[48];
	char speeds[24];
	char line[96];
	float hour = world_clock_synced() ? world_clock_hour_now() : 12.0f;
	float text_x = block.x + NOW_ICON_SIZE + 34.0f;
	char hi_text[16];
	char lo_text[16];
	char big[16];

	if(!weather_state_day_now(region, 0, &today))
		return;

	weather_icon_draw(&game->textures, today.condition, weather_is_night(hour), block.x + 10.0f,
	                  block.y + (block.h - NOW_ICON_SIZE) * 0.5f, NOW_ICON_SIZE);

	weather_temp_text(weather_temp_at_hour(&today, hour), use_celsius, big, sizeof(big));
	ui_kit_text(text_x, block.y + 42.0f, big, BIG_TEMP_SIZE, KIT_WHITE, KIT_LEFT);

	// High and low sit at the right, one over the other
	weather_temp_text(today.temp_hi, use_celsius, hi_text, sizeof(hi_text));
	weather_temp_text(today.temp_lo, use_celsius, lo_text, sizeof(lo_text));
	snprintf(line, sizeof(line), "High %s", hi_text);
	ui_kit_text(block.x + block.w - 6.0f, block.y + 30.0f, line, DETAIL_SIZE, KIT_DIM, KIT_RIGHT);
	snprintf(line, sizeof(line), "Low %s", lo_text);
	ui_kit_text(block.x + block.w - 6.0f, block.y + 58.0f, line, DETAIL_SIZE, KIT_DIM, KIT_RIGHT);

	// The day, with how hard it falls; and what it is doing over the player this minute
	weather_day_name(&today, name, sizeof(name));
	ui_kit_text(text_x, block.y + 100.0f, name, CONDITION_SIZE, KIT_GOLD, KIT_LEFT);
	weather_now_describe(overhead, sizeof(overhead));
	snprintf(line, sizeof(line), "Overhead now: %s", overhead);
	ui_kit_text(text_x, block.y + 136.0f, line, DETAIL_SIZE, KIT_DIM, KIT_LEFT);
	weather_wind_range_text(today.wind, use_celsius, speeds, sizeof(speeds));
	snprintf(line, sizeof(line), "Wind: %s, %s", weather_wind_name(today.wind), speeds);
	ui_kit_text(text_x, block.y + 168.0f, line, DETAIL_SIZE, WIND_TINT, KIT_LEFT);
}

static void draw_day(const GameState* game, const weather_rect* panel, int region, int ahead,
		int scale_lo, int scale_hi, int use_celsius) {
	weather_rect row = weather_day_row(panel, ahead);
	weather_rect icon = weather_row_icon(&row);
	weather_rect precip = weather_row_precip(&row);
	weather_rect lo = weather_row_lo(&row);
	weather_rect bar = weather_row_bar(&row);
	weather_rect hi = weather_row_hi(&row);
	WeatherDayData day;
	char label[24];
	char short_name[32];
	char percent[8];
	float cy = row.y + row.h * 0.5f;

	if(!weather_state_day_now(region, ahead, &day))
		return;

	if(ahead % 2 == 0) {
		renderer_draw_rect(row.x - 8.0f, row.y, row.w + 16.0f, row.h, 1.0f, 1.0f, 1.0f, 0.05f);
	}

	// The day above, what it brings below it
	weather_day_label(ahead, label, sizeof(label));
	ui_kit_text(row.x, cy - 10.0f, label, DAY_SIZE, KIT_WHITE, KIT_LEFT);
	weather_day_short_name(&day, short_name, sizeof(short_name));
	ui_kit_text(row.x, cy + 17.0f, short_name, SMALL_SIZE, KIT_DIM, KIT_LEFT);

	weather_icon_draw(&game->textures, day.condition, 0, icon.x, icon.y, icon.w);

	if(day.precip_pct >= PRECIP_SHOWN_FROM) {
		snprintf(percent, sizeof(percent), "%d%%", day.precip_pct);
		ui_kit_text(precip.x + precip.w * 0.5f, cy - 10.0f, percent, DAY_SIZE - 3.0f, PRECIP_BLUE,
		            KIT_CENTER);
	}
	ui_kit_text(precip.x + precip.w * 0.5f, cy + 17.0f, weather_wind_name(day.wind), SMALL_SIZE,
	            WIND_TINT, KIT_CENTER);

	draw_temp(lo.x + lo.w, cy, day.temp_lo, use_celsius, KIT_RIGHT);
	draw_range_bar(&bar, scale_lo, scale_hi, &day);
	draw_temp(hi.x, cy, day.temp_hi, use_celsius, KIT_LEFT);
}

static void draw_footer(const weather_rect* panel) {
	weather_rect foot = weather_footer(panel);
	char wait[16];
	char text[96];

	format_wait(weather_state_ms_to_next_day_now(), wait, sizeof(wait));
	snprintf(text, sizeof(text), "Updates in %s. Forecasts can change.", wait);
	ui_kit_text(foot.x + foot.w * 0.5f, foot.y + foot.h * 0.5f, text, NOTE_SIZE + 2.0f, KIT_DIM,
	            KIT_CENTER);
}

// The window, its title and its close button
static void draw_chrome(const weather_rect* panel, float mx, float my) {
	weather_rect close = weather_close_button(panel);

	renderer_draw_rect(panel->x, panel->y, panel->w, panel->h, 0.05f, 0.05f, 0.09f, 0.97f);
	ui_kit_panel(panel->x, panel->y, panel->w, panel->h);
	ui_kit_text(panel->x + panel->w * 0.5f, panel->y + 32.0f, "Weather", TITLE_SIZE, KIT_GOLD,
	            KIT_CENTER);
	ui_kit_close_button(close.x, close.y, close.w, weather_rect_has(&close, mx, my));
}

// The arrow on the right edge that slides the map drawer out and in
static void draw_arrow(const weather_rect* panel, int drawer_open, float mx, float my) {
	weather_rect arrow = weather_arrow_button(panel);

	ui_kit_button(arrow.x, arrow.y, arrow.w, arrow.h, drawer_open ? "<" : ">", 0,
	              weather_rect_has(&arrow, mx, my) ? KIT_BTN_HOVER : KIT_BTN_NORMAL);
}

int weather_window_scroll(const GameState* game, float notches) {
	kit_frame frame;
	weather_rect panel;
	weather_rect drawer;
	float mx;
	float my;

	if(!s_open || !s_drawer_open || notches == 0.0f)
		return 0;

	frame = weather_window_frame(game);
	panel = current_panel(&frame);
	drawer = weather_drawer_panel(&panel);
	ui_kit_to_layout(&frame, game->input.mouse_x, game->input.mouse_y, &mx, &my);

	return weather_drawer_zoom(&drawer, mx, my, notches);
}

void weather_window_draw(const GameState* game) {
	kit_frame frame;
	weather_rect panel;
	weather_rect place;
	float mx;
	float my;
	int region = weather_region_at(game->player.x, game->player.y);
	int scale_lo;
	int scale_hi;

	if(!s_open)
		return;

	frame = weather_window_frame(game);
	panel = current_panel(&frame);
	place = weather_place_block(&panel);
	ui_kit_to_layout(&frame, game->input.mouse_x, game->input.mouse_y, &mx, &my);

	ui_kit_begin(&frame);
	draw_chrome(&panel, mx, my);

	if(!weather_state_synced()) {
		ui_kit_text(panel.x + panel.w * 0.5f, panel.y + panel.h * 0.5f,
		            "No forecast has arrived yet.", DAY_SIZE, KIT_DIM, KIT_CENTER);
		ui_kit_end();
		return;
	}

	ui_kit_text(place.x + place.w * 0.5f, place.y + place.h * 0.5f, weather_region_name(region),
	            PLACE_SIZE, KIT_GOLD, KIT_CENTER);

	temperature_scale(region, &scale_lo, &scale_hi);
	draw_now(game, &panel, region, game->settings.temp_celsius);
	for(int ahead = 0; ahead < WEATHER_FORECAST_DAYS; ahead++)
		draw_day(game, &panel, region, ahead, scale_lo, scale_hi, game->settings.temp_celsius);
	draw_footer(&panel);
	if(s_drawer_open) {
		weather_rect drawer = weather_drawer_panel(&panel);

		weather_drawer_draw(game, &frame, &drawer, mx, my);
	}
	draw_arrow(&panel, s_drawer_open, mx, my);
	ui_kit_end();
}
