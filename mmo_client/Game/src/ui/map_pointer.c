/**
 * @file
 * Draw the player's pointer on a map, in a few styles.
 */

#include "ui/map_pointer.h"
#include "render/gl_loader.h"
#include "render/renderer.h"

#define OUTLINE 0.9f

typedef struct {
	float x;
	float y;
} point;

// A point `ahead` along the facing and `side` across it, from a centre
static point at(float cx, float cy, float dir_x, float dir_y, float ahead, float side) {
	point p = { cx + dir_x * ahead - dir_y * side, cy + dir_y * ahead + dir_x * side };

	return p;
}

static void triangle(point a, point b, point c, map_color color) {
	renderer_draw_triangle(a.x, a.y, b.x, b.y, c.x, c.y, color.r, color.g, color.b, color.a);
}

static void draw_dart(float cx, float cy, float dir_x, float dir_y, float size, map_color color) {
	map_dart(cx, cy, dir_x, dir_y, size, color);
}

// The arrowhead of a navigation app: a tip and a notch in its back. These are
// its corners in units of the pointer's size, ahead and to the side.
#define ARROW_TIP_AHEAD 1.7f
#define ARROW_BACK_AHEAD -1.2f
#define ARROW_BACK_SIDE 1.15f
#define ARROW_NOTCH_AHEAD -0.45f

// How much bigger the dark outline is than the arrow
#define ARROW_OUTLINE_GROWTH 1.35f

// The arrow is drawn from a smooth texture rather than from triangles, so its
// edges are anti-aliased and it keeps its shape at any size on screen. The
// texture covers this many units each way from the centre.
#define ARROW_REACH 2.4f
#define ARROW_TEXELS 256
#define ARROW_SAMPLES 4

static unsigned int s_arrow_texture;

// Whether (px, py) lies in the triangle
static int in_triangle(float px, float py, point a, point b, point c) {
	float d1 = (px - b.x) * (a.y - b.y) - (a.x - b.x) * (py - b.y);
	float d2 = (px - c.x) * (b.y - c.y) - (b.x - c.x) * (py - c.y);
	float d3 = (px - a.x) * (c.y - a.y) - (c.x - a.x) * (py - a.y);
	int negative = d1 < 0.0f || d2 < 0.0f || d3 < 0.0f;
	int positive = d1 > 0.0f || d2 > 0.0f || d3 > 0.0f;

	return !(negative && positive);
}

// Whether a point, in the arrow's own units (x ahead, y to the side), is inside
// the arrow grown by `scale`
static int in_arrow(float x, float y, float scale) {
	point tip = { ARROW_TIP_AHEAD * scale, 0.0f };
	point left = { ARROW_BACK_AHEAD * scale, -ARROW_BACK_SIDE * scale };
	point notch = { ARROW_NOTCH_AHEAD * scale, 0.0f };
	point right = { ARROW_BACK_AHEAD * scale, ARROW_BACK_SIDE * scale };

	return in_triangle(x, y, tip, left, notch) || in_triangle(x, y, tip, notch, right);
}

// Paint the arrow in white over a dark outline; the colour is tinted in at draw time
static void paint_arrow(unsigned char* pixels) {
	for(int j = 0; j < ARROW_TEXELS; j++) {
		for(int i = 0; i < ARROW_TEXELS; i++) {
			int fill = 0;
			int outline = 0;
			unsigned char* out = &pixels[((size_t)j * ARROW_TEXELS + (size_t)i) * 4];
			float covered_fill;
			float covered_outline;

			for(int sy = 0; sy < ARROW_SAMPLES; sy++) {
				for(int sx = 0; sx < ARROW_SAMPLES; sx++) {
					float u = ((float)i + ((float)sx + 0.5f) / ARROW_SAMPLES) / ARROW_TEXELS;
					float v = ((float)j + ((float)sy + 0.5f) / ARROW_SAMPLES) / ARROW_TEXELS;
					float x = (u * 2.0f - 1.0f) * ARROW_REACH;
					float y = (v * 2.0f - 1.0f) * ARROW_REACH;

					fill += in_arrow(x, y, 1.0f);
					outline += in_arrow(x, y, ARROW_OUTLINE_GROWTH);
				}
			}

			covered_fill = (float)fill / (ARROW_SAMPLES * ARROW_SAMPLES);
			covered_outline = (float)outline / (ARROW_SAMPLES * ARROW_SAMPLES);
			if(covered_outline < covered_fill)
				covered_outline = covered_fill;

			// White where the arrow is, black where only the outline is
			out[0] = out[1] = out[2] = covered_outline > 0.0f
				? (unsigned char)(covered_fill / covered_outline * 255.0f + 0.5f) : 0;
			out[3] = (unsigned char)((covered_outline * OUTLINE + covered_fill * (1.0f - OUTLINE)) *
			                         255.0f + 0.5f);
		}
	}
}

static unsigned int arrow_texture(void) {
	static unsigned char pixels[ARROW_TEXELS * ARROW_TEXELS * 4];
	GLuint texture = 0;

	if(s_arrow_texture)
		return s_arrow_texture;

	paint_arrow(pixels);
	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, ARROW_TEXELS, ARROW_TEXELS, 0, GL_RGBA,
	             GL_UNSIGNED_BYTE, pixels);
	glGenerateMipmap(GL_TEXTURE_2D);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);
	s_arrow_texture = texture;

	return s_arrow_texture;
}

static void draw_arrow(float cx, float cy, float dx, float dy, float size, map_color color) {
	float reach = ARROW_REACH * size;
	point a = at(cx, cy, dx, dy, -reach, -reach);
	point b = at(cx, cy, dx, dy, reach, -reach);
	point c = at(cx, cy, dx, dy, reach, reach);
	point d = at(cx, cy, dx, dy, -reach, reach);
	float corners[8] = { a.x, a.y, b.x, b.y, c.x, c.y, d.x, d.y };

	renderer_draw_texture_quad(arrow_texture(), corners, color.r, color.g, color.b, color.a);
}

static void draw_beam(float cx, float cy, float dx, float dy, float size, map_color color) {
	renderer_draw_cone(cx, cy, dx, dy, size * 4.2f, 75.0f, color.r, color.g, color.b, 0.32f, 12);
	renderer_draw_circle(cx, cy, size * 1.05f + 1.6f, 0.0f, 0.0f, 0.0f, OUTLINE, 20);
	renderer_draw_circle(cx, cy, size * 1.05f, 1.0f, 1.0f, 1.0f, 1.0f, 20);
	renderer_draw_circle(cx, cy, size * 0.75f, color.r, color.g, color.b, color.a, 20);
}

static void draw_ring(float cx, float cy, float dx, float dy, float size, map_color color) {
	point tip = at(cx, cy, dx, dy, size * 2.5f, 0.0f);
	point base_a = at(cx, cy, dx, dy, size * 1.45f, -size * 0.8f);
	point base_b = at(cx, cy, dx, dy, size * 1.45f, size * 0.8f);
	map_color dark = { 0.0f, 0.0f, 0.0f, OUTLINE };

	renderer_draw_ring(cx, cy, size * 0.95f, size * 1.6f, 0.0f, 0.0f, 0.0f, OUTLINE, 28);
	renderer_draw_ring(cx, cy, size * 1.1f, size * 1.45f, color.r, color.g, color.b, color.a,
	                   28);
	renderer_draw_circle(cx, cy, size * 0.62f + 1.4f, 0.0f, 0.0f, 0.0f, OUTLINE, 18);
	renderer_draw_circle(cx, cy, size * 0.62f, color.r, color.g, color.b, color.a, 18);

	triangle(at(cx, cy, dx, dy, size * 2.5f + 1.8f, 0.0f), at(cx, cy, dx, dy, size * 1.25f,
	         -size * 0.95f), at(cx, cy, dx, dy, size * 1.25f, size * 0.95f), dark);
	triangle(tip, base_a, base_b, color);
}

static void draw_needle(float cx, float cy, float dx, float dy, float size, map_color color) {
	map_color dark = { 0.0f, 0.0f, 0.0f, OUTLINE };
	map_color tail = { 0.95f, 0.96f, 1.0f, 1.0f };
	float wide = size * 0.62f;
	float grown = size * 0.62f + 1.5f;
	point tip = at(cx, cy, dx, dy, size * 2.1f, 0.0f);
	point left = at(cx, cy, dx, dy, 0.0f, -wide);
	point right = at(cx, cy, dx, dy, 0.0f, wide);
	point end = at(cx, cy, dx, dy, -size * 1.6f, 0.0f);

	// Dark outline: the same kite a little larger
	triangle(at(cx, cy, dx, dy, size * 2.1f + 2.0f, 0.0f), at(cx, cy, dx, dy, 0.0f, -grown),
	         at(cx, cy, dx, dy, 0.0f, grown), dark);
	triangle(at(cx, cy, dx, dy, -size * 1.6f - 2.0f, 0.0f), at(cx, cy, dx, dy, 0.0f, -grown),
	         at(cx, cy, dx, dy, 0.0f, grown), dark);

	triangle(tip, left, right, color);
	triangle(end, left, right, tail);
}

void map_pointer_draw(map_pointer_style style, float cx, float cy, float dir_x, float dir_y,
                      float size, map_color color) {
	switch(style) {
	case MAP_POINTER_ARROW:
		draw_arrow(cx, cy, dir_x, dir_y, size, color);
		break;
	case MAP_POINTER_BEAM:
		draw_beam(cx, cy, dir_x, dir_y, size, color);
		break;
	case MAP_POINTER_RING:
		draw_ring(cx, cy, dir_x, dir_y, size, color);
		break;
	case MAP_POINTER_NEEDLE:
		draw_needle(cx, cy, dir_x, dir_y, size, color);
		break;
	default:
		draw_dart(cx, cy, dir_x, dir_y, size, color);
		break;
	}
}
