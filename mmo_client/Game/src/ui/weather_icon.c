/**
 * @file
 * Draw a weather condition as a picture, or a placeholder when it has none.
 */

#include "ui/weather_icon.h"
#include "renderer.h"

static const char* const ICON_PATHS[WEATHER_CONDITION_COUNT] = {
	"Game/Sprites/UI/Hud/weather_sun.png",
	"Game/Sprites/UI/Hud/weather_partly_cloudy.png",
	"Game/Sprites/UI/Hud/weather_cloudy.png",
	"Game/Sprites/UI/Hud/weather_rain.png",
	"Game/Sprites/UI/Hud/weather_thunder.png",
	"Game/Sprites/UI/Hud/weather_snow.png"
};

#define NIGHT_ICON_PATH "Game/Sprites/UI/Hud/weather_clear_night.png"

// The colour of a condition's placeholder box
typedef struct {
	float r;
	float g;
	float b;
} swatch;

static const swatch SWATCHES[WEATHER_CONDITION_COUNT] = {
	{ 1.00f, 0.80f, 0.20f },
	{ 0.92f, 0.86f, 0.55f },
	{ 0.65f, 0.68f, 0.72f },
	{ 0.35f, 0.55f, 0.85f },
	{ 0.50f, 0.40f, 0.75f },
	{ 0.85f, 0.92f, 1.00f }
};

static const swatch NIGHT_SWATCH = { 0.15f, 0.20f, 0.45f };

const char* weather_icon_path(int condition) {
	if(condition < 0 || condition >= WEATHER_CONDITION_COUNT)
		return "";
	return ICON_PATHS[condition];
}

const char* weather_icon_night_path(void) {
	return NIGHT_ICON_PATH;
}

// A flat box with a border and a lighter middle: "art goes here"
static void draw_placeholder(const swatch* color, float x, float y, float size) {
	float edge = size * 0.08f;
	float core = size * 0.28f;

	if(edge < 1.0f)
		edge = 1.0f;

	renderer_draw_rect(x, y, size, size, 0.05f, 0.05f, 0.08f, 0.9f);
	renderer_draw_rect(x + edge, y + edge, size - 2.0f * edge, size - 2.0f * edge, color->r,
	                   color->g, color->b, 0.9f);
	renderer_draw_rect(x + core, y + core, size - 2.0f * core, size - 2.0f * core, 1.0f, 1.0f,
	                   1.0f, 0.35f);
}

void weather_icon_draw(const GameTextures* textures, int condition, int night, float x,
		float y, float size) {
	unsigned int texture;
	const swatch* color;

	if(condition < 0 || condition >= WEATHER_CONDITION_COUNT)
		return;

	if(condition == WEATHER_CLEAR && night) {
		texture = textures->weather_clear_night;
		color = &NIGHT_SWATCH;
	} else {
		texture = textures->weather_icons[condition];
		color = &SWATCHES[condition];
	}

	if(texture)
		renderer_draw_sprite(x, y, size, size, texture);
	else
		draw_placeholder(color, x, y, size);
}
