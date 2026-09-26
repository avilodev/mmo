#ifndef WORLD_H
#define WORLD_H

#include <stdint.h>
#include <stdio.h>

#include "camera/camera.h"
#include "world/tile_palette.h"

/** Configure chunk dimensions and fixed world-cache capacities. */
#define CHUNK_SIZE        32
#define MAX_LOADED_CHUNKS 64
/** Three, not two: the tilted camera sees further than the top-down one did,
 *  and at full zoom-out, turned 45 degrees, its far corners reach past two
 *  chunks from the player (camera_math_test TEST 7, decision D15). 7x7 = 49
 *  resident chunks, inside MAX_LOADED_CHUNKS. */
#define LOAD_RADIUS_CHUNKS 3
#define MAX_MODIFICATIONS 256
#define MAX_TILESETS      16

/** Represent an empty packed tile; nonzero tiles use four tileset bits and twelve index bits. */
#define TILE_EMPTY 0

/** Describe one tileset record loaded from the world header. */
typedef struct {
    char     path[128];   /**< Path relative to the client working directory. */
    uint16_t cols;        /**< Tileset width in tiles. */
    uint16_t rows;        /**< Tileset height in tiles. */
    int      tile_w;      /**< Tile size in texels, known once the texture loads. */
    int      tile_h;
    int      pad;         /**< Gutter texels the loaded atlas carries (texture_atlas.h). */
} TilesetInfo;

/** The tile layers a chunk draws, in draw order. */
enum {
    CHUNK_LAYER_BASE = 0,
    CHUNK_LAYER_FLOOR,
    CHUNK_LAYER_INTERIOR,
    CHUNK_LAYER_ABOVE,
    CHUNK_LAYER_COUNT
};

/** A contiguous run of a chunk layer's vertices that share one tileset. */
typedef struct {
    int tileset;   /**< Tileset slot; 0 in a flat-colour world. */
    int first;     /**< First vertex. */
    int count;     /**< Vertex count, a multiple of six. */
} ChunkDrawRange;

/** One chunk layer uploaded to the GPU. */
typedef struct {
    unsigned int   vbo;           /**< Vertex buffer, or 0 until built. */
    int            vert_count;
    ChunkDrawRange ranges[MAX_TILESETS];
    int            range_count;
} ChunkLayerGpu;

/** Cache one world chunk's tile layers, collision, and GPU vertex buffers. */
typedef struct {
    uint16_t tiles[CHUNK_SIZE * CHUNK_SIZE];
    uint16_t overlay_floor_tiles[CHUNK_SIZE * CHUNK_SIZE];
    uint16_t overlay_interior_tiles[CHUNK_SIZE * CHUNK_SIZE];
    uint16_t overlay_above_tiles[CHUNK_SIZE * CHUNK_SIZE];
    uint8_t  collision[CHUNK_SIZE * CHUNK_SIZE];

    ChunkLayerGpu gpu[CHUNK_LAYER_COUNT];
    int           gpu_dirty;       /**< Rebuild every layer before the next draw. */

    int chunk_x;
    int chunk_y;
    int last_access_frame;
    int is_loaded;
} Chunk;

/** Track one temporary or permanent tile and collision override. */
typedef struct {
    int tile_x;
    int tile_y;
    uint16_t modified_tile;
    uint8_t  modified_collision;
    float    duration;       /**< Negative for permanent, positive seconds for temporary. */
    float    time_remaining;
} TileModification;

/** Own the world file, tilesets, chunk cache, and dynamic modifications. */
typedef struct WorldState {
    FILE* world_file;
    int   world_width;          /**< Width in tiles. */
    int   world_height;         /**< Height in tiles. */
    int   world_width_chunks;   /**< Width in chunks. */
    int   world_height_chunks;  /**< Height in chunks. */
    int   tile_size;            /**< Tile edge length in pixels. */
    int   flat_color_mode;      /**< Nonzero when tiles are palette indices, not tileset coordinates. */

    int          tileset_count;
    TilesetInfo  tilesets[MAX_TILESETS];            /**< Index 0 is reserved for empty tiles. */
    unsigned int tileset_textures[MAX_TILESETS];

    /** Byte offsets for fixed-size layer payloads in world_file. */
    long base_layer_offset;
    long overlay_floor_offset;
    long overlay_interior_offset;
    long overlay_above_offset;
    long collision_offset;

    Chunk chunks[MAX_LOADED_CHUNKS];
    int   loaded_chunk_count;
    int   current_frame;

    TileModification modifications[MAX_MODIFICATIONS];
    int modification_count;
} WorldState;

int  world_init(WorldState* world, const char* world_file_path, int tile_size);
void world_update_chunks(WorldState* world, float player_x, float player_y);
void world_update_modifications(WorldState* world, float delta_time);
void world_cleanup(WorldState* world);

Chunk* world_get_chunk(WorldState* world, int chunk_x, int chunk_y);
Chunk* world_load_chunk(WorldState* world, int chunk_x, int chunk_y);

int  world_get_tile(const WorldState* world, int tile_x, int tile_y);
void world_set_tile(WorldState* world, int tile_x, int tile_y, int tile_type);

int world_check_tile_collision(const WorldState* world, float x, float y);
int world_check_box_collision(const WorldState* world, float x, float y, float half_size);

void world_to_tile(const WorldState* world, float wx, float wy, int* tx, int* ty);
void tile_to_world(const WorldState* world, int tx, int ty, float* wx, float* wy);

int  world_add_modification(WorldState* world, int tile_x, int tile_y,
                             uint16_t tile_type, uint8_t collision, float duration);
void world_remove_modification(WorldState* world, int tile_x, int tile_y);
void world_clear_modifications(WorldState* world);

void world_render(const WorldState* world, const Camera* camera);

void world_render_overlay_floor(const WorldState* world, const Camera* camera);

void world_render_overlay_interior(const WorldState* world, const Camera* camera);

void world_render_overlay_above(const WorldState* world, const Camera* camera);

/** Draw the roof layer with a faded hole around a point (3D view, indoors). */
void world_render_overlay_above_faded(const WorldState* world, const Camera* camera,
                                      float center_x, float center_y,
                                      float inner_radius, float outer_radius);

void world_render_overlay_above_north(const WorldState* world, const Camera* camera, int player_ty);
void world_render_overlay_above_south(const WorldState* world, const Camera* camera, int player_ty);

int world_is_inside(const WorldState* world, float wx, float wy);

#endif // WORLD_H
