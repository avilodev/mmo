/**
 * @file
 * Pad tileset atlases with edge-repeating gutters and address tiles inside them.
 */

#include "texture/texture_atlas.h"

#include <stdlib.h>
#include <string.h>

static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

unsigned char* texture_atlas_pad(const unsigned char* src, int w, int h, int channels,
                                 int cols, int rows, int pad, int* out_w, int* out_h) {
    if (!src || w <= 0 || h <= 0 || channels <= 0 || cols <= 0 || rows <= 0 || pad < 0)
        return NULL;
    if (w % cols != 0 || h % rows != 0) return NULL;

    int tile_w = w / cols;
    int tile_h = h / rows;
    int cell_w = tile_w + 2 * pad;
    int cell_h = tile_h + 2 * pad;
    int dst_w  = cols * cell_w;
    int dst_h  = rows * cell_h;

    unsigned char* dst = malloc((size_t)dst_w * (size_t)dst_h * (size_t)channels);
    if (!dst) return NULL;

    for (int row = 0; row < rows; row++) {
        for (int col = 0; col < cols; col++) {
            for (int cy = 0; cy < cell_h; cy++) {
                /* A gutter pixel is the nearest pixel of this tile. */
                int sy = row * tile_h + clampi(cy - pad, 0, tile_h - 1);
                for (int cx = 0; cx < cell_w; cx++) {
                    int sx = col * tile_w + clampi(cx - pad, 0, tile_w - 1);
                    const unsigned char* from = src + ((size_t)sy * (size_t)w + (size_t)sx) * (size_t)channels;
                    unsigned char* to = dst + ((size_t)(row * cell_h + cy) * (size_t)dst_w +
                                               (size_t)(col * cell_w + cx)) * (size_t)channels;
                    memcpy(to, from, (size_t)channels);
                }
            }
        }
    }

    *out_w = dst_w;
    *out_h = dst_h;
    return dst;
}

void texture_atlas_uv(int cols, int rows, int tile_w, int tile_h, int pad, int index,
                      float* u0, float* v0, float* u1, float* v1) {
    int cell_w = tile_w + 2 * pad;
    int cell_h = tile_h + 2 * pad;
    float atlas_w = (float)(cols * cell_w);
    float atlas_h = (float)(rows * cell_h);

    int col = index % cols;
    int row = index / cols;

    *u0 = (float)(col * cell_w + pad) / atlas_w;
    *v0 = (float)(row * cell_h + pad) / atlas_h;
    *u1 = (float)(col * cell_w + pad + tile_w) / atlas_w;
    *v1 = (float)(row * cell_h + pad + tile_h) / atlas_h;
}

int texture_atlas_mip_levels(int pad) {
    int levels = 0;
    while (pad >= 2) {
        pad >>= 1;
        levels++;
    }
    return levels;
}

// Whether the pixel at (x, y) is inside the image and already has a colour
static int has_colour(const unsigned char* has, int w, int h, int x, int y) {
	return x >= 0 && y >= 0 && x < w && y < h && has[y * w + x];
}

void texture_bleed_color(unsigned char* rgba, int w, int h, int passes) {
	unsigned char* has;
	unsigned char* next;

	if(!rgba || w <= 0 || h <= 0 || passes <= 0)
		return;

	has = malloc((size_t)w * h);
	next = malloc((size_t)w * h);
	if(!has || !next) {
		free(has);
		free(next);
		return;
	}

	// Anything not fully transparent already has its own colour
	for(int i = 0; i < w * h; i++)
		has[i] = rgba[i * 4 + 3] != 0;

	for(int pass = 0; pass < passes; pass++) {
		memcpy(next, has, (size_t)w * h);

		for(int y = 0; y < h; y++) {
			for(int x = 0; x < w; x++) {
				int sum[3] = { 0, 0, 0 };
				int count = 0;

				if(has[y * w + x])
					continue;

				for(int dy = -1; dy <= 1; dy++) {
					for(int dx = -1; dx <= 1; dx++) {
						const unsigned char* p;

						if(!has_colour(has, w, h, x + dx, y + dy))
							continue;

						p = rgba + ((y + dy) * w + (x + dx)) * 4;
						sum[0] += p[0];
						sum[1] += p[1];
						sum[2] += p[2];
						count++;
					}
				}

				if(count == 0)
					continue;

				rgba[(y * w + x) * 4 + 0] = (unsigned char)(sum[0] / count);
				rgba[(y * w + x) * 4 + 1] = (unsigned char)(sum[1] / count);
				rgba[(y * w + x) * 4 + 2] = (unsigned char)(sum[2] / count);
				next[y * w + x] = 1;
			}
		}

		memcpy(has, next, (size_t)w * h);
	}

	free(has);
	free(next);
}
