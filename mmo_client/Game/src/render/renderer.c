#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include "renderer.h"
#include <stdio.h>

static int screen_width;
static int screen_height;

// Internal font state
static stbtt_bakedchar baked_chars[96]; 
static GLuint font_texture;

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
    glDisable(GL_TEXTURE_2D);
    
    glColor4f(r, g, b, a);
    glBegin(GL_QUADS);
        glVertex2f(x, y);
        glVertex2f(x + width, y);
        glVertex2f(x + width, y + height);
        glVertex2f(x, y + height);
    glEnd();
    
    glEnable(GL_TEXTURE_2D);
}

void renderer_draw_sprite(float x, float y, float width, float height, 
                          unsigned int texture_id) {
    glBindTexture(GL_TEXTURE_2D, texture_id);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    
    glBegin(GL_QUADS);
        glTexCoord2f(0.0f, 0.0f); glVertex2f(x, y);
        glTexCoord2f(1.0f, 0.0f); glVertex2f(x + width, y);
        glTexCoord2f(1.0f, 1.0f); glVertex2f(x + width, y + height);
        glTexCoord2f(0.0f, 1.0f); glVertex2f(x, y + height);
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

    FILE* f = fopen(path, "rb");
    if (!f) { printf("Font not found: %s\n", path); return; }
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
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, font_texture);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f); // White text
    
    glBegin(GL_QUADS);
    while (*text) {
        if (*text >= 32 && *text < 128) { 
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
        if (text[i] >= 32 && text[i] < 128) 
            tw += baked_chars[text[i]-32].xadvance;
    }
    // Adjust y by roughly half the font height (size/2) to center vertically
    renderer_draw_text(x + (w - tw) / 2.0f, y + (h / 2.0f) + 6.0f, text);
}

void renderer_cleanup(void) {
    printf("Renderer cleaned up\n");
}