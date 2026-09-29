#ifndef CHUNK_MESH_H
#define CHUNK_MESH_H

/**
 * @file
 * Turn one chunk tile layer into vertex data for the ground shader.
 *
 * Pure CPU work with no OpenGL, so it is unit-tested headless; world.c
 * uploads the result into the chunk's vertex buffer.
 */

#include <stdint.h>

#include "world/world.h"

/** Two triangles for every tile of a chunk. */
#define CHUNK_MESH_MAX_VERTS (CHUNK_SIZE * CHUNK_SIZE * 6)

/** One world vertex: world position and height, atlas UV, and an RGBA8 tint.
 *
 *  Tile layers lie flat (h = 0); 3D structures (structure_mesh.h) use the same
 *  vertex with a height, so one shader draws both. */
typedef struct {
    float   x, y, h;
    float   u, v;
    uint8_t r, g, b, a;
} ChunkVertex;

/** How to address one tileset's tiles. */
typedef struct {
    int present;          /**< Nonzero when the tileset has a texture. */
    int cols, rows;       /**< Tiles across and down. */
    int tile_w, tile_h;   /**< Tile size in texels, before padding. */
    int pad;              /**< Gutter texels around each tile (texture_atlas.h). */
} ChunkMeshTileset;

/** Everything the builder reads. */
typedef struct {
    const uint16_t* tiles;          /**< CHUNK_SIZE * CHUNK_SIZE packed tiles. */
    int chunk_x, chunk_y;
    int world_w, world_h;           /**< In tiles; tiles past these are skipped. */
    int tile_size;                  /**< World units per tile. */
    int flat_color;                 /**< Tiles are palette indices, not tileset coordinates. */
    ChunkMeshTileset tilesets[MAX_TILESETS];
    const TileModification* mods;   /**< Overrides applied on top of `tiles`, or NULL. */
    int mod_count;
} ChunkMeshInput;

/** The built mesh. `verts` is supplied by the caller, CHUNK_MESH_MAX_VERTS long. */
typedef struct {
    ChunkVertex*   verts;
    int            vert_count;
    ChunkDrawRange ranges[MAX_TILESETS];
    int            range_count;
} ChunkMesh;

/** Build a layer's quads, grouped by tileset so each texture binds once. */
void chunk_mesh_build(ChunkMesh* out, const ChunkMeshInput* in);

#endif /* CHUNK_MESH_H */
