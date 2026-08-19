#ifndef RENDERER_H
#define RENDERER_H

#include <GLFW/glfw3.h>

// Initialize the renderer
void renderer_init(int window_width, int window_height);

// Clear the screen with a color
void renderer_clear(float r, float g, float b);

// Begin 2D rendering mode
void renderer_begin_2d(void);

// Leave world space and establish screen space for UI drawing.
//
// Every overlay drawn after this call is in screen coordinates: (0,0) at the
// top-left of the logical viewport passed to renderer_init. This is a one-way
// transition for the frame's UI phase and does not need a matching call.
void renderer_end_2d(void);

// Enter screen space for a self-contained overlay, saving the current matrices.
//
// Use this when a panel must draw in screen coordinates regardless of what the
// caller had bound, and must leave the caller's matrices untouched. Screen space
// is already active after renderer_end_2d, so overlays in the normal UI phase
// only need this if they cannot rely on that ordering.
void renderer_begin_screen_space(void);

// Restore the matrices saved by renderer_begin_screen_space.
void renderer_end_screen_space(void);
 
// Draw a colored rectangle
void renderer_draw_rect(float x, float y, float width, float height,
                        float r, float g, float b, float a);

// Draw a textured sprite (full texture)
void renderer_draw_sprite(float x, float y, float width, float height,
                          unsigned int texture_id);

// Draw a sub-region of a texture using normalized UV coordinates [0..1]
// u0,v0 = top-left of region; u1,v1 = bottom-right of region
void renderer_draw_sprite_uv(float x, float y, float width, float height,
                             unsigned int texture_id,
                             float u0, float v0, float u1, float v1);

// Same but with a tint color / alpha (pass 1,1,1,1 for no tint)
void renderer_draw_sprite_uv_tinted(float x, float y, float width, float height,
                                    unsigned int texture_id,
                                    float u0, float v0, float u1, float v1,
                                    float r, float g, float b, float a);

void renderer_draw_text_primitive(float x, float y, const char* text, float r, float g, float b);

void renderer_font_init(const char* path, float size);

void renderer_draw_text(float x, float y, const char* text);

void renderer_draw_text_centered(float x, float y, float w, float h, const char* text);

// Draw a filled circle (for telegraphs, zones)
void renderer_draw_circle(float cx, float cy, float radius,
                          float r, float g, float b, float a, int segments);

// Draw a filled cone (for telegraph cones)
void renderer_draw_cone(float cx, float cy, float dir_x, float dir_y,
                        float radius, float angle_deg,
                        float r, float g, float b, float a, int segments);

// Clean up renderer resources
void renderer_cleanup(void);

#endif // RENDERER_H