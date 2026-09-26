/**
 * @file
 * Check the tileset gutter padding that keeps mipmaps from bleeding tiles.
 *
 * Viewed at an angle, ground tiles are minified, so they need mipmaps; and a
 * mipmapped atlas averages each tile's edge with its neighbour's, which shows
 * as seams. The fix is a gutter around every tile filled with that tile's own
 * edge pixels. What matters, and is checked here, is that each gutter pixel
 * comes from its own tile, never the neighbour, and that the UVs still point
 * at exactly the tile.
 */

#include "texture/texture_atlas.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond, what)                                                   \
    do {                                                                    \
        if (cond) {                                                         \
            printf("  ok   %s\n", (what));                                  \
        } else {                                                            \
            printf("  FAIL %s  (%s:%d)\n", (what), __FILE__, __LINE__);     \
            g_failures++;                                                   \
        }                                                                   \
    } while (0)

/* A 2x2 atlas of 2x2-pixel tiles, one channel, each tile a distinct value.
 * Tile (col,row) pixel (x,y) = 10 * (tile index + 1) + (y * 2 + x). */
static void make_atlas(unsigned char px[16]) {
    for (int ty = 0; ty < 2; ty++)
        for (int tx = 0; tx < 2; tx++)
            for (int y = 0; y < 2; y++)
                for (int x = 0; x < 2; x++)
                    px[(ty * 2 + y) * 4 + (tx * 2 + x)] =
                        (unsigned char)(10 * (ty * 2 + tx + 1) + y * 2 + x);
}

static unsigned char at(const unsigned char* img, int w, int x, int y) {
    return img[y * w + x];
}

int main(void) {
    printf("=== tileset atlas padding ===\n");

    unsigned char src[16];
    make_atlas(src);

    printf("\nTEST 1: the padded atlas has room for a gutter round every tile\n");
    int w = 0, h = 0;
    unsigned char* out = texture_atlas_pad(src, 4, 4, 1, 2, 2, 2, &w, &h);
    CHECK(out != NULL, "padding succeeds");
    CHECK(w == 12 && h == 12, "each 2px tile becomes a 6px cell");
    if (!out) return 1;

    printf("\nTEST 2: every tile is copied intact into its cell\n");
    {
        int intact = 1;
        for (int ty = 0; ty < 2; ty++)
            for (int tx = 0; tx < 2; tx++)
                for (int y = 0; y < 2; y++)
                    for (int x = 0; x < 2; x++) {
                        unsigned char want = src[(ty * 2 + y) * 4 + (tx * 2 + x)];
                        if (at(out, w, tx * 6 + 2 + x, ty * 6 + 2 + y) != want) intact = 0;
                    }
        CHECK(intact, "tile pixels land at cell origin + gutter");
    }

    printf("\nTEST 3: gutters repeat the tile's own edge, never a neighbour's\n");
    {
        /* Tile 0 (values 10..13) occupies cell (0,0), x/y 0..5. */
        CHECK(at(out, w, 0, 0) == 10 && at(out, w, 1, 1) == 10, "top-left corner gutter is tile 0's corner");
        CHECK(at(out, w, 5, 2) == 11 && at(out, w, 4, 3) == 13, "right gutter repeats tile 0's right column");
        CHECK(at(out, w, 2, 5) == 12 && at(out, w, 3, 4) == 13, "bottom gutter repeats tile 0's bottom row");
        /* Tile 1 (values 20..23) starts at x = 6: its left gutter is its own. */
        CHECK(at(out, w, 6, 2) == 20 && at(out, w, 7, 3) == 22, "tile 1's left gutter is tile 1, not tile 0");
    }

    printf("\nTEST 4: UVs frame exactly the tile inside its gutter\n");
    {
        float u0, v0, u1, v1;
        texture_atlas_uv(2, 2, 2, 2, 2, 3, &u0, &v0, &u1, &v1);   /* tile 3 = (1,1) */
        CHECK(u0 * 12.0f > 7.99f && u0 * 12.0f < 8.01f, "u0 is the tile's left edge");
        CHECK(u1 * 12.0f > 9.99f && u1 * 12.0f < 10.01f, "u1 is the tile's right edge");
        CHECK(v0 * 12.0f > 7.99f && v1 * 12.0f < 10.01f, "v spans the tile's rows");
    }

    printf("\nTEST 5: an atlas that does not divide into whole tiles is refused\n");
    {
        int bw = 0, bh = 0;
        unsigned char* bad = texture_atlas_pad(src, 4, 4, 1, 3, 2, 2, &bw, &bh);
        CHECK(bad == NULL, "4px wide cannot be three columns");
        free(bad);
        CHECK(texture_atlas_mip_levels(2) == 1 && texture_atlas_mip_levels(4) == 2 &&
              texture_atlas_mip_levels(0) == 0,
              "mip levels stop while the gutter is still a whole pixel");
    }

    free(out);

    printf("\n");
    if (g_failures) {
        printf("%d ASSERTION%s FAILED\n", g_failures, g_failures == 1 ? "" : "S");
        return 1;
    }
    printf("ALL ASSERTIONS PASSED\n");
    return 0;
}
