#ifndef SETTINGS_PANEL_H
#define SETTINGS_PANEL_H

#include "core/game_types.h"

/**
 * @file
 * Declare settings-panel layout shared by menu and gameplay states.
 */

/** Define the panel and slider geometry in screen pixels. */
#define SP_PW    420.0f
#define SP_PH    510.0f
#define SP_SX    150.0f
#define SP_SW    195.0f
#define SP_SH    14.0f
#define SP_ROW   36.0f

/** Every row the panel draws, in the order it draws them.
 *
 * The draw pass and the hit-test pass used to compute row positions
 * separately, from the same constants but by different arithmetic -- so adding
 * a row meant getting the same offset right twice, and getting it wrong meant
 * a control that drew in one place and responded in another. Both now index
 * this enum through sp_row_y().
 */
typedef enum {
    SP_ROW_MASTER_VOLUME,
    SP_ROW_MUSIC_VOLUME,
    SP_ROW_SFX_VOLUME,
    SP_ROW_SHOW_FPS,
    SP_ROW_FULLSCREEN,
    SP_ROW_VSYNC,
    SP_ROW_FPS_LIMIT,
    SP_ROW_UI_SCALE,
    SP_ROW_COUNT,
} SettingsRow;

/** Frame limits the cap cycles through, in order. The first is "uncapped". */
#define SP_FPS_LIMIT_CHOICES 6
extern const int SP_FPS_LIMITS[SP_FPS_LIMIT_CHOICES];

/** Y offset of one row, relative to the panel origin. */
float sp_row_y(float py, SettingsRow row);

void sp_draw_slider(float px, float py, const char* label, float val,
                    float sr, float sg, float sb);

void sp_draw_checkbox(float px, float py, const char* label, int on);

void sp_draw_content(float px, float py, const GameSettings* s);

int sp_handle_mouse(float px, float py, float mx, float my,
                    int clicked, int held, GameSettings* s, float btn_y);

#endif // SETTINGS_PANEL_H
