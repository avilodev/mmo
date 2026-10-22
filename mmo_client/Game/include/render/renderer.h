#ifndef RENDERER_H
#define RENDERER_H

#include <GLFW/glfw3.h>
#include "render/letterbox.h"

// Initialize the renderer
void renderer_init(int window_width, int window_height);

// Clear the screen with a color
void renderer_clear(float r, float g, float b);

// Paint black the margin around the letterboxed view, once the frame is drawn
void renderer_clear_bars(int fb_width, int fb_height, const letterbox_rect* box);

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

void renderer_draw_text_primitive(float x, float y, const char* text, float r, float g, float b);

void renderer_font_init(const char* path, float size);

void renderer_draw_text(float x, float y, const char* text);

/** As renderer_draw_text(), in a colour (components 0 to 1). */
void renderer_draw_text_colored(float x, float y, const char* text,
                                float r, float g, float b);

void renderer_draw_text_centered(float x, float y, float w, float h, const char* text);

/** The font is also baked at larger sizes (render/font_levels.h), so big text is
 *  drawn from a bake near its size instead of one bake stretched.
 *  The level whose bake is nearest a pixel height on screen: */
int renderer_font_level_for(float pixel_height);

/** The pixel height a level was baked at. */
float renderer_font_level_size(int level);

/** As renderer_text_width() and renderer_draw_text_colored(), in a level's bake. */
float renderer_text_width_level(const char* text, int level);
void renderer_draw_text_level(float x, float y, const char* text, int level,
                              float r, float g, float b);

/** Measure text in the shared baked font, in pixels.
 *
 * Bytes outside printable ASCII contribute nothing, exactly as they draw nothing.
 */
float renderer_text_width(const char* text);

/** Draw text broken to fit a width, honouring explicit newlines.
 *
 * Wrapping happens on whitespace; a single word longer than max_width is drawn
 * over the edge rather than split, because splitting a word is worse than
 * overrunning by a few pixels and this is prose, not a table.
 *
 * @param x, y        Baseline of the first line.
 * @param max_width   Width to wrap within, in pixels.
 * @param line_height Baseline-to-baseline spacing.
 * @return            How many lines were drawn, so a caller can size a panel.
 */
int renderer_draw_text_wrapped(float x, float y, float max_width, float line_height,
                               const char* text);

/** Count the lines renderer_draw_text_wrapped() would draw, without drawing them. */
int renderer_measure_text_wrapped(float max_width, const char* text);

// Draw a filled circle (for telegraphs, zones)
void renderer_draw_circle(float cx, float cy, float radius,
                          float r, float g, float b, float a, int segments);

// Draw a flat ring between two radii (selection markers on the ground)
void renderer_draw_ring(float cx, float cy, float inner_radius, float outer_radius,
                        float r, float g, float b, float a, int segments);

// Draw a filled cone (for telegraph cones)
void renderer_draw_cone(float cx, float cy, float dir_x, float dir_y,
                        float radius, float angle_deg,
                        float r, float g, float b, float a, int segments);

// Draw a texture on a quad with these corners, in order (the texture's top-left,
// top-right, bottom-right, bottom-left), tinted by a colour
void renderer_draw_texture_quad(unsigned int texture_id, const float corners[8],
                                float r, float g, float b, float a);

// Draw a filled triangle between three points
void renderer_draw_triangle(float x0, float y0, float x1, float y1, float x2, float y2,
                            float r, float g, float b, float a);

/** Raise everything drawn until renderer_lift_end() by `z` world units, so a
 *  ground marking lies on the floor it belongs to. In the world pass the third
 *  coordinate is height (camera.h). */
void renderer_lift_begin(float z);
void renderer_lift_end(void);

// Clean up renderer resources
void renderer_cleanup(void);

#endif // RENDERER_H