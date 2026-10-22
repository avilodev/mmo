#ifndef CHAR_SELECT_PREVIEW_H
#define CHAR_SELECT_PREVIEW_H

// The selected character's body drawn into one tall texture, standing and
// facing the viewer, for the middle of the character select screen.

#include <stdint.h>

// The picture's size in pixels
#define CS_PREVIEW_PX_W 640
#define CS_PREVIEW_PX_H 960

// Draws the body in its race and look; needs the character model and GL.
// A NULL race_key draws the body with no race parts. `yaw` turns it, in radians,
// 0 facing the viewer.
void char_select_preview_render(const char* race_key, uint32_t appearance, float yaw, float dt);

// The texture, or 0 before it has been drawn
unsigned int char_select_preview_texture(void);

// Frees the texture and the framebuffer
void char_select_preview_release(void);

#endif /* CHAR_SELECT_PREVIEW_H */
