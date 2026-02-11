#ifndef RENDERER_H
#define RENDERER_H

#include <GLFW/glfw3.h>

// Initialize the renderer
void renderer_init(int window_width, int window_height);

// Clear the screen with a color
void renderer_clear(float r, float g, float b);

// Begin 2D rendering mode
void renderer_begin_2d(void);

// End 2D rendering mode
void renderer_end_2d(void);
 
// Draw a colored rectangle
void renderer_draw_rect(float x, float y, float width, float height,
                        float r, float g, float b, float a);

// Draw a textured sprite
void renderer_draw_sprite(float x, float y, float width, float height,
                          unsigned int texture_id);

void renderer_draw_text_primitive(float x, float y, const char* text, float r, float g, float b);

void renderer_font_init(const char* path, float size);

void renderer_draw_text(float x, float y, const char* text);

void renderer_draw_text_centered(float x, float y, float w, float h, const char* text);

// Clean up renderer resources
void renderer_cleanup(void);

#endif // RENDERER_H