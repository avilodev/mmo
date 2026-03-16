#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include "renderer.h"
#include <stdio.h>
#include <math.h>

static int screen_width;
static int screen_height;

// Internal font state
static stbtt_bakedchar baked_chars[96];
static GLuint font_texture;

// Cached GL texture state — avoids redundant glEnable/glDisable calls
static int g_texture_enabled = 1;

static void set_texture_enabled(int enable) {
    if (enable == g_texture_enabled) return;
    if (enable) glEnable(GL_TEXTURE_2D);
    else        glDisable(GL_TEXTURE_2D);
    g_texture_enabled = enable;
}

void renderer_init(int window_width, int window_height) {
    screen_width = window_width;
    screen_height = window_height;
    
    // Enable blending for transparency
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    
    // Enable 2D textures
    glEnable(GL_TEXTURE_2D);
    
    printf("Renderer initialized: %dx%d\n", window_width, window_height);
}

void renderer_clear(float r, float g, float b) {
    glClearColor(r, g, b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void renderer_begin_2d(void) {
    // Just set up the modelview matrix
    // The camera will handle the projection matrix
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

void renderer_end_2d(void) {
    // Nothing needed here for now
}

void renderer_draw_rect(float x, float y, float width, float height,
                        float r, float g, float b, float a) {
    set_texture_enabled(0);

    glColor4f(r, g, b, a);
    glBegin(GL_QUADS);
        glVertex2f(x, y);
        glVertex2f(x + width, y);
        glVertex2f(x + width, y + height);
        glVertex2f(x, y + height);
    glEnd();
}

void renderer_draw_sprite(float x, float y, float width, float height,
                          unsigned int texture_id) {
    set_texture_enabled(1);
    glBindTexture(GL_TEXTURE_2D, texture_id);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    
    glBegin(GL_QUADS);
        glTexCoord2f(0.0f, 0.0f); glVertex2f(x, y);
        glTexCoord2f(1.0f, 0.0f); glVertex2f(x + width, y);
        glTexCoord2f(1.0f, 1.0f); glVertex2f(x + width, y + height);
        glTexCoord2f(0.0f, 1.0f); glVertex2f(x, y + height);
    glEnd();
}

void renderer_draw_sprite_uv(float x, float y, float width, float height,
                             unsigned int texture_id,
                             float u0, float v0, float u1, float v1) {
    set_texture_enabled(1);
    glBindTexture(GL_TEXTURE_2D, texture_id);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    glBegin(GL_QUADS);
        glTexCoord2f(u0, v0); glVertex2f(x,         y);
        glTexCoord2f(u1, v0); glVertex2f(x + width, y);
        glTexCoord2f(u1, v1); glVertex2f(x + width, y + height);
        glTexCoord2f(u0, v1); glVertex2f(x,         y + height);
    glEnd();
}

void renderer_draw_sprite_uv_tinted(float x, float y, float width, float height,
                                    unsigned int texture_id,
                                    float u0, float v0, float u1, float v1,
                                    float r, float g, float b, float a) {
    set_texture_enabled(1);
    glBindTexture(GL_TEXTURE_2D, texture_id);
    glColor4f(r, g, b, a);
    glBegin(GL_QUADS);
        glTexCoord2f(u0, v0); glVertex2f(x,         y);
        glTexCoord2f(u1, v0); glVertex2f(x + width, y);
        glTexCoord2f(u1, v1); glVertex2f(x + width, y + height);
        glTexCoord2f(u0, v1); glVertex2f(x,         y + height);
    glEnd();
}

void renderer_draw_text_primitive(float x, float y, const char* text, float r, float g, float b) {
    // Very simple: draw a colored box per character
    for (int i = 0; text[i] != '\0'; i++) {
        renderer_draw_rect(x + i * 10, y, 8, 12, r, g, b, 1.0f);
    }
}

void renderer_font_init(const char* path, float size) {
    unsigned char* ttf_buffer = malloc(1<<20);
    unsigned char* temp_bitmap = malloc(512*512);

    if (!ttf_buffer || !temp_bitmap) {
        printf("Font allocation failed\n");
        free(ttf_buffer);
        free(temp_bitmap);
        return;
    }

    FILE* f = fopen(path, "rb");
    if (!f) {
        printf("Font not found: %s\n", path);
        free(ttf_buffer);
        free(temp_bitmap);
        return;
    }
    fread(ttf_buffer, 1, 1<<20, f);
    fclose(f);

    stbtt_BakeFontBitmap(ttf_buffer, 0, size, temp_bitmap, 512, 512, 32, 96, baked_chars);

    glGenTextures(1, &font_texture);
    glBindTexture(GL_TEXTURE_2D, font_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, 512, 512, 0, GL_ALPHA, GL_UNSIGNED_BYTE, temp_bitmap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

    free(ttf_buffer);
    free(temp_bitmap);
}

void renderer_draw_text(float x, float y, const char* text) {
    set_texture_enabled(1);
    glBindTexture(GL_TEXTURE_2D, font_texture);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f); // White text
    
    glBegin(GL_QUADS);
    while (*text) {
        if ((unsigned char)*text >= 32 && (unsigned char)*text < 128) {
            stbtt_aligned_quad q;
            stbtt_GetBakedQuad(baked_chars, 512, 512, *text - 32, &x, &y, &q, 1);
            glTexCoord2f(q.s0, q.t0); glVertex2f(q.x0, q.y0);
            glTexCoord2f(q.s1, q.t0); glVertex2f(q.x1, q.y0);
            glTexCoord2f(q.s1, q.t1); glVertex2f(q.x1, q.y1);
            glTexCoord2f(q.s0, q.t1); glVertex2f(q.x0, q.y1);
        }
        text++;
    }
    glEnd();
}

void renderer_draw_text_centered(float x, float y, float w, float h, const char* text) {
    float tw = 0;
    for (int i = 0; text[i]; i++) {
        if ((unsigned char)text[i] >= 32 && (unsigned char)text[i] < 128)
            tw += baked_chars[text[i]-32].xadvance;
    }
    // Adjust y by roughly half the font height (size/2) to center vertically
    renderer_draw_text(x + (w - tw) / 2.0f, y + (h / 2.0f) + 6.0f, text);
}

void renderer_draw_circle(float cx, float cy, float radius,
                          float r, float g, float b, float a, int segments) {
    set_texture_enabled(0);
    glColor4f(r, g, b, a);
    glBegin(GL_TRIANGLE_FAN);
    glVertex2f(cx, cy);
    for (int i = 0; i <= segments; i++) {
        float angle = 2.0f * 3.14159265f * (float)i / (float)segments;
        glVertex2f(cx + cosf(angle) * radius, cy + sinf(angle) * radius);
    }
    glEnd();
}

void renderer_draw_cone(float cx, float cy, float dir_x, float dir_y,
                        float radius, float angle_deg,
                        float r, float g, float b, float a, int segments) {
    set_texture_enabled(0);
    glColor4f(r, g, b, a);

    float base_angle = atan2f(dir_y, dir_x);
    float half_angle = angle_deg * 3.14159265f / 360.0f; // half in radians

    glBegin(GL_TRIANGLE_FAN);
    glVertex2f(cx, cy);
    for (int i = 0; i <= segments; i++) {
        float t = (float)i / (float)segments;
        float ang = base_angle - half_angle + t * 2.0f * half_angle;
        glVertex2f(cx + cosf(ang) * radius, cy + sinf(ang) * radius);
    }
    glEnd();
}

void renderer_cleanup(void) {
    if (font_texture) {
        glDeleteTextures(1, &font_texture);
        font_texture = 0;
    }
    printf("Renderer cleaned up\n");
}