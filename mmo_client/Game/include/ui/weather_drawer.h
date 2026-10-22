#ifndef WEATHER_DRAWER_H
#define WEATHER_DRAWER_H

// The drawer beside the weather window: the city from above with today's weather
// over it and the player's pointer, and nothing else from the map. It zooms (scroll),
// drags, and brings itself back to the player on the Reset button.

#include "core/game_types.h"
#include "ui/ui_kit.h"
#include "ui/weather_layout.h"

// Take this frame's clicks and mouse movement on the map; (mx, my) is the mouse in layout units
void weather_drawer_update(GameState* game, const kit_frame* frame, const weather_rect* drawer,
                           float mx, float my);

// Zoom by scroll notches if the mouse is over the map; 1 when the scroll was used
int weather_drawer_zoom(const weather_rect* drawer, float mx, float my, float notches);

// Nonzero while the map is being dragged
int weather_drawer_dragging(void);

// Draw the drawer in the weather window's layout units
void weather_drawer_draw(const GameState* game, const kit_frame* frame,
                         const weather_rect* drawer, float mx, float my);

// Free the map image and the cover and forget where the map was looking
void weather_drawer_release(void);

#endif /* WEATHER_DRAWER_H */
