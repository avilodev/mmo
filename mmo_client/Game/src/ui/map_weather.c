/**
 * @file
 * Paint the weather over the full map as a translucent layer.
 */

#include "ui/map_weather.h"
#include "render/renderer.h"
#include "texture/texture.h"
#include "world/weather_field.h"
#include "world/weather_state.h"

#include <GLFW/glfw3.h>
#include <string.h>

#define COLUMNS 128
#define MAX_ROWS 128

// The map's details must still read through the cover
#define COVER_OPACITY 0.62f

static map_weather_cover s_map_cover;

// Today's weather in every region; 0 when the forecast has not arrived
static int gather_today(WeatherDayData* out) {
	int count = weather_state_region_count();

	if(count > WEATHER_REGION_MAX)
		count = WEATHER_REGION_MAX;
	for(int r = 0; r < count; r++)
		if(!weather_state_day_now(r, 0, &out[r]))
			return 0;

	return count;
}

static int same_view(const map_weather_painted* p, const map_rect* area, const map_projection* proj,
                     const WeatherDayData* days, int count) {
	return p->valid && p->count == count && memcmp(&p->proj, proj, sizeof(*proj)) == 0 &&
	       memcmp(&p->area, area, sizeof(*area)) == 0 &&
	       memcmp(p->days, days, sizeof(WeatherDayData) * (size_t)count) == 0;
}

// Sample the weather at every cell of the visible ground and upload it
static void paint(map_weather_cover* cover, const map_rect* area, const map_projection* proj,
                  const WeatherDayData* days, int count) {
	static unsigned char pixels[COLUMNS * MAX_ROWS * 4];
	int rows = (int)((float)COLUMNS * area->h / area->w + 0.5f);

	if(rows < 1)
		rows = 1;
	if(rows > MAX_ROWS)
		rows = MAX_ROWS;

	for(int y = 0; y < rows; y++) {
		for(int x = 0; x < COLUMNS; x++) {
			float sx = area->x + ((float)x + 0.5f) / (float)COLUMNS * area->w;
			float sy = area->y + ((float)y + 0.5f) / (float)rows * area->h;
			unsigned char* px = &pixels[((size_t)y * COLUMNS + (size_t)x) * 4];
			weather_sample sample;
			float rgba[4];
			float wx;
			float wy;

			map_unproject(proj, sx, sy, &wx, &wy);
			weather_field_blend(days, count, wx, wy, &sample);
			weather_field_roughen(&sample, wx, wy, 17u);
			weather_field_color(WEATHER_LAYER_CLOUDS, &sample, rgba);
			px[0] = (unsigned char)(rgba[0] * 255.0f + 0.5f);
			px[1] = (unsigned char)(rgba[1] * 255.0f + 0.5f);
			px[2] = (unsigned char)(rgba[2] * 255.0f + 0.5f);
			px[3] = (unsigned char)(rgba[3] * COVER_OPACITY * 255.0f + 0.5f);
		}
	}

	if(!cover->texture)
		glGenTextures(1, &cover->texture);
	glBindTexture(GL_TEXTURE_2D, cover->texture);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, COLUMNS, rows, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);
}

void map_weather_cover_draw(map_weather_cover* cover, const map_rect* area,
                            const map_projection* proj) {
	WeatherDayData days[WEATHER_REGION_MAX];
	int count = gather_today(days);

	if(count == 0)
		return;

	if(!cover->texture || !same_view(&cover->painted, area, proj, days, count)) {
		paint(cover, area, proj, days, count);
		cover->painted.valid = 1;
		cover->painted.proj = *proj;
		cover->painted.area = *area;
		cover->painted.count = count;
		memcpy(cover->painted.days, days, sizeof(WeatherDayData) * (size_t)count);
	}

	renderer_draw_sprite_uv(area->x, area->y, area->w, area->h, cover->texture, 0.0f, 0.0f, 1.0f,
	                        1.0f);
}

void map_weather_cover_release(map_weather_cover* cover) {
	if(cover->texture)
		texture_unload(cover->texture);
	memset(cover, 0, sizeof(*cover));
}

void map_weather_draw(const map_rect* area, const map_projection* proj) {
	map_weather_cover_draw(&s_map_cover, area, proj);
}

void map_weather_release(void) {
	map_weather_cover_release(&s_map_cover);
}
