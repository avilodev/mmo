// The sizes the font is baked at, and which one suits a size on screen.

#include "render/font_levels.h"

#include <math.h>

static int clamp_level(int level) {
	if(level < 0)
		return 0;
	return level > FONT_LEVEL_COUNT - 1 ? FONT_LEVEL_COUNT - 1 : level;
}

float font_level_size(int level, float base_size) {
	return base_size * powf(2.0f, (float)clamp_level(level) * 0.5f);
}

int font_level_for(float pixel_height, float base_size) {
	if(pixel_height <= 0.0f || base_size <= 0.0f)
		return 0;

	// Halves of an octave above the base, to the nearest
	return clamp_level((int)lroundf(2.0f * log2f(pixel_height / base_size)));
}
