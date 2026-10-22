#ifndef SKY_RENDERER_H
#define SKY_RENDERER_H

// Draw the sky behind the world: a gradient from the horizon to the zenith,
// the sun and moon, and stars at night. One fullscreen triangle, one shader.

#include "camera/camera_math.h"
#include "world/sky.h"

// Compile the shader. Needs gl_loader_init(). Returns nonzero on success;
// failures are logged, and sky_renderer_draw then does nothing.
int sky_renderer_init(void);
void sky_renderer_shutdown(void);

// Paint the whole view with the sky. Draw it first, before any depth is written.
void sky_renderer_draw(const CameraView* view, const sky_state* sky);

#endif /* SKY_RENDERER_H */
