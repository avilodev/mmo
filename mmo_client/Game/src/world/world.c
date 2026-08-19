/**
 * @file
 * Stream, modify, query, and render the client's chunked tile world.
 */

#include "game_types.h"
#include "renderer.h"
#include "texture/texture.h"
#include "world/world_overview.h"
#include <GLFW/glfw3.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/**
 * Open a world file, read its layer metadata, and load tileset textures.
 *
 * The open file and textures remain owned by world until world_cleanup.
 *
 * @param world_file_path  Path to the binary world file in native file encoding.
 * @param tile_size  Runtime tile size in pixels.
 * @return      Nonzero on initialization success; otherwise zero.
 */
int world_init(WorldState* world, const char* world_file_path, int tile_size) {
    memset(world, 0, sizeof(WorldState));

    world->world_file = fopen(world_file_path, "rb");
    if (!world->world_file) {
        fprintf(stderr, "[WORLD] Failed to open %s\n", world_file_path);
        return 0;
    }

    // Read header: width, height, tile_size
    fread(&world->world_width,  sizeof(int), 1, world->world_file);
    fread(&world->world_height, sizeof(int), 1, world->world_file);
    int file_tile_size;
    fread(&file_tile_size, sizeof(int), 1, world->world_file);
    (void)file_tile_size;

    world->tile_size           = tile_size;
    world->world_width_chunks  = (world->world_width  + CHUNK_SIZE - 1) / CHUNK_SIZE;
    world->world_height_chunks = (world->world_height + CHUNK_SIZE - 1) / CHUNK_SIZE;

    // Read tileset table
    uint8_t ts_count = 0;
    fread(&ts_count, sizeof(uint8_t), 1, world->world_file);
    world->tileset_count = ts_count;

    // A world that declares no tilesets stores palette indices directly and
    // renders as flat colour instead of sampling a texture atlas.
    world->flat_color_mode = (ts_count == 0);

    for (int i = 0; i < ts_count && i < MAX_TILESETS - 1; i++) {
        int slot = i + 1;  // slot 0 is reserved for "empty"

        uint8_t path_len = 0;
        fread(&path_len, sizeof(uint8_t), 1, world->world_file);

        char path[128] = {0};
        if (path_len > 0 && path_len < 128) {
            fread(path, 1, path_len, world->world_file);
        } else {
            fseek(world->world_file, path_len, SEEK_CUR);
        }
        path[path_len] = '\0';
        memcpy(world->tilesets[slot].path, path, path_len);
        world->tilesets[slot].path[path_len] = '\0';

        uint16_t cols = 1, rows = 1;
        fread(&cols, sizeof(uint16_t), 1, world->world_file);
        fread(&rows, sizeof(uint16_t), 1, world->world_file);
        world->tilesets[slot].cols = cols;
        world->tilesets[slot].rows = rows;

        world->tileset_textures[slot] = texture_load(path);
        if (!world->tileset_textures[slot])
            fprintf(stderr, "[WORLD] Warning: failed to load tileset %s\n", path);
        else
            printf("[WORLD] Tileset [%d] loaded: %s (%dx%d tiles)\n", slot, path, cols, rows);
    }

    // Record file offsets for the five data layers
    world->base_layer_offset      = ftell(world->world_file);
    long total_tiles              = (long)world->world_width * world->world_height;
    world->overlay_floor_offset    = world->base_layer_offset       + total_tiles * (long)sizeof(uint16_t);
    world->overlay_interior_offset = world->overlay_floor_offset    + total_tiles * (long)sizeof(uint16_t);
    world->overlay_above_offset    = world->overlay_interior_offset + total_tiles * (long)sizeof(uint16_t);
    world->collision_offset        = world->overlay_above_offset    + total_tiles * (long)sizeof(uint16_t);

    for (int i = 0; i < MAX_LOADED_CHUNKS; i++)
        world->chunks[i].is_loaded = 0;

    // Companion overview for the full map screen; absence is not fatal.
    world_overview_load(world_file_path);

    printf("[WORLD] Initialized %dx%d tiles (%dx%d chunks), %d tilesets%s\n",
           world->world_width, world->world_height,
           world->world_width_chunks, world->world_height_chunks,
           world->tileset_count,
           world->flat_color_mode ? " [flat colour]" : "");
    return 1;
}

/**
 * Ensure chunks around a player position are resident.
 */
void world_update_chunks(WorldState* world, float player_x, float player_y) {
    if (!world->world_file || world->tile_size == 0) return;
    world->current_frame++;

    int player_chunk_x = (int)(player_x / world->tile_size) / CHUNK_SIZE;
    int player_chunk_y = (int)(player_y / world->tile_size) / CHUNK_SIZE;

    for (int cy = player_chunk_y - LOAD_RADIUS_CHUNKS;
         cy <= player_chunk_y + LOAD_RADIUS_CHUNKS; cy++) {
        for (int cx = player_chunk_x - LOAD_RADIUS_CHUNKS;
             cx <= player_chunk_x + LOAD_RADIUS_CHUNKS; cx++) {
            world_get_chunk(world, cx, cy);
        }
    }
}

/**
 * Expire temporary tile modifications.
 *
 * @param delta_time  Elapsed frame time in seconds.
 */
void world_update_modifications(WorldState* world, float delta_time) {
    for (int i = 0; i < world->modification_count; i++) {
        if (world->modifications[i].duration > 0) {
            world->modifications[i].time_remaining -= delta_time;
            if (world->modifications[i].time_remaining <= 0) {
                world->modifications[i] = world->modifications[world->modification_count - 1];
                world->modification_count--;
                i--;
            }
        }
    }
}

static void chunk_free_display_lists(Chunk* c) {
    if (c->dl_base)             { glDeleteLists(c->dl_base,             1); c->dl_base             = 0; }
    if (c->dl_overlay_floor)    { glDeleteLists(c->dl_overlay_floor,    1); c->dl_overlay_floor    = 0; }
    if (c->dl_overlay_interior) { glDeleteLists(c->dl_overlay_interior, 1); c->dl_overlay_interior = 0; }
    if (c->dl_overlay_above)    { glDeleteLists(c->dl_overlay_above,    1); c->dl_overlay_above    = 0; }
    c->dl_dirty = 1;
}

/**
 * Close the world file and release chunk display lists and tileset textures.
 *
 * A current OpenGL context must exist while display lists and textures are released.
 */
void world_cleanup(WorldState* world) {
    if (world->world_file) {
        fclose(world->world_file);
        world->world_file = NULL;
    }
    for (int i = 0; i < MAX_LOADED_CHUNKS; i++)
        if (world->chunks[i].is_loaded)
            chunk_free_display_lists(&world->chunks[i]);
    for (int i = 1; i < MAX_TILESETS; i++) {
        if (world->tileset_textures[i]) {
            texture_unload(world->tileset_textures[i]);
            world->tileset_textures[i] = 0;
        }
    }
    world_overview_unload();
    world->loaded_chunk_count = 0;
    world->modification_count = 0;
    printf("[WORLD] Cleaned up\n");
}

/**
 * Find or load a chunk and refresh its recency marker.
 *
 * @return      Resident chunk, or NULL for out-of-range coordinates or load failure.
 */
Chunk* world_get_chunk(WorldState* world, int chunk_x, int chunk_y) {
    if (chunk_x < 0 || chunk_x >= world->world_width_chunks ||
        chunk_y < 0 || chunk_y >= world->world_height_chunks)
        return NULL;

    for (int i = 0; i < MAX_LOADED_CHUNKS; i++) {
        if (world->chunks[i].is_loaded &&
            world->chunks[i].chunk_x == chunk_x &&
            world->chunks[i].chunk_y == chunk_y) {
            world->chunks[i].last_access_frame = world->current_frame;
            return &world->chunks[i];
        }
    }
    return world_load_chunk(world, chunk_x, chunk_y);
}

/**
 * Read one chunk-shaped region from a world layer.
 *
 * @param layer_offset  Byte offset of the layer in the open world file.
 * @param dst  Destination with capacity for CHUNK_SIZE squared tiles.
 * @param fill_val  Value assigned outside world bounds.
 */
static void load_layer_rows(WorldState* world, long layer_offset, int tile_start_x, int tile_start_y,
                             uint16_t* dst, uint16_t fill_val) {
    for (int ly = 0; ly < CHUNK_SIZE; ly++) {
        int wy = tile_start_y + ly;
        if (wy >= world->world_height) {
            for (int lx = 0; lx < CHUNK_SIZE; lx++)
                dst[ly * CHUNK_SIZE + lx] = fill_val;
            continue;
        }
        size_t offset = (size_t)layer_offset +
                        ((size_t)wy * world->world_width + tile_start_x) * sizeof(uint16_t);
        fseek(world->world_file, (long)offset, SEEK_SET);

        int n = CHUNK_SIZE;
        if (tile_start_x + CHUNK_SIZE > world->world_width)
            n = world->world_width - tile_start_x;
        fread(&dst[ly * CHUNK_SIZE], sizeof(uint16_t), n, world->world_file);
        for (int lx = n; lx < CHUNK_SIZE; lx++)
            dst[ly * CHUNK_SIZE + lx] = fill_val;
    }
}

/**
 * Load a chunk into an unused or least-recently-used cache slot.
 *
 * @return      Loaded cache entry, or NULL when no file or slot is available.
 */
Chunk* world_load_chunk(WorldState* world, int chunk_x, int chunk_y) {
    if (!world->world_file) return NULL;

    Chunk* target = NULL;
    if (world->loaded_chunk_count < MAX_LOADED_CHUNKS) {
        for (int i = 0; i < MAX_LOADED_CHUNKS; i++) {
            if (!world->chunks[i].is_loaded) {
                target = &world->chunks[i];
                world->loaded_chunk_count++;
                break;
            }
        }
    } else {
        int oldest = world->current_frame;
        for (int i = 0; i < MAX_LOADED_CHUNKS; i++) {
            if (world->chunks[i].last_access_frame < oldest) {
                oldest = world->chunks[i].last_access_frame;
                target = &world->chunks[i];
            }
        }
        if (target) chunk_free_display_lists(target);  // evict old display lists
    }
    if (!target) {
        fprintf(stderr, "[WORLD] No chunk slot available!\n");
        return NULL;
    }

    target->chunk_x           = chunk_x;
    target->chunk_y           = chunk_y;
    target->is_loaded         = 1;
    target->last_access_frame = world->current_frame;
    target->dl_base             = 0;
    target->dl_overlay_floor    = 0;
    target->dl_overlay_interior = 0;
    target->dl_overlay_above    = 0;
    target->dl_dirty            = 1;

    int tx0 = chunk_x * CHUNK_SIZE;
    int ty0 = chunk_y * CHUNK_SIZE;

    // Base layer
    load_layer_rows(world, world->base_layer_offset, tx0, ty0, target->tiles, TILE_EMPTY);

    // Overlay floor (always visible)
    load_layer_rows(world, world->overlay_floor_offset,    tx0, ty0, target->overlay_floor_tiles,    TILE_EMPTY);

    // Overlay interior (only visible when inside)
    load_layer_rows(world, world->overlay_interior_offset, tx0, ty0, target->overlay_interior_tiles, TILE_EMPTY);

    // Overlay above (wall, roof — in front of player when inside)
    load_layer_rows(world, world->overlay_above_offset,    tx0, ty0, target->overlay_above_tiles,    TILE_EMPTY);

    // Collision layer
    for (int ly = 0; ly < CHUNK_SIZE; ly++) {
        int wy = ty0 + ly;
        if (wy >= world->world_height) {
            memset(&target->collision[ly * CHUNK_SIZE], 1, CHUNK_SIZE);
            continue;
        }
        size_t offset = (size_t)world->collision_offset +
                        ((size_t)wy * world->world_width + tx0) * sizeof(uint8_t);
        fseek(world->world_file, (long)offset, SEEK_SET);
        int n = CHUNK_SIZE;
        if (tx0 + CHUNK_SIZE > world->world_width)
            n = world->world_width - tx0;
        fread(&target->collision[ly * CHUNK_SIZE], sizeof(uint8_t), n, world->world_file);
        memset(&target->collision[ly * CHUNK_SIZE + n], 1, CHUNK_SIZE - n);
    }

    return target;
}

/**
 * Read the effective base tile at world tile coordinates.
 *
 * @return      Packed tile value, or -1 outside the world or on chunk-load failure.
 */
int world_get_tile(const WorldState* world, int tx, int ty) {
    if (tx < 0 || tx >= world->world_width || ty < 0 || ty >= world->world_height)
        return -1;

    for (int i = 0; i < world->modification_count; i++) {
        if (world->modifications[i].tile_x == tx &&
            world->modifications[i].tile_y == ty)
            return world->modifications[i].modified_tile;
    }

    Chunk* chunk = world_get_chunk((WorldState*)world, tx / CHUNK_SIZE, ty / CHUNK_SIZE);
    if (!chunk) return -1;
    return chunk->tiles[(ty % CHUNK_SIZE) * CHUNK_SIZE + (tx % CHUNK_SIZE)];
}

/**
 * Replace a resident base tile when its coordinates are valid.
 */
void world_set_tile(WorldState* world, int tx, int ty, int tile_type) {
    if (tx < 0 || tx >= world->world_width || ty < 0 || ty >= world->world_height)
        return;
    Chunk* chunk = world_get_chunk(world, tx / CHUNK_SIZE, ty / CHUNK_SIZE);
    if (!chunk) return;
    chunk->tiles[(ty % CHUNK_SIZE) * CHUNK_SIZE + (tx % CHUNK_SIZE)] = (uint16_t)tile_type;
}

/**
 * Check collision at a world-space point.
 *
 * @return      Nonzero for blocked, out-of-range, or unavailable tiles; otherwise zero.
 */
int world_check_tile_collision(const WorldState* world, float x, float y) {
    int tx = (int)(x / world->tile_size);
    int ty = (int)(y / world->tile_size);
    if (tx < 0 || tx >= world->world_width || ty < 0 || ty >= world->world_height)
        return 1;

    for (int i = 0; i < world->modification_count; i++) {
        if (world->modifications[i].tile_x == tx &&
            world->modifications[i].tile_y == ty)
            return world->modifications[i].modified_collision;
    }

    Chunk* chunk = world_get_chunk((WorldState*)world, tx / CHUNK_SIZE, ty / CHUNK_SIZE);
    if (!chunk) return 1;
    return chunk->collision[(ty % CHUNK_SIZE) * CHUNK_SIZE + (tx % CHUNK_SIZE)];
}

/**
 * Check collision at the four corners of an axis-aligned box.
 *
 * @return      Nonzero when any corner is blocked; otherwise zero.
 */
int world_check_box_collision(const WorldState* world, float x, float y, float half_size) {
    if (world_check_tile_collision(world, x - half_size, y - half_size)) return 1;
    if (world_check_tile_collision(world, x + half_size, y - half_size)) return 1;
    if (world_check_tile_collision(world, x - half_size, y + half_size)) return 1;
    if (world_check_tile_collision(world, x + half_size, y + half_size)) return 1;
    return 0;
}

/**
 * Convert a world position to containing tile coordinates.
 */
void world_to_tile(const WorldState* world, float wx, float wy, int* tx, int* ty) {
    *tx = (int)(wx / world->tile_size);
    *ty = (int)(wy / world->tile_size);
}

/**
 * Convert tile coordinates to the tile center in world space.
 */
void tile_to_world(const WorldState* world, int tx, int ty, float* wx, float* wy) {
    *wx = tx * world->tile_size + world->tile_size / 2.0f;
    *wy = ty * world->tile_size + world->tile_size / 2.0f;
}

// Mark the chunk containing (tile_x, tile_y) as needing a display list rebuild.
static void mark_chunk_dirty(WorldState* world, int tile_x, int tile_y) {
    int cx = tile_x / CHUNK_SIZE;
    int cy = tile_y / CHUNK_SIZE;
    for (int i = 0; i < MAX_LOADED_CHUNKS; i++) {
        if (world->chunks[i].is_loaded &&
            world->chunks[i].chunk_x == cx &&
            world->chunks[i].chunk_y == cy) {
            world->chunks[i].dl_dirty = 1;
            break;
        }
    }
}

/**
 * Add or replace a dynamic tile and collision override.
 *
 * @param duration  Lifetime in seconds, or a non-positive value for no expiry.
 * @return      Nonzero when stored; otherwise zero when capacity is exhausted.
 */
int world_add_modification(WorldState* world, int tile_x, int tile_y,
                            uint16_t tile_type, uint8_t collision, float duration) {
    if (world->modification_count >= MAX_MODIFICATIONS) {
        fprintf(stderr, "[WORLD] Max modifications reached!\n");
        return 0;
    }
    for (int i = 0; i < world->modification_count; i++) {
        if (world->modifications[i].tile_x == tile_x &&
            world->modifications[i].tile_y == tile_y) {
            world->modifications[i].modified_tile      = tile_type;
            world->modifications[i].modified_collision = collision;
            world->modifications[i].duration           = duration;
            world->modifications[i].time_remaining     = duration;
            mark_chunk_dirty(world, tile_x, tile_y);
            return 1;
        }
    }
    TileModification* m = &world->modifications[world->modification_count++];
    m->tile_x             = tile_x;
    m->tile_y             = tile_y;
    m->modified_tile      = tile_type;
    m->modified_collision = collision;
    m->duration           = duration;
    m->time_remaining     = duration;
    mark_chunk_dirty(world, tile_x, tile_y);
    return 1;
}

/**
 * Remove a dynamic override at tile coordinates.
 */
void world_remove_modification(WorldState* world, int tile_x, int tile_y) {
    for (int i = 0; i < world->modification_count; i++) {
        if (world->modifications[i].tile_x == tile_x &&
            world->modifications[i].tile_y == tile_y) {
            world->modifications[i] = world->modifications[--world->modification_count];
            mark_chunk_dirty(world, tile_x, tile_y);
            return;
        }
    }
}

/**
 * Clear every dynamic tile override and invalidate resident display lists.
 */
void world_clear_modifications(WorldState* world) {
    // Mark all loaded chunks dirty since any could have had modifications
    for (int i = 0; i < MAX_LOADED_CHUNKS; i++)
        if (world->chunks[i].is_loaded)
            world->chunks[i].dl_dirty = 1;
    world->modification_count = 0;
}

/**
 * Compute the inclusive chunk range intersecting the camera viewport.
 */
static void visible_chunk_range(const WorldState* world, const Camera* camera,
                                 int* sc_x, int* ec_x, int* sc_y, int* ec_y) {
    float half_w = camera->viewport_width  / (2.0f * camera->zoom);
    float half_h = camera->viewport_height / (2.0f * camera->zoom);

    int sx = (int)((camera->x - half_w) / world->tile_size) - 1;
    int ex = (int)((camera->x + half_w) / world->tile_size) + 1;
    int sy = (int)((camera->y - half_h) / world->tile_size) - 1;
    int ey = (int)((camera->y + half_h) / world->tile_size) + 1;

    if (sx < 0) sx = 0;
    if (sy < 0) sy = 0;
    if (ex >= world->world_width)  ex = world->world_width  - 1;
    if (ey >= world->world_height) ey = world->world_height - 1;

    *sc_x = sx / CHUNK_SIZE;
    *ec_x = ex / CHUNK_SIZE;
    *sc_y = sy / CHUNK_SIZE;
    *ec_y = ey / CHUNK_SIZE;
}

/**
 * Compile one chunk layer into its OpenGL display list.
 *
 * @param layer  Layer index: zero base, one floor, two interior, or three above.
 */
static void chunk_build_display_list(WorldState* world, Chunk* chunk, int layer) {
    unsigned int* dl_id = (layer == 0) ? &chunk->dl_base
                        : (layer == 1) ? &chunk->dl_overlay_floor
                        : (layer == 2) ? &chunk->dl_overlay_interior
                                       : &chunk->dl_overlay_above;

    if (*dl_id == 0)
        *dl_id = glGenLists(1);

    int tx0 = chunk->chunk_x * CHUNK_SIZE;
    int ty0 = chunk->chunk_y * CHUNK_SIZE;

    glNewList(*dl_id, GL_COMPILE);
    glEnable(GL_TEXTURE_2D);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

    unsigned int bound_tex = 0;

    for (int ly = 0; ly < CHUNK_SIZE; ly++) {
        for (int lx = 0; lx < CHUNK_SIZE; lx++) {
            int wx = tx0 + lx;
            int wy = ty0 + ly;
            if (wx >= world->world_width || wy >= world->world_height) continue;

            int slot = ly * CHUNK_SIZE + lx;
            uint16_t packed = (layer == 0) ? chunk->tiles[slot]
                            : (layer == 1) ? chunk->overlay_floor_tiles[slot]
                            : (layer == 2) ? chunk->overlay_interior_tiles[slot]
                                           : chunk->overlay_above_tiles[slot];

            // Check dynamic modifications (bake them into the compiled list)
            if (layer == 0) {
                for (int m = 0; m < world->modification_count; m++) {
                    if (world->modifications[m].tile_x == wx &&
                        world->modifications[m].tile_y == wy) {
                        packed = world->modifications[m].modified_tile;
                        break;
                    }
                }
            }

            if (packed == TILE_EMPTY) continue;

            float dx = (float)(wx * world->tile_size);
            float dy = (float)(wy * world->tile_size);
            float ds = (float)world->tile_size;

            if (world->flat_color_mode) {
                const float* rgb = tile_palette_rgb(packed);
                if (!rgb) continue;

                glDisable(GL_TEXTURE_2D);
                glColor3f(rgb[0], rgb[1], rgb[2]);
                glBegin(GL_QUADS);
                    glVertex2f(dx,      dy);
                    glVertex2f(dx + ds, dy);
                    glVertex2f(dx + ds, dy + ds);
                    glVertex2f(dx,      dy + ds);
                glEnd();
                continue;
            }

            int ts_id  = (packed >> 12) & 0xF;
            int ts_idx =  packed        & 0xFFF;

            unsigned int tex = world->tileset_textures[ts_id];
            if (!tex) continue;

            int cols = world->tilesets[ts_id].cols;
            int rows = world->tilesets[ts_id].rows;
            if (cols == 0 || rows == 0) continue;

            int src_col = ts_idx % cols;
            int src_row = ts_idx / cols;

            float u0 = (float) src_col      / cols;
            float v0 = (float) src_row      / rows;
            float u1 = (float)(src_col + 1) / cols;
            float v1 = (float)(src_row + 1) / rows;

            if (tex != bound_tex) {
                glBindTexture(GL_TEXTURE_2D, tex);
                bound_tex = tex;
            }

            glEnable(GL_TEXTURE_2D);
            glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
            glBegin(GL_QUADS);
                glTexCoord2f(u0, v0); glVertex2f(dx,      dy);
                glTexCoord2f(u1, v0); glVertex2f(dx + ds, dy);
                glTexCoord2f(u1, v1); glVertex2f(dx + ds, dy + ds);
                glTexCoord2f(u0, v1); glVertex2f(dx,      dy + ds);
            glEnd();
        }
    }

    glEndList();
}

/**
 * Render visible chunks for one cached tile layer.
 */
static void render_tile_layer(const WorldState* world, const Camera* camera, int layer) {
    int sc_x, ec_x, sc_y, ec_y;
    visible_chunk_range(world, camera, &sc_x, &ec_x, &sc_y, &ec_y);

    for (int cy = sc_y; cy <= ec_y; cy++) {
        for (int cx = sc_x; cx <= ec_x; cx++) {
            Chunk* chunk = world_get_chunk((WorldState*)world, cx, cy);
            if (!chunk) continue;

            unsigned int dl = (layer == 0) ? chunk->dl_base
                            : (layer == 1) ? chunk->dl_overlay_floor
                            : (layer == 2) ? chunk->dl_overlay_interior
                                           : chunk->dl_overlay_above;

            // Build or rebuild all four lists together when dirty
            if (chunk->dl_dirty || dl == 0) {
                chunk_build_display_list((WorldState*)world, chunk, 0);
                chunk_build_display_list((WorldState*)world, chunk, 1);
                chunk_build_display_list((WorldState*)world, chunk, 2);
                chunk_build_display_list((WorldState*)world, chunk, 3);
                chunk->dl_dirty = 0;
                dl = (layer == 0) ? chunk->dl_base
                   : (layer == 1) ? chunk->dl_overlay_floor
                   : (layer == 2) ? chunk->dl_overlay_interior
                                  : chunk->dl_overlay_above;
            }

            if (dl) glCallList(dl);
        }
    }
}

/**
 * Render the visible base tile layer.
 */
void world_render(const WorldState* world, const Camera* camera) {
    render_tile_layer(world, camera, 0);
}

/**
 * Render the visible floor overlay layer.
 */
void world_render_overlay_floor(const WorldState* world, const Camera* camera) {
    render_tile_layer(world, camera, 1);
}

/**
 * Render the visible interior overlay layer.
 */
void world_render_overlay_interior(const WorldState* world, const Camera* camera) {
    render_tile_layer(world, camera, 2);
}

/**
 * Render all visible above-player overlay tiles from display lists.
 */
void world_render_overlay_above(const WorldState* world, const Camera* camera) {
    render_tile_layer(world, camera, 3);
}

/**
 * Render one Y-partition of the above-player overlay without display lists.
 *
 * @param player_ty  Player tile row used as the partition boundary.
 * @param north_half  Nonzero for rows north of the boundary; zero for remaining rows.
 */
static void render_overlay_above_half(const WorldState* world, const Camera* camera,
                                       int player_ty, int north_half) {
    int sc_x, ec_x, sc_y, ec_y;
    visible_chunk_range(world, camera, &sc_x, &ec_x, &sc_y, &ec_y);

    glEnable(GL_TEXTURE_2D);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    unsigned int bound_tex = 0;
    int in_begin = 0;

    for (int cy = sc_y; cy <= ec_y; cy++) {
        for (int cx = sc_x; cx <= ec_x; cx++) {
            Chunk* chunk = world_get_chunk((WorldState*)world, cx, cy);
            if (!chunk) continue;

            int tx0 = cx * CHUNK_SIZE;
            int ty0 = cy * CHUNK_SIZE;

            for (int ly = 0; ly < CHUNK_SIZE; ly++) {
                int wy = ty0 + ly;
                if (wy >= world->world_height) continue;
                if ( north_half && wy >= player_ty) continue;
                if (!north_half && wy <  player_ty) continue;

                for (int lx = 0; lx < CHUNK_SIZE; lx++) {
                    int wx = tx0 + lx;
                    if (wx >= world->world_width) continue;

                    uint16_t packed = chunk->overlay_above_tiles[ly * CHUNK_SIZE + lx];
                    if (packed == TILE_EMPTY) continue;

                    if (world->flat_color_mode) {
                        const float* rgb = tile_palette_rgb(packed);
                        if (!rgb) continue;

                        if (in_begin) { glEnd(); in_begin = 0; }

                        float fdx = (float)(wx * world->tile_size);
                        float fdy = (float)(wy * world->tile_size);
                        float fds = (float)world->tile_size;

                        glDisable(GL_TEXTURE_2D);
                        glColor3f(rgb[0], rgb[1], rgb[2]);
                        glBegin(GL_QUADS);
                            glVertex2f(fdx,       fdy);
                            glVertex2f(fdx + fds, fdy);
                            glVertex2f(fdx + fds, fdy + fds);
                            glVertex2f(fdx,       fdy + fds);
                        glEnd();
                        continue;
                    }

                    int ts_id  = (packed >> 12) & 0xF;
                    int ts_idx =  packed        & 0xFFF;
                    unsigned int tex = world->tileset_textures[ts_id];
                    if (!tex) continue;

                    int cols = world->tilesets[ts_id].cols;
                    int rows = world->tilesets[ts_id].rows;
                    if (cols == 0 || rows == 0) continue;

                    int src_col = ts_idx % cols;
                    int src_row = ts_idx / cols;

                    float u0 = (float) src_col      / cols;
                    float v0 = (float) src_row      / rows;
                    float u1 = (float)(src_col + 1) / cols;
                    float v1 = (float)(src_row + 1) / rows;

                    float dx = (float)(wx * world->tile_size);
                    float dy = (float)(wy * world->tile_size);
                    float ds = (float)world->tile_size;

                    if (tex != bound_tex) {
                        if (in_begin) { glEnd(); in_begin = 0; }
                        glBindTexture(GL_TEXTURE_2D, tex);
                        bound_tex = tex;
                        glBegin(GL_QUADS);
                        in_begin = 1;
                    }

                    glTexCoord2f(u0, v0); glVertex2f(dx,      dy);
                    glTexCoord2f(u1, v0); glVertex2f(dx + ds, dy);
                    glTexCoord2f(u1, v1); glVertex2f(dx + ds, dy + ds);
                    glTexCoord2f(u0, v1); glVertex2f(dx,      dy + ds);
                }
            }
        }
    }
    if (in_begin) glEnd();
    // The flat-colour branch toggles texturing and colour per tile; restore the
    // defaults so later passes are unaffected.
    glEnable(GL_TEXTURE_2D);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
}

/**
 * Render above-player overlay rows north of the player.
 */
void world_render_overlay_above_north(const WorldState* world, const Camera* camera, int player_ty) {
    render_overlay_above_half(world, camera, player_ty, 1);
}

/**
 * Render above-player overlay rows at or south of the player.
 */
void world_render_overlay_above_south(const WorldState* world, const Camera* camera, int player_ty) {
    render_overlay_above_half(world, camera, player_ty, 0);
}

/**
 * Check whether a world position has an interior floor overlay.
 *
 * @return      Nonzero for an interior tile; otherwise zero.
 */
int world_is_inside(const WorldState* world, float wx, float wy) {
    int tx = (int)(wx / world->tile_size);
    int ty = (int)(wy / world->tile_size);
    if (tx < 0 || tx >= world->world_width || ty < 0 || ty >= world->world_height)
        return 0;
    Chunk* chunk = world_get_chunk((WorldState*)world, tx / CHUNK_SIZE, ty / CHUNK_SIZE);
    if (!chunk) return 0;
    int slot = (ty % CHUNK_SIZE) * CHUNK_SIZE + (tx % CHUNK_SIZE);
    return chunk->overlay_floor_tiles[slot] != TILE_EMPTY;
}
