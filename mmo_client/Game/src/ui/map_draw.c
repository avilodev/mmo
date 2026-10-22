/**
 * @file
 * The map screen's shared drawing helpers.
 */
#include "ui/map_draw.h"
#include "render/renderer.h"

#include <GLFW/glfw3.h>
#include <math.h>
#include <string.h>

#define BOX_PAD       7.0f
#define BOX_LINE      15.0f
#define DOT_SIZE      2.0f

static const map_color DARK = { 0.0f, 0.0f, 0.0f, 0.78f };

float map_text_width(float scale, const char* text) {
	return renderer_text_width(text) * scale;
}

void map_text(float x, float y, float scale, const char* text, map_color color) {
	glMatrixMode(GL_MODELVIEW);
	glPushMatrix();
	glTranslatef(x, y, 0.0f);
	glScalef(scale, scale, 1.0f);
	renderer_draw_text_colored(0.0f, 0.0f, text, color.r, color.g, color.b);
	glPopMatrix();
}

void map_text_shadowed(float x, float y, float scale, const char* text, map_color color) {
	map_text(x + 1.0f, y + 1.0f, scale, text, DARK);
	map_text(x, y, scale, text, color);
}

void map_text_fit(char* out, int out_size, float scale, float max_width, const char* text) {
	int length = (int)strlen(text);

	if(length >= out_size)
		length = out_size - 1;
	memcpy(out, text, (size_t)length);
	out[length] = '\0';
	if(map_text_width(scale, out) <= max_width)
		return;
	while(length > 2) {
		length--;
		out[length] = '\0';
		out[length - 1] = '.';
		out[length - 2] = '.';
		if(map_text_width(scale, out) <= max_width)
			return;
	}
}

void map_fill(const map_rect* r, map_color color) {
	renderer_draw_rect(r->x, r->y, r->w, r->h, color.r, color.g, color.b, color.a);
}

void map_outline(const map_rect* r, float thickness, map_color color) {
	renderer_draw_rect(r->x, r->y, r->w, thickness, color.r, color.g, color.b, color.a);
	renderer_draw_rect(r->x, r->y + r->h - thickness, r->w, thickness,
	                   color.r, color.g, color.b, color.a);
	renderer_draw_rect(r->x, r->y, thickness, r->h, color.r, color.g, color.b, color.a);
	renderer_draw_rect(r->x + r->w - thickness, r->y, thickness, r->h,
	                   color.r, color.g, color.b, color.a);
}

void map_bar(const map_rect* r, float fraction, map_color fill) {
	if(fraction < 0.0f)
		fraction = 0.0f;
	if(fraction > 1.0f)
		fraction = 1.0f;
	renderer_draw_rect(r->x - 1.0f, r->y - 1.0f, r->w + 2.0f, r->h + 2.0f,
	                   0.0f, 0.0f, 0.0f, 0.85f);
	renderer_draw_rect(r->x, r->y, r->w, r->h, 0.16f, 0.16f, 0.2f, 1.0f);
	renderer_draw_rect(r->x, r->y, r->w * fraction, r->h, fill.r, fill.g, fill.b, fill.a);
}

void map_diamond(float cx, float cy, float radius, map_color color) {
	renderer_draw_circle(cx, cy, radius + 1.6f, 0.0f, 0.0f, 0.0f, 0.85f, 4);
	renderer_draw_circle(cx, cy, radius, color.r, color.g, color.b, color.a, 4);
}

void map_dart(float cx, float cy, float dir_x, float dir_y, float size, map_color color) {
	float tip_x = cx + dir_x * size * 0.55f;
	float tip_y = cy + dir_y * size * 0.55f;

	renderer_draw_cone(tip_x, tip_y, -dir_x, -dir_y, size * 1.7f, 80.0f,
	                   0.0f, 0.0f, 0.0f, 0.9f, 2);
	renderer_draw_cone(tip_x, tip_y, -dir_x, -dir_y, size * 1.35f, 70.0f,
	                   color.r, color.g, color.b, color.a, 2);
}

void map_disc(float cx, float cy, float radius, map_color color) {
	renderer_draw_circle(cx, cy, radius + 1.5f, 0.0f, 0.0f, 0.0f, 0.85f, 20);
	renderer_draw_circle(cx, cy, radius, color.r, color.g, color.b, color.a, 20);
}

void map_dotted_line(float x0, float y0, float x1, float y1, float spacing, map_color color) {
	float dx = x1 - x0;
	float dy = y1 - y0;
	float length = sqrtf(dx * dx + dy * dy);
	int dots = (int)(length / spacing);

	if(dots > 400)
		dots = 400;
	for(int i = 0; i <= dots; i++) {
		float t = dots > 0 ? (float)i / (float)dots : 0.0f;

		renderer_draw_rect(x0 + dx * t - DOT_SIZE * 0.5f, y0 + dy * t - DOT_SIZE * 0.5f,
		                   DOT_SIZE, DOT_SIZE, color.r, color.g, color.b, color.a);
	}
}

// The widest line in a box of text, at body size.
static float widest_line(const char* const* lines, int count) {
	float widest = 0.0f;

	for(int i = 0; i < count; i++) {
		float w = map_text_width(MAP_TEXT_BODY, lines[i]);

		if(w > widest)
			widest = w;
	}

	return widest;
}

void map_text_box(float x, float y, const char* const* lines, const map_color* colors,
                  int count, const map_rect* keep_inside) {
	map_rect box = { x, y, widest_line(lines, count) + BOX_PAD * 2.0f,
	                 (float)count * BOX_LINE + BOX_PAD * 2.0f - 3.0f };

	if(keep_inside) {
		if(box.x + box.w > keep_inside->x + keep_inside->w)
			box.x = keep_inside->x + keep_inside->w - box.w;
		if(box.y + box.h > keep_inside->y + keep_inside->h)
			box.y = keep_inside->y + keep_inside->h - box.h;
	}
	map_fill(&box, (map_color){ 0.04f, 0.05f, 0.09f, 0.94f });
	map_outline(&box, 1.5f, (map_color){ 0.45f, 0.58f, 0.78f, 1.0f });
	for(int i = 0; i < count; i++)
		map_text(box.x + BOX_PAD, box.y + BOX_PAD + 8.0f + (float)i * BOX_LINE,
		         MAP_TEXT_BODY, lines[i], colors[i]);
}

void map_clip_begin(const map_rect* r, float viewport_w) {
	GLint viewport[4];

	glGetIntegerv(GL_VIEWPORT, viewport);

	float scale = (float)viewport[2] / viewport_w;

	glEnable(GL_SCISSOR_TEST);
	glScissor(viewport[0] + (GLint)(r->x * scale),
	          viewport[1] + viewport[3] - (GLint)((r->y + r->h) * scale),
	          (GLsizei)(r->w * scale), (GLsizei)(r->h * scale));
}

void map_clip_end(void) {
	glDisable(GL_SCISSOR_TEST);
}
