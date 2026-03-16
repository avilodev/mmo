#ifndef SETTINGS_PANEL_H
#define SETTINGS_PANEL_H

#include "core/game_types.h"

// ============================================================================
// SETTINGS PANEL  — shared by state_settings.c and state_playing.c
// ============================================================================

#define SP_PW    420.0f
#define SP_PH    350.0f
#define SP_SX    150.0f
#define SP_SW    195.0f
#define SP_SH    14.0f
#define SP_ROW   36.0f

// Draw a labeled slider row at (px, py)
void sp_draw_slider(float px, float py, const char* label, float val,
                    float sr, float sg, float sb);

// Draw a labeled checkbox row at (px, py)
void sp_draw_checkbox(float px, float py, const char* label, int on);

// Draw the full panel content (title bar, audio sliders, display section)
void sp_draw_content(float px, float py, const GameSettings* s);

// Handle mouse input for the panel.
// Returns 1 if the close/back button was clicked, 0 otherwise.
// btn_y: top-y of the close/back button (caller positions it).
int sp_handle_mouse(float px, float py, float mx, float my,
                    int clicked, int held, GameSettings* s, float btn_y);

#endif // SETTINGS_PANEL_H
