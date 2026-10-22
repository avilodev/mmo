#ifndef PRECIP_RENDERER_H
#define PRECIP_RENDERER_H

// Rain streaks and snow flakes falling round the camera's focus. Every drop is
// worked out in the vertex shader from its number and the shared weather time,
// so there are no buffers: the renderer only says how many to draw.

#include "camera/camera_math.h"

// Compile the shader. Nonzero on success; without it nothing falls.
int precip_renderer_init(void);

void precip_renderer_shutdown(void);

// Draw the rain or snow of weather_now_get() round the world point (x, y) at
// a ground height h. Call after the world and the bodies, before the 2D pass.
void precip_renderer_draw(const CameraView* view, float x, float y, float h);

#endif /* PRECIP_RENDERER_H */
