/**
 * @file
 * Drawing calls that do nothing, so model code can be tested without a GPU.
 *
 * Some client modules mix a data model with the code that draws it --
 * inventory.c holds the bag *and* renders it -- so linking the model into a
 * test drags in the renderer, and the renderer drags in OpenGL and a window.
 *
 * These four stubs are the whole of what that costs. They are the drawing half
 * of the same idea as net_stubs.c: supply the names the source mentions so the
 * logic under test can be linked and exercised on a machine with no display.
 *
 * Included by a test, not compiled into anything shipped.
 */

#ifndef RENDER_STUBS_C
#define RENDER_STUBS_C

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

#endif /* RENDER_STUBS_C */
