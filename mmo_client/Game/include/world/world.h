#ifndef WORLD_H
#define WORLD_H

#include <stdint.h>
#include <stdio.h>

// ============================================================================
// CONSTANTS
// ============================================================================

#define CHUNK_SIZE        32    // 32x32 tiles per chunk
#define MAX_LOADED_CHUNKS 64    // Keep 64 chunks in RAM
#define LOAD_RADIUS_CHUNKS 2    // Load 2 chunks around player (5x5 grid)
#define MAX_MODIFICATIONS 256   // Max dynamic tile changes (walls from abilities)
#define MAX_TILESETS      16    // Max tilesets per world

// Packed tile format:  bits 15-12 = tileset_id (1-based, 0=empty)
//                      bits 11-0  = flat tile index within that tileset
#define TILE_EMPTY 0

// ============================================================================
// STRUCTURES
// ============================================================================

// Tileset metadata (loaded from world.dat header)
typedef struct {
    char     path[128];   // Path relative to game CWD
    uint16_t cols;        // Tileset width in tiles
    uint16_t rows;        // Tileset height in tiles
} TilesetInfo;

// Single chunk of the world
typedef struct {
    uint16_t tiles[CHUNK_SIZE * CHUNK_SIZE];                  // Base layer
    uint16_t overlay_floor_tiles[CHUNK_SIZE * CHUNK_SIZE];    // Floor (always visible, behind player)
    uint16_t overlay_interior_tiles[CHUNK_SIZE * CHUNK_SIZE]; // Interior + Decoration (only when inside, behind player)
    uint16_t overlay_above_tiles[CHUNK_SIZE * CHUNK_SIZE];    // Wall + Roof (in front of player when inside)
    uint8_t  collision[CHUNK_SIZE * CHUNK_SIZE];

    // Display list cache (one GL list per visual layer)
    unsigned int dl_base;             // 0 = not compiled yet
    unsigned int dl_overlay_floor;
    unsigned int dl_overlay_interior;
    unsigned int dl_overlay_above;
    int          dl_dirty;            // 1 = needs rebuild this frame

    int chunk_x;
    int chunk_y;
    int last_access_frame;
    int is_loaded;
} Chunk;

// Dynamic tile modification (for ability walls, etc.)
typedef struct {
    int tile_x;
    int tile_y;
    uint16_t modified_tile;
    uint8_t  modified_collision;
    float    duration;       // -1 = permanent, >0 = temporary
    float    time_remaining;
} TileModification;

// Main world state
typedef struct WorldState {
    FILE* world_file;
    int   world_width;          // In tiles
    int   world_height;         // In tiles
    int   world_width_chunks;   // In chunks
    int   world_height_chunks;  // In chunks
    int   tile_size;            // Pixels per tile

    // Tileset system (loaded from world.dat header)
    int          tileset_count;
    TilesetInfo  tilesets[MAX_TILESETS];            // Index 0 unused (empty)
    unsigned int tileset_textures[MAX_TILESETS];    // OpenGL texture IDs

    // File offsets (computed after reading variable-length header)
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

// ============================================================================
// WORLD LIFECYCLE
// ============================================================================

int  world_init(WorldState* world, const char* world_file_path, int tile_size);
void world_update_chunks(WorldState* world, float player_x, float player_y);
void world_update_modifications(WorldState* world, float delta_time);
void world_cleanup(WorldState* world);

// ============================================================================
// CHUNK MANAGEMENT
// ============================================================================

Chunk* world_get_chunk(WorldState* world, int chunk_x, int chunk_y);
Chunk* world_load_chunk(WorldState* world, int chunk_x, int chunk_y);

// ============================================================================
// TILE ACCESS
// ============================================================================

int  world_get_tile(const WorldState* world, int tile_x, int tile_y);
void world_set_tile(WorldState* world, int tile_x, int tile_y, int tile_type);

// ============================================================================
// COLLISION CHECKING
// ============================================================================

int world_check_tile_collision(const WorldState* world, float x, float y);
int world_check_box_collision(const WorldState* world, float x, float y, float half_size);

// ============================================================================
// COORDINATE CONVERSION
// ============================================================================

void world_to_tile(const WorldState* world, float wx, float wy, int* tx, int* ty);
void tile_to_world(const WorldState* world, int tx, int ty, float* wx, float* wy);

// ============================================================================
// DYNAMIC MODIFICATIONS
// ============================================================================

int  world_add_modification(WorldState* world, int tile_x, int tile_y,
                             uint16_t tile_type, uint8_t collision, float duration);
void world_remove_modification(WorldState* world, int tile_x, int tile_y);
void world_clear_modifications(WorldState* world);

// ============================================================================
// RENDERING
// ============================================================================

// Render base ground layer (camera already applied by caller)
void world_render(const WorldState* world, const Camera* camera);

// Render floor layer — always visible, behind player
void world_render_overlay_floor(const WorldState* world, const Camera* camera);

// Render interior/decoration layer — only when player is inside, behind player
void world_render_overlay_interior(const WorldState* world, const Camera* camera);

// Render wall/roof layer — used when inside (all tiles over player, display list)
void world_render_overlay_above(const WorldState* world, const Camera* camera);

// Y-sorted wall/roof — call north before player, south after (exterior use)
void world_render_overlay_above_north(const WorldState* world, const Camera* camera, int player_ty);
void world_render_overlay_above_south(const WorldState* world, const Camera* camera, int player_ty);

// Returns 1 if world position (wx, wy) is inside a building (has a floor tile)
int world_is_inside(const WorldState* world, float wx, float wy);

#endif // WORLD_H
