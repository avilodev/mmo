/**
 * @file
 * Build ground-shader vertex data for one chunk tile layer.
 */

#include "world/chunk_mesh.h"
#include "texture/texture_atlas.h"

#include <math.h>
#include <string.h>

/** The tile at a world coordinate, after any live modification. */
static uint16_t effective_tile(const ChunkMeshInput* in, int slot, int wx, int wy) {
    for (int m = 0; m < in->mod_count; m++) {
        if (in->mods[m].tile_x == wx && in->mods[m].tile_y == wy)
            return in->mods[m].modified_tile;
    }
    return in->tiles[slot];
}

/** The tileset a packed tile draws from, or -1 when it draws nothing. */
static int tile_tileset(const ChunkMeshInput* in, uint16_t packed) {
    if (packed == TILE_EMPTY) return -1;
    if (in->flat_color) return tile_palette_rgb(packed) ? 0 : -1;

    int ts = (packed >> 12) & 0xF;
    const ChunkMeshTileset* set = &in->tilesets[ts];
    if (!set->present || set->cols <= 0 || set->rows <= 0) return -1;
    return ts;
}

static uint8_t to_byte(float c) {
    if (c <= 0.0f) return 0;
    if (c >= 1.0f) return 255;
    return (uint8_t)lroundf(c * 255.0f);
}

static void emit_quad(ChunkVertex* v, float x, float y, float s,
                      float u0, float v0, float u1, float v1,
                      uint8_t r, uint8_t g, uint8_t b) {
    const ChunkVertex corners[4] = {
        { x,     y,     u0, v0, r, g, b, 255 },
        { x + s, y,     u1, v0, r, g, b, 255 },
        { x + s, y + s, u1, v1, r, g, b, 255 },
        { x,     y + s, u0, v1, r, g, b, 255 },
    };
    v[0] = corners[0]; v[1] = corners[1]; v[2] = corners[2];
    v[3] = corners[0]; v[4] = corners[2]; v[5] = corners[3];
}

void chunk_mesh_build(ChunkMesh* out, const ChunkMeshInput* in) {
    int counts[MAX_TILESETS];
    memset(counts, 0, sizeof(counts));

    int tx0 = in->chunk_x * CHUNK_SIZE;
    int ty0 = in->chunk_y * CHUNK_SIZE;

    /* First pass: how many quads each tileset gets, so each can be written
     * into its own contiguous range in one pass. */
    for (int ly = 0; ly < CHUNK_SIZE; ly++) {
        for (int lx = 0; lx < CHUNK_SIZE; lx++) {
            int wx = tx0 + lx, wy = ty0 + ly;
            if (wx >= in->world_w || wy >= in->world_h) continue;
            int ts = tile_tileset(in, effective_tile(in, ly * CHUNK_SIZE + lx, wx, wy));
            if (ts >= 0) counts[ts]++;
        }
    }

    int next[MAX_TILESETS];
    out->range_count = 0;
    out->vert_count  = 0;
    for (int ts = 0; ts < MAX_TILESETS; ts++) {
        next[ts] = out->vert_count;
        if (counts[ts] == 0) continue;
        ChunkDrawRange* range = &out->ranges[out->range_count++];
        range->tileset = ts;
        range->first   = out->vert_count;
        range->count   = counts[ts] * 6;
        out->vert_count += range->count;
    }

    float size = (float)in->tile_size;
    for (int ly = 0; ly < CHUNK_SIZE; ly++) {
        for (int lx = 0; lx < CHUNK_SIZE; lx++) {
            int wx = tx0 + lx, wy = ty0 + ly;
            if (wx >= in->world_w || wy >= in->world_h) continue;
            uint16_t packed = effective_tile(in, ly * CHUNK_SIZE + lx, wx, wy);
            int ts = tile_tileset(in, packed);
            if (ts < 0) continue;

            float x = (float)wx * size;
            float y = (float)wy * size;
            ChunkVertex* v = &out->verts[next[ts]];
            next[ts] += 6;

            if (in->flat_color) {
                const float* rgb = tile_palette_rgb(packed);
                emit_quad(v, x, y, size, 0.0f, 0.0f, 0.0f, 0.0f,
                          to_byte(rgb[0]), to_byte(rgb[1]), to_byte(rgb[2]));
            } else {
                const ChunkMeshTileset* set = &in->tilesets[ts];
                float u0, v0, u1, v1;
                texture_atlas_uv(set->cols, set->rows, set->tile_w, set->tile_h,
                                 set->pad, packed & 0xFFF, &u0, &v0, &u1, &v1);
                emit_quad(v, x, y, size, u0, v0, u1, v1, 255, 255, 255);
            }
        }
    }
}
