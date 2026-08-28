/**
 * @file
 * Drawing calls that do nothing, so model code can be tested without a GPU.
 *
 * Some client modules mix a data model with the code that draws it --
 * inventory.c holds the bag *and* renders it -- so linking the model into a
 * test drags in the renderer, and the renderer drags in OpenGL and a window.
 *
 * These few stubs are the whole of what that costs. They are the drawing half
 * of the same idea as net_stubs.c: supply the names the source mentions so the
 * logic under test can be linked and exercised on a machine with no display.
 *
 * Included by a test, not compiled into anything shipped.
 */

#ifndef RENDER_STUBS_C
#define RENDER_STUBS_C

#include <stddef.h>

void renderer_begin_screen_space(void) {}
void renderer_end_screen_space(void) {}

void renderer_draw_rect(float x, float y, float width, float height,
                        float r, float g, float b, float a) {
    (void)x; (void)y; (void)width; (void)height;
    (void)r; (void)g; (void)b; (void)a;
}

void renderer_draw_text(float x, float y, const char* text) {
    (void)x; (void)y; (void)text;
}

void renderer_draw_text_centered(float x, float y, float w, float h, const char* text) {
    (void)x; (void)y; (void)w; (void)h; (void)text;
}

/** Report a width from the character count.
 *
 * Zero would be wrong in a way that hides bugs: callers right-align against
 * this, and every one of them would land at the same place. The real renderer
 * is monospaced-ish at this size, so a per-character estimate keeps layout
 * arithmetic under test meaningful.
 */
float renderer_text_width(const char* text) {
    if (!text) return 0.0f;
    size_t n = 0;
    while (text[n]) n++;
    return (float)n * 9.0f;
}

#endif /* RENDER_STUBS_C */
