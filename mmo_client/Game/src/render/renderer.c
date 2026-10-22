/**
 * @file
 * Provide immediate-mode OpenGL drawing and baked-font rendering for the client.
 */

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include "renderer.h"
#include "render/font_levels.h"
#include <stdio.h>
#include <math.h>
#include <string.h>
#include "core/client_log.h"

static int screen_width;
static int screen_height;

// Internal font state
static stbtt_bakedchar baked_chars[96];
static GLuint font_texture;

// The same font baked at the sizes of font_levels.h; level 0 is the one above
static stbtt_bakedchar level_chars[FONT_LEVEL_COUNT][96];
static GLuint level_texture[FONT_LEVEL_COUNT];
static int level_atlas[FONT_LEVEL_COUNT];
static float level_base_size = 22.0f;

// Cached GL texture state — avoids redundant glEnable/glDisable calls
static int g_texture_enabled = 1;

static void set_texture_enabled(int enable) {
    if (enable == g_texture_enabled) 
        return;
    if (enable) 
        glEnable(GL_TEXTURE_2D);
    else        
        glDisable(GL_TEXTURE_2D);
    g_texture_enabled = enable;
}

/**
 * Initialize renderer state for a logical viewport.
 *
 * A current OpenGL context must exist before this call.
 */
void renderer_init(int window_width, int window_height) {
    screen_width = window_width;
    screen_height = window_height;
    
    // Enable blending for transparency
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    
    // Enable 2D textures
    glEnable(GL_TEXTURE_2D);
    
    CLOG_INFO("Renderer initialized: %dx%d", window_width, window_height);
}

/**
 * Clear the color and depth buffers with an opaque color.
 */
void renderer_clear(float r, float g, float b) {
    glClearColor(r, g, b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

/**
 * Paint the margin around the letterboxed view black.
 *
 * Done last, with the scissor test, because glClear ignores the viewport and
 * the scene's own clear would otherwise colour the bars.
 */
void renderer_clear_bars(int fb_width, int fb_height, const letterbox_rect* box) {
	// Left and right, then below and above
	const int bars[4][4] = {
		{ 0, 0, box->x, fb_height },
		{ box->x + box->w, 0, fb_width - box->x - box->w, fb_height },
		{ box->x, 0, box->w, box->y },
		{ box->x, box->y + box->h, box->w, fb_height - box->y - box->h },
	};

	if(box->x == 0 && box->y == 0 && box->w == fb_width && box->h == fb_height)
		return;

	glViewport(0, 0, fb_width, fb_height);
	glEnable(GL_SCISSOR_TEST);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

	for(int i = 0; i < 4; i++) {
		if(bars[i][2] <= 0 || bars[i][3] <= 0)
			continue;

		glScissor(bars[i][0], bars[i][1], bars[i][2], bars[i][3]);
		glClear(GL_COLOR_BUFFER_BIT);
	}

	glDisable(GL_SCISSOR_TEST);
}

/**
 * Reset the model-view matrix for a 2D drawing pass.
 */
void renderer_begin_2d(void) {
    // Just set up the modelview matrix
    // The camera will handle the projection matrix
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

/**
 * Finish a 2D drawing pass.
 */
void renderer_end_2d(void) {
    // Establish screen space for the frame's UI phase.
    //
    // This used to be empty, which meant screen space was only ever set as a
    // side effect of hud_render happening to call glOrtho. Any overlay drawn
    // before the HUD therefore inherited the camera's WORLD projection and was
    // positioned in world coordinates -- invisible once the player was far from
    // the origin. Making the transition explicit here means every UI caller has
    // a defined coordinate system regardless of draw order.
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, screen_width, screen_height, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    // World rendering toggles GL_TEXTURE_2D directly, bypassing the cache below,
    // which can leave the cache disagreeing with GL and make the next
    // set_texture_enabled call a no-op against the wrong state. Force both to a
    // known value so the UI phase starts consistent.
    glEnable(GL_TEXTURE_2D);
    g_texture_enabled = 1;
}

/**
 * Enter screen space, saving the caller's projection and model-view matrices.
 */
void renderer_begin_screen_space(void) {
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0, screen_width, screen_height, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
}

/**
 * Restore the matrices saved by renderer_begin_screen_space.
 */
void renderer_end_screen_space(void) {
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
}

/**
 * Draw an axis-aligned rectangle with a uniform RGBA color.
 */
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

/**
 * Draw an entire texture as an axis-aligned sprite.
 *
 * @param texture_id  OpenGL texture object to bind for the draw.
 */
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

/**
 * Draw a normalized texture region as an axis-aligned sprite.
 *
 * @param texture_id  OpenGL texture object to bind for the draw.
 * @param u0  Left normalized texture coordinate.
 * @param v0  Top normalized texture coordinate.
 * @param u1  Right normalized texture coordinate.
 * @param v1  Bottom normalized texture coordinate.
 */
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

/**
 * Render placeholder text as one colored rectangle per character.
 *
 * @param text  NUL-terminated text whose character count controls the rectangles.
 */
void renderer_draw_text_primitive(float x, float y, const char* text, float r, float g, float b) {
    // Very simple: draw a colored box per character
    for (int i = 0; text[i] != '\0'; i++) {
        renderer_draw_rect(x + i * 10, y, 8, 12, r, g, b, 1.0f);
    }
}

// Side of the square atlas a bake of this pixel height needs
static int atlas_side_for(float size) {
	return size > 45.0f ? 1024 : 512;
}

// Bake the sizes above the first; a level that fails falls back to a smaller one
static void bake_font_levels(const unsigned char* ttf_buffer) {
	for(int level = 1; level < FONT_LEVEL_COUNT; level++) {
		float size = font_level_size(level, level_base_size);
		int side = atlas_side_for(size);
		unsigned char* bitmap = malloc((size_t)side * (size_t)side);

		if(!bitmap)
			continue;

		if(stbtt_BakeFontBitmap(ttf_buffer, 0, size, bitmap, side, side, 32, 96,
		                        level_chars[level]) <= 0) {
			CLOG_WARN("Font size %.0f did not fit its atlas", size);
			free(bitmap);
			continue;
		}

		glGenTextures(1, &level_texture[level]);
		glBindTexture(GL_TEXTURE_2D, level_texture[level]);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, side, side, 0, GL_ALPHA, GL_UNSIGNED_BYTE,
		             bitmap);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		level_atlas[level] = side;
		free(bitmap);
	}
}

/**
 * Load and bake a TrueType font into the renderer's shared glyph texture.
 *
 * A current OpenGL context must exist before this call.
 *
 * @param path  Path to a readable TrueType font file.
 * @param size  Glyph height passed to the font baker, in pixels.
 */
void renderer_font_init(const char* path, float size) {
    unsigned char* ttf_buffer = malloc(1<<20);
    unsigned char* temp_bitmap = malloc(512*512);

    if (!ttf_buffer || !temp_bitmap) {
        CLOG_ERROR("Font allocation failed");
        free(ttf_buffer);
        free(temp_bitmap);
        return;
    }

    FILE* f = fopen(path, "rb");
    if (!f) {
        CLOG_ERROR("Font not found: %s", path);
        free(ttf_buffer);
        free(temp_bitmap);
        return;
    }
    /* stbtt_BakeFontBitmap parses this buffer as a font, so how much of it is
     * actually font matters: an unchecked read left the tail as uninitialised
     * heap and handed it to the parser. A file that read as nothing is not a
     * font at all, and the text is better missing than baked from rubbish. */
    size_t ttf_bytes = fread(ttf_buffer, 1, 1 << 20, f);
    fclose(f);

    if (ttf_bytes == 0) {
        CLOG_ERROR("Font %s could not be read", path);
        free(ttf_buffer);
        free(temp_bitmap);
        return;
    }
    memset(ttf_buffer + ttf_bytes, 0, (1 << 20) - ttf_bytes);

    stbtt_BakeFontBitmap(ttf_buffer, 0, size, temp_bitmap, 512, 512, 32, 96, baked_chars);

    glGenTextures(1, &font_texture);
    glBindTexture(GL_TEXTURE_2D, font_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, 512, 512, 0, GL_ALPHA, GL_UNSIGNED_BYTE, temp_bitmap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

	level_base_size = size;
	memcpy(level_chars[0], baked_chars, sizeof(baked_chars));
	level_texture[0] = font_texture;
	level_atlas[0] = 512;
	bake_font_levels(ttf_buffer);

    free(ttf_buffer);
    free(temp_bitmap);
}

/**
 * Draw printable ASCII text with the shared baked font.
 *
 * @param text  NUL-terminated text; bytes outside ASCII 32 through 127 are skipped.
 */
void renderer_draw_text(float x, float y, const char* text) {
	renderer_draw_text_colored(x, y, text, 1.0f, 1.0f, 1.0f);   // White text
}

/**
 * Draw printable ASCII text with the shared baked font, in a colour.
 *
 * @param text  NUL-terminated text; bytes outside ASCII 32 through 127 are skipped.
 */
void renderer_draw_text_colored(float x, float y, const char* text,
                                float r, float g, float b) {
	set_texture_enabled(1);
	glBindTexture(GL_TEXTURE_2D, font_texture);
	glColor4f(r, g, b, 1.0f);

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

/**
 * Draw text centered within a rectangular region.
 *
 * @param text  NUL-terminated text measured with the shared baked font.
 */
void renderer_draw_text_centered(float x, float y, float w, float h, const char* text) {
    float tw = 0;
    for (int i = 0; text[i]; i++) {
        if ((unsigned char)text[i] >= 32 && (unsigned char)text[i] < 128)
            tw += baked_chars[text[i]-32].xadvance;
    }
    // Adjust y by roughly half the font height (size/2) to center vertically
    renderer_draw_text(x + (w - tw) / 2.0f, y + (h / 2.0f) + 6.0f, text);
}

/**
 * Draw a filled circle as a triangle fan.
 *
 * @param segments  Positive number of perimeter subdivisions.
 */
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

/**
 * Raise world-space drawing by z world units until renderer_lift_end().
 */
void renderer_lift_begin(float z) {
    glPushMatrix();
    glTranslatef(0.0f, 0.0f, z);
}

void renderer_lift_end(void) {
    glPopMatrix();
}

/**
 * Draw a filled cone centered on a direction vector.
 *
 * @param angle_deg  Full cone angle in degrees.
 * @param segments  Positive number of arc subdivisions.
 */
void renderer_draw_ring(float cx, float cy, float inner_radius, float outer_radius,
                        float r, float g, float b, float a, int segments) {
    set_texture_enabled(0);
    glColor4f(r, g, b, a);
    glBegin(GL_TRIANGLE_STRIP);
    for (int i = 0; i <= segments; i++) {
        float angle = 2.0f * 3.14159265f * (float)i / (float)segments;
        float c = cosf(angle), s = sinf(angle);
        glVertex2f(cx + c * outer_radius, cy + s * outer_radius);
        glVertex2f(cx + c * inner_radius, cy + s * inner_radius);
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

void renderer_draw_texture_quad(unsigned int texture_id, const float corners[8],
                                float r, float g, float b, float a) {
	set_texture_enabled(1);
	glBindTexture(GL_TEXTURE_2D, texture_id);
	glColor4f(r, g, b, a);
	glBegin(GL_QUADS);
	glTexCoord2f(0.0f, 0.0f); glVertex2f(corners[0], corners[1]);
	glTexCoord2f(1.0f, 0.0f); glVertex2f(corners[2], corners[3]);
	glTexCoord2f(1.0f, 1.0f); glVertex2f(corners[4], corners[5]);
	glTexCoord2f(0.0f, 1.0f); glVertex2f(corners[6], corners[7]);
	glEnd();
}

void renderer_draw_triangle(float x0, float y0, float x1, float y1, float x2, float y2,
                            float r, float g, float b, float a) {
	set_texture_enabled(0);
	glColor4f(r, g, b, a);
	glBegin(GL_TRIANGLES);
	glVertex2f(x0, y0);
	glVertex2f(x1, y1);
	glVertex2f(x2, y2);
	glEnd();
}

/**
 * Release the shared font texture.
 *
 * A current OpenGL context must exist before this call.
 */
void renderer_cleanup(void) {
	for(int level = 1; level < FONT_LEVEL_COUNT; level++) {
		if(level_texture[level]) {
			glDeleteTextures(1, &level_texture[level]);
			level_texture[level] = 0;
		}
	}
    if (font_texture) {
        glDeleteTextures(1, &font_texture);
        font_texture = 0;
    }
    CLOG_INFO("Renderer cleaned up");
}

// The level asked for if it baked, else the nearest smaller one that did
static int usable_level(int level) {
	if(level >= FONT_LEVEL_COUNT)
		level = FONT_LEVEL_COUNT - 1;
	while(level > 0 && !level_texture[level])
		level--;

	return level < 0 ? 0 : level;
}

int renderer_font_level_for(float pixel_height) {
	return usable_level(font_level_for(pixel_height, level_base_size));
}

float renderer_font_level_size(int level) {
	return font_level_size(usable_level(level), level_base_size);
}

float renderer_text_width_level(const char* text, int level) {
	float width = 0.0f;

	level = usable_level(level);
	for(int i = 0; text && text[i]; i++) {
		unsigned char c = (unsigned char)text[i];

		if(c >= 32 && c < 128)
			width += level_chars[level][c - 32].xadvance;
	}

	return width;
}

void renderer_draw_text_level(float x, float y, const char* text, int level, float r, float g,
                              float b) {
	int side;

	level = usable_level(level);
	side = level_atlas[level];
	set_texture_enabled(1);
	glBindTexture(GL_TEXTURE_2D, level_texture[level]);
	glColor4f(r, g, b, 1.0f);

	glBegin(GL_QUADS);
	while(*text) {
		if((unsigned char)*text >= 32 && (unsigned char)*text < 128) {
			stbtt_aligned_quad q;

			stbtt_GetBakedQuad(level_chars[level], side, side, *text - 32, &x, &y, &q, 1);
			glTexCoord2f(q.s0, q.t0); glVertex2f(q.x0, q.y0);
			glTexCoord2f(q.s1, q.t0); glVertex2f(q.x1, q.y0);
			glTexCoord2f(q.s1, q.t1); glVertex2f(q.x1, q.y1);
			glTexCoord2f(q.s0, q.t1); glVertex2f(q.x0, q.y1);
		}
		text++;
	}
	glEnd();
}

/**
 * Measure text in the shared baked font.
 *
 * @return      Advance width in pixels.
 */
float renderer_text_width(const char* text) {
    float width = 0.0f;
    for (int i = 0; text && text[i]; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c >= 32 && c < 128) width += baked_chars[c - 32].xadvance;
    }
    return width;
}

/**
 * Lay text out into lines that fit a width, optionally drawing them.
 *
 * Shared by the drawing and measuring entry points so a panel sized from the
 * measurement can never disagree with what is drawn into it.
 *
 * @param draw  Nonzero to draw; zero to measure only.
 * @return      The number of lines produced.
 */
static int layout_text_wrapped(float x, float y, float max_width, float line_height,
                               const char* text, int draw) {
    if (!text || !*text) return 0;

    char line[512];
    int  line_len = 0;
    int  lines    = 0;

    const char* word = text;

    for (;;) {
        /* Find the end of the next word and the separator that follows it. */
        const char* cursor = word;
        while (*cursor && *cursor != ' ' && *cursor != '\n' && *cursor != '\t') cursor++;

        int word_len = (int)(cursor - word);
        char candidate[512];
        int  candidate_len = 0;

        if (line_len > 0) {
            memcpy(candidate, line, (size_t)line_len);
            candidate_len = line_len;
            candidate[candidate_len++] = ' ';
        }
        if (word_len > 0) {
            if (candidate_len + word_len >= (int)sizeof(candidate))
                word_len = (int)sizeof(candidate) - candidate_len - 1;
            memcpy(candidate + candidate_len, word, (size_t)word_len);
            candidate_len += word_len;
        }
        candidate[candidate_len] = '\0';

        int overflows = (line_len > 0 && renderer_text_width(candidate) > max_width);

        if (overflows) {
            /* Emit the line as it stood and start the next one with this word. */
            line[line_len] = '\0';
            if (draw) renderer_draw_text(x, y + lines * line_height, line);
            lines++;

            line_len = word_len;
            if (line_len >= (int)sizeof(line)) line_len = (int)sizeof(line) - 1;
            memcpy(line, word, (size_t)line_len);
        } else {
            memcpy(line, candidate, (size_t)candidate_len);
            line_len = candidate_len;
        }

        if (*cursor == '\n') {
            line[line_len] = '\0';
            if (draw) renderer_draw_text(x, y + lines * line_height, line);
            lines++;
            line_len = 0;
        }

        if (!*cursor) break;
        word = cursor + 1;
    }

    if (line_len > 0) {
        line[line_len] = '\0';
        if (draw) renderer_draw_text(x, y + lines * line_height, line);
        lines++;
    }

    return lines;
}

/**
 * Draw text broken to fit a width, honouring explicit newlines.
 *
 * @return      The number of lines drawn.
 */
int renderer_draw_text_wrapped(float x, float y, float max_width, float line_height,
                               const char* text) {
    return layout_text_wrapped(x, y, max_width, line_height, text, 1);
}

/**
 * Count the lines renderer_draw_text_wrapped() would draw.
 */
int renderer_measure_text_wrapped(float max_width, const char* text) {
    return layout_text_wrapped(0.0f, 0.0f, max_width, 0.0f, text, 0);
}
