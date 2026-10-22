#ifndef WEATHER_WINDOW_H
#define WEATHER_WINDOW_H

/**
 * @file
 * Declare the weather window: the forecast for where the player is, a week ahead.
 *
 * It shows the region the player stands in, whichever that is as they move.
 * Each day shows its condition, chance of rain or snow, and low and high.
 */

#include "core/game_types.h"
#include "ui/ui_kit.h"
#include "ui/weather_layout.h"

// Opens the window; it takes the game's input while it is open
void weather_window_open(void);
void weather_window_close(void);
int weather_window_is_open(void);

// Closed, for leaving a session
void weather_window_reset(void);

// Takes this frame's input while open, and keeps the game from seeing it
void weather_window_update(GameState* game);

// Zooms the map drawer when the mouse is over it; 1 when the scroll was used
int weather_window_scroll(const GameState* game, float notches);

void weather_window_draw(const GameState* game);


#endif /* WEATHER_WINDOW_H */
