#ifndef WEATHER_ICON_H
#define WEATHER_ICON_H

/**
 * @file
 * Declare how a weather condition is drawn as a small picture.
 *
 * A condition whose art is missing draws as a coloured placeholder box, so
 * the window works before the art does.
 */

#include "core/game_types.h"

// The picture file of each condition, and of the clear sky at night
const char* weather_icon_path(int condition);
const char* weather_icon_night_path(void);

// Draw a condition at x, y with a side of size; night only changes a clear sky
void weather_icon_draw(const GameTextures* textures, int condition, int night, float x,
		float y, float size);

#endif /* WEATHER_ICON_H */
