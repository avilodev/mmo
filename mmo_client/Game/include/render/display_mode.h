#ifndef DISPLAY_MODE_H
#define DISPLAY_MODE_H

/**
 * @file
 * Switch the screen into a real exclusive display mode for Fullscreen.
 *
 * GLFW only changes the display mode when the one asked for differs from the
 * current one, so Fullscreen at the screen's own size left the desktop alone
 * and behaved like a borderless window. This asks Windows for the mode
 * directly, as a temporary mode that resets when the game exits.
 */

// Put the primary screen into w x h at hz; nonzero when Windows accepted it
int display_mode_enter(int w, int h, int hz);

// Give the primary screen back its normal mode
void display_mode_leave(void);

#endif /* DISPLAY_MODE_H */
