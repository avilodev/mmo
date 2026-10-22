#ifndef KEYBINDS_PAGE_H
#define KEYBINDS_PAGE_H

/**
 * @file
 * Declare the key bindings page of the settings menu.
 *
 * Everything is in layout units (ui/ui_kit.h) and the pointer is given in the
 * same units.
 */

#include "core/game_types.h"

// Called as the page is shown and hidden
void kp_enter(void);
void kp_leave(void);

// Where the page sits; set before the calls below, every frame
void kp_set_area(float x, float y, float w, float h);

void kp_draw(float mx, float my);

// A left click at (mx, my)
void kp_click(float mx, float my);

// Bind the next key or button pressed to the action being rebound
void kp_poll_keys(float mx, float my, const InputState* input);

// Scroll by wheel notches; nonzero when the page took them
int kp_scroll(float notches);

// Escape cancels a rebind; nonzero when there was one to cancel
int kp_escape(void);

// A key is being waited for, so other keys must not act as shortcuts
int kp_capturing(void);

#endif // KEYBINDS_PAGE_H
