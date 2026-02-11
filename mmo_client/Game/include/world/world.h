#ifndef WORLD_H
#define WORLD_H

#include <stdint.h>
#include <stdio.h>

// Note: Camera and GameTextures must be defined before including this header
// This is guaranteed by including world.h through game_types.h which includes camera.h first
// GameTextures should have: grass, water, rock, tree1, shrub1, etc.

// ============================================================================
// CONSTANTS
// ============================================================================

#define CHUNK_SIZE 32              // 32x32 tiles per chunk
#define MAX_LOADED_CHUNKS 64       // Keep 64 chunks in RAM
#define LOAD_RADIUS_CHUNKS 2       // Load 2 chunks around player (5x5 grid)
#define MAX_MODIFICATIONS 256      // Max dynamic tile changes (walls from abilities)

// ============================================================================
// STRUCTURES
// ============================================================================

// Decoration on a tile
typedef struct {
    uint16_t decoration_id;  // 0=none, 1=tree1, 2=shrub1, etc.
    float offset_x;          // Fine positioning within tile (-0.5 to 0.5)
    float offset_y;          // Fine positioning within tile (-0.5 to 0.5)
} Decoration;

// Single chunk of the world
typedef struct {
    uint16_t tiles[CHUNK_SIZE * CHUNK_SIZE];
    uint8_t collision[CHUNK_SIZE * CHUNK_SIZE];
    
    Decoration decorations[16];  // Max 16 decorations per chunk
    int decoration_count;
    
    int chunk_x;
    int chunk_y;
    int last_access_frame;  // For LRU eviction
    int is_loaded;
} Chunk;

// Dynamic tile modification (for ability walls, etc.)
typedef struct {
    int tile_x;
    int tile_y;
    uint16_t modified_tile;
    uint8_t modified_collision;
    float duration;  // -1 for permanent, >0 for temporary
    float time_remaining;
} TileModification;

// Main world state
typedef struct WorldState {
    FILE* world_file;           // Keep file open for fast seeking
    int world_width;            // In tiles
    int world_height;           // In tiles
    int world_width_chunks;     // In chunks
    int world_height_chunks;    // In chunks
    int tile_size;              // Pixel size of each tile
    
    Chunk chunks[MAX_LOADED_CHUNKS];
    int loaded_chunk_count;
    int current_frame;
    
    // Dynamic modifications (ability walls, etc.)
    TileModification modifications[MAX_MODIFICATIONS];
    int modification_count;
} WorldState;

// ============================================================================
// WORLD LIFECYCLE
// ============================================================================

// Initialize world from binary file
int world_init(WorldState* world, const char* world_file_path, int tile_size);

// Update chunk loading based on player position
void world_update_chunks(WorldState* world, float player_x, float player_y);

// Update temporary modifications (timers)
void world_update_modifications(WorldState* world, float delta_time);

// Free world resources
void world_cleanup(WorldState* world);

// ============================================================================
// CHUNK MANAGEMENT
// ============================================================================

// Get chunk from cache or load from disk
Chunk* world_get_chunk(WorldState* world, int chunk_x, int chunk_y);

// Load a chunk from disk
Chunk* world_load_chunk(WorldState* world, int chunk_x, int chunk_y);

// ============================================================================
// TILE ACCESS
// ============================================================================

// Get tile at tile coordinates (checks modifications first)
int world_get_tile(const WorldState* world, int tile_x, int tile_y);

// Set tile at position (not recommended for large worlds, use modifications instead)
void world_set_tile(WorldState* world, int tile_x, int tile_y, int tile_type);

// ============================================================================
// COLLISION CHECKING
// ============================================================================

// Check if world position has collision
int world_check_tile_collision(const WorldState* world, float x, float y);

// Check if box collides with world
int world_check_box_collision(const WorldState* world, float x, float y, float half_size);

// ============================================================================
// COORDINATE CONVERSION
// ============================================================================

// Convert world coords to tile coords
void world_to_tile(const WorldState* world, float wx, float wy, int* tx, int* ty);

// Convert tile coords to world coords (center of tile)
void tile_to_world(const WorldState* world, int tx, int ty, float* wx, float* wy);

// ============================================================================
// DYNAMIC MODIFICATIONS (for ability walls, etc.)
// ============================================================================

// Add a temporary or permanent tile modification
int world_add_modification(WorldState* world, int tile_x, int tile_y, 
                          uint16_t tile_type, uint8_t collision, float duration);

// Remove a modification at position
void world_remove_modification(WorldState* world, int tile_x, int tile_y);

// Clear all modifications
void world_clear_modifications(WorldState* world);

// ============================================================================
// RENDERING
// ============================================================================

// Render visible tiles (uses chunked loading)
void world_render(const WorldState* world, const Camera* camera, const GameTextures* textures);

// Render decorations (call after world_render for proper layering)
void world_render_decorations(const WorldState* world, const Camera* camera, const GameTextures* textures);

#endif // WORLD_H