/**
 * @file
 * Check the vertex data a chunk's tile layer is turned into for the GPU.
 *
 * The mesh replaces a display list that drew each tile as a quad at
 * (tile * tile_size). What has to survive the move: every non-empty tile is
 * exactly one quad in the same place, empty and out-of-world tiles draw
 * nothing, flat-colour worlds get the palette colour, a live tile
 * modification wins over the file, and tiles from different tilesets are
 * grouped so each texture is bound once per chunk.
 */

#include "world/chunk_mesh.h"

#include <math.h>
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

#define TILE 16

static ChunkVertex g_verts[CHUNK_MESH_MAX_VERTS];

static ChunkMeshInput flat_input(const uint16_t* tiles) {
    ChunkMeshInput in;
    memset(&in, 0, sizeof(in));
    in.tiles = tiles;
    in.chunk_x = 1;
    in.chunk_y = 2;
    in.world_w = 1000;
    in.world_h = 1000;
    in.tile_size = TILE;
    in.flat_color = 1;
    return in;
}

/** Bounds of the quad at vertex `first`, which is two triangles. */
static void quad_bounds(const ChunkVertex* v, float* x0, float* y0, float* x1, float* y1) {
    *x0 = *y0 = 1e30f;
    *x1 = *y1 = -1e30f;
    for (int i = 0; i < 6; i++) {
        if (v[i].x < *x0) *x0 = v[i].x;
        if (v[i].x > *x1) *x1 = v[i].x;
        if (v[i].y < *y0) *y0 = v[i].y;
        if (v[i].y > *y1) *y1 = v[i].y;
    }
}

int main(void) {
    printf("=== chunk mesh ===\n");

    static uint16_t tiles[CHUNK_SIZE * CHUNK_SIZE];
    ChunkMesh mesh;
    mesh.verts = g_verts;

    printf("\nTEST 1: an empty layer draws nothing\n");
    {
        memset(tiles, 0, sizeof(tiles));
        ChunkMeshInput in = flat_input(tiles);
        chunk_mesh_build(&mesh, &in);
        CHECK(mesh.vert_count == 0 && mesh.range_count == 0, "no vertices, no draw ranges");
    }

    printf("\nTEST 2: a tile is one quad, where the display list drew it\n");
    {
        memset(tiles, 0, sizeof(tiles));
        tiles[3 * CHUNK_SIZE + 5] = PAL_ROAD;           /* local (5, 3) */
        ChunkMeshInput in = flat_input(tiles);
        chunk_mesh_build(&mesh, &in);
        CHECK(mesh.vert_count == 6, "six vertices: two triangles");
        float x0, y0, x1, y1;
        quad_bounds(mesh.verts, &x0, &y0, &x1, &y1);
        float ex = (float)((1 * CHUNK_SIZE + 5) * TILE);
        float ey = (float)((2 * CHUNK_SIZE + 3) * TILE);
        CHECK(x0 == ex && x1 == ex + TILE && y0 == ey && y1 == ey + TILE,
              "spans exactly tile (chunk*32 + local) * tile_size");

        const float* rgb = tile_palette_rgb(PAL_ROAD);
        int colour_ok = 1;
        for (int i = 0; i < 6; i++) {
            if (abs((int)mesh.verts[i].r - (int)lroundf(rgb[0] * 255.0f)) > 1 ||
                abs((int)mesh.verts[i].g - (int)lroundf(rgb[1] * 255.0f)) > 1 ||
                abs((int)mesh.verts[i].b - (int)lroundf(rgb[2] * 255.0f)) > 1 ||
                mesh.verts[i].a != 255) colour_ok = 0;
        }
        CHECK(colour_ok, "a flat-colour tile carries its palette colour, opaque");
        CHECK(mesh.range_count == 1 && mesh.ranges[0].tileset == 0 &&
              mesh.ranges[0].first == 0 && mesh.ranges[0].count == 6,
              "flat colour is one untextured range");
    }

    printf("\nTEST 3: tiles past the edge of the world are skipped\n");
    {
        for (int i = 0; i < CHUNK_SIZE * CHUNK_SIZE; i++) tiles[i] = PAL_SNOW;
        ChunkMeshInput in = flat_input(tiles);
        in.chunk_x = 0; in.chunk_y = 0;
        in.world_w = 10; in.world_h = 4;               /* only 10x4 tiles exist */
        chunk_mesh_build(&mesh, &in);
        CHECK(mesh.vert_count == 10 * 4 * 6, "only in-world tiles become quads");
    }

    printf("\nTEST 4: a live modification replaces the tile from the file\n");
    {
        memset(tiles, 0, sizeof(tiles));
        tiles[0] = PAL_ROAD;
        TileModification mod;
        memset(&mod, 0, sizeof(mod));
        mod.tile_x = 1 * CHUNK_SIZE;                   /* local (0,0) of chunk (1,2) */
        mod.tile_y = 2 * CHUNK_SIZE;
        mod.modified_tile = PAL_SNOW;
        ChunkMeshInput in = flat_input(tiles);
        in.mods = &mod;
        in.mod_count = 1;
        chunk_mesh_build(&mesh, &in);
        const float* snow = tile_palette_rgb(PAL_SNOW);
        CHECK(mesh.vert_count == 6 &&
              mesh.verts[0].r == (uint8_t)lroundf(snow[0] * 255.0f) &&
              mesh.verts[0].b == (uint8_t)lroundf(snow[2] * 255.0f),
              "the modified tile draws in its new colour");

        mod.modified_tile = TILE_EMPTY;
        chunk_mesh_build(&mesh, &in);
        CHECK(mesh.vert_count == 0, "a modification to empty removes the tile");
    }

    printf("\nTEST 5: textured tiles are grouped by tileset with atlas UVs\n");
    {
        memset(tiles, 0, sizeof(tiles));
        /* packed = tileset << 12 | index. Interleave two tilesets. */
        tiles[0] = (uint16_t)((1 << 12) | 0);
        tiles[1] = (uint16_t)((2 << 12) | 3);
        tiles[2] = (uint16_t)((1 << 12) | 1);
        tiles[3] = (uint16_t)((5 << 12) | 0);          /* tileset 5 has no texture */

        ChunkMeshInput in = flat_input(tiles);
        in.flat_color = 0;
        in.tilesets[1] = (ChunkMeshTileset){ .present = 1, .cols = 2, .rows = 2,
                                             .tile_w = 16, .tile_h = 16, .pad = 0 };
        in.tilesets[2] = (ChunkMeshTileset){ .present = 1, .cols = 2, .rows = 2,
                                             .tile_w = 16, .tile_h = 16, .pad = 4 };
        chunk_mesh_build(&mesh, &in);

        CHECK(mesh.vert_count == 3 * 6, "tiles on a missing tileset are skipped");
        CHECK(mesh.range_count == 2, "one range per tileset used");
        int contiguous = mesh.range_count == 2 &&
                         mesh.ranges[0].tileset == 1 && mesh.ranges[0].count == 12 &&
                         mesh.ranges[1].tileset == 2 && mesh.ranges[1].count == 6 &&
                         mesh.ranges[1].first == 12;
        CHECK(contiguous, "each tileset's quads are contiguous");

        int white = 1;
        for (int i = 0; i < mesh.vert_count; i++)
            if (mesh.verts[i].r != 255 || mesh.verts[i].a != 255) white = 0;
        CHECK(white, "textured vertices are untinted");

        /* Tileset 2, index 3 = (1,1) in a 2x2 atlas of 24px cells (16 + 2*4). */
        float umin = 1e9f, umax = -1e9f;
        for (int i = 12; i < 18; i++) {
            if (mesh.verts[i].u < umin) umin = mesh.verts[i].u;
            if (mesh.verts[i].u > umax) umax = mesh.verts[i].u;
        }
        CHECK(fabsf(umin - 28.0f / 48.0f) < 1e-4f && fabsf(umax - 44.0f / 48.0f) < 1e-4f,
              "padded tileset UVs frame the tile inside its gutter");
    }

    printf("\n");
    if (g_failures) {
        printf("%d ASSERTION%s FAILED\n", g_failures, g_failures == 1 ? "" : "S");
        return 1;
    }
    printf("ALL ASSERTIONS PASSED\n");
    return 0;
}
