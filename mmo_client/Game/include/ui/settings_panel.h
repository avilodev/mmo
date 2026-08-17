#ifndef SETTINGS_PANEL_H
#define SETTINGS_PANEL_H

#include "core/game_types.h"

/**
 * @file
 * Declare settings-panel layout shared by menu and gameplay states.
 */

/** Define the panel and slider geometry in screen pixels. */
#define SP_PW    420.0f
#define SP_PH    430.0f
#define SP_SX    150.0f
#define SP_SW    195.0f
#define SP_SH    14.0f
#define SP_ROW   36.0f

void sp_draw_slider(float px, float py, const char* label, float val,
                    float sr, float sg, float sb);

void sp_draw_checkbox(float px, float py, const char* label, int on);

void sp_draw_content(float px, float py, const GameSettings* s);

int sp_handle_mouse(float px, float py, float mx, float my,
                    int clicked, int held, GameSettings* s, float btn_y);

#endif // SETTINGS_PANEL_H
