#ifndef MAP_DRAW_H
#define MAP_DRAW_H

// Small drawing helpers the map screen shares: scaled text, shapes, bars.

#include "ui/map_layout.h"

/** A colour with opacity, components 0 to 1. */
typedef struct {
	float r, g, b, a;
} map_color;

#define MAP_TEXT_SMALL 0.62f
#define MAP_TEXT_BODY  0.74f
#define MAP_TEXT_HEAD  0.90f

/** Draw text at a fraction of the font's size; y is the baseline. */
void map_text(float x, float y, float scale, const char* text, map_color color);

/** As map_text, over a one-pixel dark shadow so it reads on any terrain. */
void map_text_shadowed(float x, float y, float scale, const char* text, map_color color);

float map_text_width(float scale, const char* text);

/** Copy text into out, ending it with ".." when it does not fit in max_width. */
void map_text_fit(char* out, int out_size, float scale, float max_width, const char* text);

void map_fill(const map_rect* r, map_color color);
void map_outline(const map_rect* r, float thickness, map_color color);

/** A bar that is `fraction` full, over a dark track. */
void map_bar(const map_rect* r, float fraction, map_color fill);

/** A diamond, drawn dark behind a coloured one so it reads on any ground. */
void map_diamond(float cx, float cy, float radius, map_color color);

/** A dart pointing along (dir_x, dir_y), its centre at (cx, cy). */
void map_dart(float cx, float cy, float dir_x, float dir_y, float size, map_color color);

/** A filled disc with a dark rim. */
void map_disc(float cx, float cy, float radius, map_color color);

/** A line of dots from one point to another, for lines to a target. */
void map_dotted_line(float x0, float y0, float x1, float y1, float spacing, map_color color);

/** A box with a bright edge and the lines of text inside it. */
void map_text_box(float x, float y, const char* const* lines, const map_color* colors,
                  int count, const map_rect* keep_inside);

/** Cut drawing to a rectangle until map_clip_end. */
void map_clip_begin(const map_rect* r, float viewport_w);
void map_clip_end(void);

#endif /* MAP_DRAW_H */
