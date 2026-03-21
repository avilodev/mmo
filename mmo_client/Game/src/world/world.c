#include "game_types.h"  // Includes world.h, camera.h, and defines GameTextures
#include "renderer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// ============================================================================
// WORLD LIFECYCLE
// ============================================================================

int world_init(WorldState* world, const char* world_file_path, int tile_size) {
    memset(world, 0, sizeof(WorldState));
    
    world->world_file = fopen(world_file_path, "rb");
    if (!world->world_file) {
        fprintf(stderr, "[WORLD] Failed to open %s\n", world_file_path);
        return 0;
    }
    
    // Read header (width, height, tile_size from file)
    fread(&world->world_width, sizeof(int), 1, world->world_file);
    fread(&world->world_height, sizeof(int), 1, world->world_file);
    int file_tile_size;
    fread(&file_tile_size, sizeof(int), 1, world->world_file);
    
    world->tile_size = tile_size;  // Use parameter, not file value
    world->world_width_chunks = (world->world_width + CHUNK_SIZE - 1) / CHUNK_SIZE;
    world->world_height_chunks = (world->world_height + CHUNK_SIZE - 1) / CHUNK_SIZE;
    world->loaded_chunk_count = 0;
    world->current_frame = 0;
    world->modification_count = 0;
    
    // Initialize all chunks as unloaded
    for (int i = 0; i < MAX_LOADED_CHUNKS; i++) {
        world->chunks[i].is_loaded = 0;
    }
    
    printf("[WORLD] Initialized %dx%d tiles (%dx%d chunks)\n",
           world->world_width, world->world_height,
           world->world_width_chunks, world->world_height_chunks);
    printf("[WORLD] File: %s, Tile size: %d pixels\n", world_file_path, tile_size);
    printf("[WORLD] Memory usage: ~%d KB for %d chunks\n", 
           (int)((sizeof(Chunk) * MAX_LOADED_CHUNKS) / 1024), MAX_LOADED_CHUNKS);
    
    return 1;
}

void world_update_chunks(WorldState* world, float player_x, float player_y) {
    if (!world->world_file || world->tile_size == 0) return;

    world->current_frame++;

    // Calculate which chunk player is in
    int player_chunk_x = (int)(player_x / world->tile_size) / CHUNK_SIZE;
    int player_chunk_y = (int)(player_y / world->tile_size) / CHUNK_SIZE;
    
    // Load chunks in radius around player
    for (int cy = player_chunk_y - LOAD_RADIUS_CHUNKS; 
         cy <= player_chunk_y + LOAD_RADIUS_CHUNKS; cy++) {
        for (int cx = player_chunk_x - LOAD_RADIUS_CHUNKS; 
             cx <= player_chunk_x + LOAD_RADIUS_CHUNKS; cx++) {
            world_get_chunk(world, cx, cy);  // Loads if not already loaded
        }
    }
}

void world_update_modifications(WorldState* world, float delta_time) {
    for (int i = 0; i < world->modification_count; i++) {
        if (world->modifications[i].duration > 0) {
            world->modifications[i].time_remaining -= delta_time;
            
            if (world->modifications[i].time_remaining <= 0) {
                // Remove expired modification
                world->modifications[i] = world->modifications[world->modification_count - 1];
                world->modification_count--;
                i--;
            }
        }
    }
}

void world_cleanup(WorldState* world) {
    if (world->world_file) {
        fclose(world->world_file);
        world->world_file = NULL;
    }
    
    world->loaded_chunk_count = 0;
    world->modification_count = 0;
    
    printf("[WORLD] Cleaned up\n");
}

// ============================================================================
// CHUNK MANAGEMENT
// ============================================================================

Chunk* world_get_chunk(WorldState* world, int chunk_x, int chunk_y) {
    // Bounds check
    if (chunk_x < 0 || chunk_x >= world->world_width_chunks ||
        chunk_y < 0 || chunk_y >= world->world_height_chunks) {
        return NULL;
    }
    
    // Check if already loaded
    for (int i = 0; i < MAX_LOADED_CHUNKS; i++) {
        if (world->chunks[i].is_loaded &&
            world->chunks[i].chunk_x == chunk_x &&
            world->chunks[i].chunk_y == chunk_y) {
            world->chunks[i].last_access_frame = world->current_frame;
            return &world->chunks[i];
        }
    }
    
    // Not loaded - need to load it
    return world_load_chunk(world, chunk_x, chunk_y);
}

Chunk* world_load_chunk(WorldState* world, int chunk_x, int chunk_y) {
    if (!world->world_file) return NULL;

    Chunk* target = NULL;

    if (world->loaded_chunk_count < MAX_LOADED_CHUNKS) {
        // Use next empty slot
        for (int i = 0; i < MAX_LOADED_CHUNKS; i++) {
            if (!world->chunks[i].is_loaded) {
                target = &world->chunks[i];
                world->loaded_chunk_count++;
                break;
            }
        }
    } else {
        // Evict least recently used chunk
        int oldest_frame = world->current_frame;
        for (int i = 0; i < MAX_LOADED_CHUNKS; i++) {
            if (world->chunks[i].last_access_frame < oldest_frame) {
                oldest_frame = world->chunks[i].last_access_frame;
                target = &world->chunks[i];
            }
        }
    }
    
    if (!target) {
        fprintf(stderr, "[WORLD] Failed to find chunk slot!\n");
        return NULL;
    }
    
    // Initialize chunk
    target->chunk_x = chunk_x;
    target->chunk_y = chunk_y;
    target->is_loaded = 1;
    target->last_access_frame = world->current_frame;
    target->decoration_count = 0;  // Initialize decorations
    
    // Calculate file offset for this chunk
    size_t header_size = sizeof(int) * 3;
    // We need to read the chunk's tiles from the file
    // The file stores tiles sequentially by row, so we need to read
    // CHUNK_SIZE rows, each containing CHUNK_SIZE tiles
    
    int tile_start_x = chunk_x * CHUNK_SIZE;
    int tile_start_y = chunk_y * CHUNK_SIZE;
    
    // Read tiles for this chunk
    for (int local_y = 0; local_y < CHUNK_SIZE; local_y++) {
        int world_y = tile_start_y + local_y;
        if (world_y >= world->world_height) {
            // Fill with empty tiles if beyond world bounds
            for (int local_x = 0; local_x < CHUNK_SIZE; local_x++) {
                target->tiles[local_y * CHUNK_SIZE + local_x] = 0;
            }
            continue;
        }
        
        // Calculate file position for this row
        size_t row_offset = header_size + 
                          (world_y * world->world_width + tile_start_x) * sizeof(uint16_t);
        fseek(world->world_file, row_offset, SEEK_SET);
        
        // Read the row (or partial row if at edge)
        int tiles_to_read = CHUNK_SIZE;
        if (tile_start_x + CHUNK_SIZE > world->world_width) {
            tiles_to_read = world->world_width - tile_start_x;
        }
        
        fread(&target->tiles[local_y * CHUNK_SIZE], sizeof(uint16_t), 
              tiles_to_read, world->world_file);
        
        // Fill remainder with empty if at edge
        for (int local_x = tiles_to_read; local_x < CHUNK_SIZE; local_x++) {
            target->tiles[local_y * CHUNK_SIZE + local_x] = 0;
        }
    }
    
    // Read collision data
    size_t total_tiles = world->world_width * world->world_height;
    size_t collision_base = header_size + (total_tiles * sizeof(uint16_t));
    
    for (int local_y = 0; local_y < CHUNK_SIZE; local_y++) {
        int world_y = tile_start_y + local_y;
        if (world_y >= world->world_height) {
            for (int local_x = 0; local_x < CHUNK_SIZE; local_x++) {
                target->collision[local_y * CHUNK_SIZE + local_x] = 1;  // Solid
            }
            continue;
        }
        
        size_t row_offset = collision_base + 
                          (world_y * world->world_width + tile_start_x) * sizeof(uint8_t);
        fseek(world->world_file, row_offset, SEEK_SET);
        
        int tiles_to_read = CHUNK_SIZE;
        if (tile_start_x + CHUNK_SIZE > world->world_width) {
            tiles_to_read = world->world_width - tile_start_x;
        }
        
        fread(&target->collision[local_y * CHUNK_SIZE], sizeof(uint8_t), 
              tiles_to_read, world->world_file);
        
        for (int local_x = tiles_to_read; local_x < CHUNK_SIZE; local_x++) {
            target->collision[local_y * CHUNK_SIZE + local_x] = 1;  // Solid
        }
    }
    
    // Read decoration data
    size_t decorations_base = header_size + 
                             (total_tiles * sizeof(uint16_t)) + 
                             (total_tiles * sizeof(uint8_t));
    
    // Calculate offset for this chunk's decorations
    size_t chunk_index = chunk_y * world->world_width_chunks + chunk_x;
    
    // Each chunk has: 1 byte count + up to 16 decorations
    size_t chunk_deco_offset = decorations_base;
    for (size_t i = 0; i < chunk_index; i++) {
        // Skip to this chunk by reading previous chunks' decoration counts
        fseek(world->world_file, chunk_deco_offset, SEEK_SET);
        uint8_t count;
        fread(&count, sizeof(uint8_t), 1, world->world_file);
        chunk_deco_offset += sizeof(uint8_t) + (count * sizeof(Decoration));
    }
    
    // Read this chunk's decorations
    fseek(world->world_file, chunk_deco_offset, SEEK_SET);
    uint8_t deco_count;
    fread(&deco_count, sizeof(uint8_t), 1, world->world_file);
    
    if (deco_count > 16) deco_count = 16;  // Safety clamp
    target->decoration_count = deco_count;
    
    if (deco_count > 0) {
        fread(target->decorations, sizeof(Decoration), deco_count, world->world_file);
    }
    
    return target;
}

// ============================================================================
// TILE ACCESS
// ============================================================================

int world_get_tile(const WorldState* world, int tx, int ty) {
    if (tx < 0 || tx >= world->world_width || ty < 0 || ty >= world->world_height) {
        return -1;
    }
    
    // Check modifications first
    for (int i = 0; i < world->modification_count; i++) {
        if (world->modifications[i].tile_x == tx && 
            world->modifications[i].tile_y == ty) {
            return world->modifications[i].modified_tile;
        }
    }
    
    // Get from chunk
    int chunk_x = tx / CHUNK_SIZE;
    int chunk_y = ty / CHUNK_SIZE;
    int local_x = tx % CHUNK_SIZE;
    int local_y = ty % CHUNK_SIZE;
    
    Chunk* chunk = world_get_chunk((WorldState*)world, chunk_x, chunk_y);
    if (!chunk) return -1;
    
    return chunk->tiles[local_y * CHUNK_SIZE + local_x];
}

void world_set_tile(WorldState* world, int tx, int ty, int tile_type) {
    if (tx < 0 || tx >= world->world_width || ty < 0 || ty >= world->world_height) {
        return;
    }
    
    int chunk_x = tx / CHUNK_SIZE;
    int chunk_y = ty / CHUNK_SIZE;
    int local_x = tx % CHUNK_SIZE;
    int local_y = ty % CHUNK_SIZE;
    
    Chunk* chunk = world_get_chunk(world, chunk_x, chunk_y);
    if (!chunk) return;
    
    chunk->tiles[local_y * CHUNK_SIZE + local_x] = tile_type;
}

// ============================================================================
// COLLISION CHECKING
// ============================================================================

int world_check_tile_collision(const WorldState* world, float x, float y) {
    int tx = (int)(x / world->tile_size);
    int ty = (int)(y / world->tile_size);
    
    if (tx < 0 || tx >= world->world_width || ty < 0 || ty >= world->world_height) {
        return 1;  // Out of bounds = collision
    }
    
    // Check modifications first
    for (int i = 0; i < world->modification_count; i++) {
        if (world->modifications[i].tile_x == tx && 
            world->modifications[i].tile_y == ty) {
            return world->modifications[i].modified_collision;
        }
    }
    
    // Get from chunk
    int chunk_x = tx / CHUNK_SIZE;
    int chunk_y = ty / CHUNK_SIZE;
    int local_x = tx % CHUNK_SIZE;
    int local_y = ty % CHUNK_SIZE;
    
    Chunk* chunk = world_get_chunk((WorldState*)world, chunk_x, chunk_y);
    if (!chunk) return 1;
    
    return chunk->collision[local_y * CHUNK_SIZE + local_x];
}

int world_check_box_collision(const WorldState* world, float x, float y, float half_size) {
    // Check all 4 corners
    if (world_check_tile_collision(world, x - half_size, y - half_size)) return 1;
    if (world_check_tile_collision(world, x + half_size, y - half_size)) return 1;
    if (world_check_tile_collision(world, x - half_size, y + half_size)) return 1;
    if (world_check_tile_collision(world, x + half_size, y + half_size)) return 1;
    return 0;
}

// ============================================================================
// COORDINATE CONVERSION
// ============================================================================

void world_to_tile(const WorldState* world, float wx, float wy, int* tx, int* ty) {
    *tx = (int)(wx / world->tile_size);
    *ty = (int)(wy / world->tile_size);
}

void tile_to_world(const WorldState* world, int tx, int ty, float* wx, float* wy) {
    *wx = tx * world->tile_size + world->tile_size / 2.0f;
    *wy = ty * world->tile_size + world->tile_size / 2.0f;
}

// ============================================================================
// DYNAMIC MODIFICATIONS
// ============================================================================

int world_add_modification(WorldState* world, int tile_x, int tile_y, 
                          uint16_t tile_type, uint8_t collision, float duration) {
    if (world->modification_count >= MAX_MODIFICATIONS) {
        fprintf(stderr, "[WORLD] Max modifications reached!\n");
        return 0;
    }
    
    // Check if already modified at this position
    for (int i = 0; i < world->modification_count; i++) {
        if (world->modifications[i].tile_x == tile_x &&
            world->modifications[i].tile_y == tile_y) {
            // Update existing modification
            world->modifications[i].modified_tile = tile_type;
            world->modifications[i].modified_collision = collision;
            world->modifications[i].duration = duration;
            world->modifications[i].time_remaining = duration;
            return 1;
        }
    }
    
    // Add new modification
    TileModification* mod = &world->modifications[world->modification_count];
    mod->tile_x = tile_x;
    mod->tile_y = tile_y;
    mod->modified_tile = tile_type;
    mod->modified_collision = collision;
    mod->duration = duration;
    mod->time_remaining = duration;
    world->modification_count++;
    
    return 1;
}

void world_remove_modification(WorldState* world, int tile_x, int tile_y) {
    for (int i = 0; i < world->modification_count; i++) {
        if (world->modifications[i].tile_x == tile_x &&
            world->modifications[i].tile_y == tile_y) {
            // Remove by swapping with last
            world->modifications[i] = world->modifications[world->modification_count - 1];
            world->modification_count--;
            return;
        }
    }
}

void world_clear_modifications(WorldState* world) {
    world->modification_count = 0;
}

// ============================================================================
// RENDERING
// ============================================================================

void world_render(const WorldState* world, const Camera* camera, const GameTextures* textures) {
    // Calculate visible tile range
    int start_x = (int)((camera->x - camera->viewport_width / (2.0f * camera->zoom)) / world->tile_size) - 1;
    int end_x = (int)((camera->x + camera->viewport_width / (2.0f * camera->zoom)) / world->tile_size) + 1;
    int start_y = (int)((camera->y - camera->viewport_height / (2.0f * camera->zoom)) / world->tile_size) - 1;
    int end_y = (int)((camera->y + camera->viewport_height / (2.0f * camera->zoom)) / world->tile_size) + 1;
    
    // Clamp to world bounds
    if (start_x < 0) start_x = 0;
    if (end_x >= world->world_width) end_x = world->world_width - 1;
    if (start_y < 0) start_y = 0;
    if (end_y >= world->world_height) end_y = world->world_height - 1;
    
    // Calculate chunk range
    int start_chunk_x = start_x / CHUNK_SIZE;
    int end_chunk_x = end_x / CHUNK_SIZE;
    int start_chunk_y = start_y / CHUNK_SIZE;
    int end_chunk_y = end_y / CHUNK_SIZE;
    
    // Render visible chunks
    for (int cy = start_chunk_y; cy <= end_chunk_y; cy++) {
        for (int cx = start_chunk_x; cx <= end_chunk_x; cx++) {
            Chunk* chunk = world_get_chunk((WorldState*)world, cx, cy);
            if (!chunk) continue;
            
            // Render tiles in this chunk
            int tile_start_x = cx * CHUNK_SIZE;
            int tile_start_y = cy * CHUNK_SIZE;
            
            for (int local_y = 0; local_y < CHUNK_SIZE; local_y++) {
                for (int local_x = 0; local_x < CHUNK_SIZE; local_x++) {
                    int world_x = tile_start_x + local_x;
                    int world_y = tile_start_y + local_y;
                    
                    // Skip if outside visible range or world bounds
                    if (world_x < start_x || world_x > end_x ||
                        world_y < start_y || world_y > end_y ||
                        world_x >= world->world_width || world_y >= world->world_height) {
                        continue;
                    }
                    
                    // Check for modifications first
                    uint16_t tile = chunk->tiles[local_y * CHUNK_SIZE + local_x];
                    for (int i = 0; i < world->modification_count; i++) {
                        if (world->modifications[i].tile_x == world_x &&
                            world->modifications[i].tile_y == world_y) {
                            tile = world->modifications[i].modified_tile;
                            break;
                        }
                    }
                    
                    // Skip empty tiles
                    if (tile == 0) continue;
                    
                    unsigned int texture;
                    switch (tile) {
                        case 1:  texture = textures->grass; break;
                        case 2:  texture = textures->water; break;
                        default: texture = textures->rock;  break;
                    }
                    
                    renderer_draw_sprite(
                        world_x * world->tile_size,
                        world_y * world->tile_size,
                        world->tile_size,
                        world->tile_size,
                        texture
                    );
                }
            }
        }
    }
}

// ============================================================================
// DECORATION RENDERING
// ============================================================================

void world_render_decorations(const WorldState* world, const Camera* camera, const GameTextures* textures) {
    // Calculate visible tile range
    int start_x = (int)((camera->x - camera->viewport_width / (2.0f * camera->zoom)) / world->tile_size) - 1;
    int end_x = (int)((camera->x + camera->viewport_width / (2.0f * camera->zoom)) / world->tile_size) + 1;
    int start_y = (int)((camera->y - camera->viewport_height / (2.0f * camera->zoom)) / world->tile_size) - 1;
    int end_y = (int)((camera->y + camera->viewport_height / (2.0f * camera->zoom)) / world->tile_size) + 1;
    
    // Clamp to world bounds
    if (start_x < 0) start_x = 0;
    if (end_x >= world->world_width) end_x = world->world_width - 1;
    if (start_y < 0) start_y = 0;
    if (end_y >= world->world_height) end_y = world->world_height - 1;
    
    // Calculate chunk range
    int start_chunk_x = start_x / CHUNK_SIZE;
    int end_chunk_x = end_x / CHUNK_SIZE;
    int start_chunk_y = start_y / CHUNK_SIZE;
    int end_chunk_y = end_y / CHUNK_SIZE;
    
    // Render decorations from visible chunks
    for (int cy = start_chunk_y; cy <= end_chunk_y; cy++) {
        for (int cx = start_chunk_x; cx <= end_chunk_x; cx++) {
            Chunk* chunk = world_get_chunk((WorldState*)world, cx, cy);
            if (!chunk) continue;
            
            int tile_start_x = cx * CHUNK_SIZE;
            int tile_start_y = cy * CHUNK_SIZE;
            
            // Render each decoration in this chunk
            for (int i = 0; i < chunk->decoration_count; i++) {
                Decoration* deco = &chunk->decorations[i];
                if (deco->decoration_id == 0) continue;
                
                // Calculate world position (decorations use chunk-relative coords)
                float world_x = (tile_start_x + deco->offset_x) * world->tile_size;
                float world_y = (tile_start_y + deco->offset_y) * world->tile_size;
                
                // Select texture (use placeholders until you add tree1/shrub1)
                unsigned int texture = 0;
                switch (deco->decoration_id) {
                    case 1:  texture = textures->tree1;  break;
                    case 2:  texture = textures->shrub1; break;
                    default: continue;
                }
                
                if (texture == 0) continue;
                
                // Render decoration
                renderer_draw_sprite(
                    world_x,
                    world_y,
                    world->tile_size,
                    world->tile_size,
                    texture
                );
            }
        }
    }
}