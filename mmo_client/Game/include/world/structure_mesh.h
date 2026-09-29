#ifndef STRUCTURE_MESH_H
#define STRUCTURE_MESH_H

/**
 * @file
 * Raise the world's solid tiles into 3D: buildings, the city wall, mountains.
 *
 * The server validates every step against the collision layer of world.dat, so
 * the only honest 3D world is one grown out of that same layer: a wall stands
 * exactly where the server says a wall is, tile for tile, and nothing solid is
 * drawn where the server lets a player walk (D4). Height is visual only.
 *
 * Pure CPU work with no OpenGL, so structure_mesh_test.c pins it headless.
 * world.c feeds it the resident chunks and uploads the result.
 */

#include <stdint.h>

#include "world/chunk_mesh.h"

/** What stands on a tile. */
typedef enum {
    STRUCTURE_NONE = 0,
    STRUCTURE_BUILDING,   /**< Walls up to the eaves, hip roof above. */
    STRUCTURE_BLOCK,      /**< A flat-topped mass: city wall, courtyard wall, tower. */
    STRUCTURE_ROCK,       /**< Rugged mountain rock rising from the ground. */
    STRUCTURE_COLUMN,     /**< A stone column on a plinth (one per footprint). */
    STRUCTURE_LAMP,       /**< A lamp post with a lit lantern. */
    STRUCTURE_TREE        /**< A tree in a stone planter. */
} StructureKind;

/** A tile's structure and the palette index it came from, which picks a
 *  block's height and colour. */
typedef struct {
    StructureKind kind;
    uint16_t      style;
} StructureTile;

/** Wall height of a building, world units (a little over two characters). */
#define STRUCTURE_BUILDING_EAVE   44.0f
/** Roof rise per tile in from the building's edge, and its cap. */
#define STRUCTURE_ROOF_RISE       7.0f
#define STRUCTURE_ROOF_MAX_RISE   44.0f
/** Block heights by what they are. */
#define STRUCTURE_CITY_WALL_HEIGHT      80.0f
#define STRUCTURE_COURTYARD_WALL_HEIGHT 26.0f
#define STRUCTURE_GATE_TOWER_HEIGHT     64.0f
/** Mountain rock: height where a corner is surrounded by rock, plus noise. */
#define STRUCTURE_ROCK_BASE       22.0f
#define STRUCTURE_ROCK_NOISE      30.0f
/** How far a roof is searched for its edge, in tiles. Buildings are at most
 *  BUILDING_MAX (16) tiles across, so half of this is ample. */
#define STRUCTURE_SCAN_LIMIT      24

/** Worst case per tile: a two-triangle top and four two-triangle sides; props
 *  are a few hundred vertices per footprint of several tiles, well inside it.
 *  The builder stops rather than overrun. */
#define STRUCTURE_MESH_MAX_VERTS (CHUNK_SIZE * CHUNK_SIZE * 30)

/** Classify one tile from its base layer, roof layer and collision.
 *
 * Only solid tiles become structures; a tile a player can stand on never
 * does, whatever its colour.
 */
StructureTile structure_tile_of(uint16_t base, uint16_t above, uint8_t collision);

/** Answer "what stands on tile (tx, ty)", for any tile, including ones in
 *  neighbouring chunks. Out-of-world or unknown tiles are STRUCTURE_NONE. */
typedef StructureTile (*StructureSampler)(void* ctx, int tx, int ty);

typedef struct {
    int chunk_x, chunk_y;
    int world_w, world_h;       /**< In tiles. */
    int tile_size;              /**< World units per tile. */
    StructureSampler tile_at;
    void* ctx;
} StructureMeshInput;

/** Build a chunk's structures into `out` (STRUCTURE_MESH_MAX_VERTS long).
 *  Faces are lit by the world sun and baked into the vertex colour.
 *  @return The vertex count, a multiple of three. */
int structure_mesh_build(ChunkVertex* out, const StructureMeshInput* in);

/** Height of a building's roof at a tile corner (grid vertex), for tests and
 *  for anything that needs to know how tall a building is at a point. */
float structure_roof_height(const StructureMeshInput* in, int vx, int vy);

#endif /* STRUCTURE_MESH_H */
