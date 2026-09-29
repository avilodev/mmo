/**
 * @file
 * Stream, modify, query, and render the client's chunked tile world.
 */

#include "game_types.h"
#include "renderer.h"
#include "texture/texture.h"
#include "world/world_overview.h"
#include "world/chunk_mesh.h"
#include "world/structure_mesh.h"
#include "render/ground_renderer.h"
#include "world_format.h"
#include <GLFW/glfw3.h>
#include <math.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include "core/client_log.h"

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
        CLOG_ERROR("[WORLD] Failed to open %s", world_file_path);
        return 0;
    }

    /* The file says what it is before it says anything about its contents.
     *
     * There was no magic and no version: the header was three int32s and this
     * reader trusted them. A truncated download, a file from a different
     * generator, or a layout the writer had changed all read as a world of some
     * shape, and the game drew whatever came next. */
    char magic[WORLD_FORMAT_MAGIC_LEN];
    if (fread(magic, 1, sizeof(magic), world->world_file) != sizeof(magic) ||
        memcmp(magic, WORLD_FORMAT_MAGIC, sizeof(magic)) != 0) {
        CLOG_WARN("[WORLD] %s is not a world file (bad magic). "
                        "Regenerate it with 'make world'.", world_file_path);
        fclose(world->world_file);
        world->world_file = NULL;
        return 0;
    }

    uint32_t file_version = 0, tile_layers = 0;
    if (fread(&file_version, sizeof(uint32_t), 1, world->world_file) != 1 ||
        fread(&tile_layers,  sizeof(uint32_t), 1, world->world_file) != 1) {
        CLOG_WARN("[WORLD] %s ends inside its header", world_file_path);
        fclose(world->world_file);
        world->world_file = NULL;
        return 0;
    }

    if (file_version != WORLD_FORMAT_VERSION) {
        CLOG_WARN("[WORLD] %s is world format version %u; this build reads "
                        "version %d. Regenerate it with 'make world'.",
                world_file_path, file_version, WORLD_FORMAT_VERSION);
        fclose(world->world_file);
        world->world_file = NULL;
        return 0;
    }

    if (tile_layers != WORLD_FORMAT_TILE_LAYERS) {
        /* This reader keeps one named offset per layer, so unlike the server's
         * collision reader it cannot simply seek past an unknown number of
         * them. Saying so is better than reading the wrong bytes. */
        CLOG_WARN("[WORLD] %s holds %u tile layers; this build draws %d. "
                        "Regenerate it with 'make world'.",
                world_file_path, tile_layers, WORLD_FORMAT_TILE_LAYERS);
        fclose(world->world_file);
        world->world_file = NULL;
        return 0;
    }

    /* Read header: width, height, tile_size.
     *
     * Checked, like the magic and the version above. These three were read
     * without looking at whether the read succeeded, which on a truncated or
     * unreadable file leaves the dimensions holding whatever was in the
     * struct -- and the struct is calloc'd, so that is zero, which the
     * validation below happens to reject. "Happens to" is the problem: the
     * safety came from the allocator rather than from the check, and a future
     * field that is not zero-initialised would not be so lucky. */
    int file_tile_size = 0;
    if (fread(&world->world_width,  sizeof(int), 1, world->world_file) != 1 ||
        fread(&world->world_height, sizeof(int), 1, world->world_file) != 1 ||
        fread(&file_tile_size,      sizeof(int), 1, world->world_file) != 1) {
        CLOG_WARN("[WORLD] %s ends inside its header", world_file_path);
        fclose(world->world_file);
        world->world_file = NULL;
        return 0;
    }
    (void)file_tile_size;

    /* The dimensions size every per-tile allocation below, so both the axes
     * and their product are bounded. The per-axis limit alone lets a header
     * declare two individually plausible numbers that multiply into a
     * request no machine will satisfy. */
    if (world->world_width <= 0 || world->world_height <= 0 ||
        world->world_width  > WORLD_FORMAT_MAX_DIMENSION ||
        world->world_height > WORLD_FORMAT_MAX_DIMENSION ||
        (uint64_t)world->world_width * (uint64_t)world->world_height > WORLD_FORMAT_MAX_TILES) {
        CLOG_WARN("[WORLD] %s declares %dx%d tiles, which is not usable",
                world_file_path, world->world_width, world->world_height);
        fclose(world->world_file);
        world->world_file = NULL;
        return 0;
    }

    world->tile_size           = tile_size;
    world->world_width_chunks  = (world->world_width  + CHUNK_SIZE - 1) / CHUNK_SIZE;
    world->world_height_chunks = (world->world_height + CHUNK_SIZE - 1) / CHUNK_SIZE;

    // Read tileset table
    uint8_t ts_count = 0;
    if (fread(&ts_count, sizeof(uint8_t), 1, world->world_file) != 1) {
        CLOG_WARN("[WORLD] %s ends before its tileset table", world_file_path);
        fclose(world->world_file);
        world->world_file = NULL;
        return 0;
    }
    world->tileset_count = ts_count;

    // A world that declares no tilesets stores palette indices directly and
    // renders as flat colour instead of sampling a texture atlas.
    world->flat_color_mode = (ts_count == 0);

    for (int i = 0; i < ts_count && i < MAX_TILESETS - 1; i++) {
        int slot = i + 1;  // slot 0 is reserved for "empty"

        uint8_t path_len = 0;
        if (fread(&path_len, sizeof(uint8_t), 1, world->world_file) != 1) {
            CLOG_WARN("[WORLD] %s ends inside tileset entry %d", world_file_path, i);
            fclose(world->world_file);
            world->world_file = NULL;
            return 0;
        }

        char path[128] = {0};
        if (path_len > 0 && path_len < 128) {
            if (fread(path, 1, path_len, world->world_file) != path_len) {
                CLOG_WARN("[WORLD] %s ends inside tileset %d's path",
                        world_file_path, i);
                fclose(world->world_file);
                world->world_file = NULL;
                return 0;
            }
        } else {
            fseek(world->world_file, path_len, SEEK_CUR);
        }
        path[path_len] = '\0';
        memcpy(world->tilesets[slot].path, path, path_len);
        world->tilesets[slot].path[path_len] = '\0';

        uint16_t cols = 1, rows = 1;
        if (fread(&cols, sizeof(uint16_t), 1, world->world_file) != 1 ||
            fread(&rows, sizeof(uint16_t), 1, world->world_file) != 1) {
            CLOG_WARN("[WORLD] %s ends inside tileset %d's dimensions",
                    world_file_path, i);
            fclose(world->world_file);
            world->world_file = NULL;
            return 0;
        }
        world->tilesets[slot].cols = cols;
        world->tilesets[slot].rows = rows;

        TilesetLayout layout = { 0, 0, 0 };
        world->tileset_textures[slot] = texture_load_tileset(path, cols, rows, &layout);
        world->tilesets[slot].tile_w = layout.tile_w;
        world->tilesets[slot].tile_h = layout.tile_h;
        world->tilesets[slot].pad    = layout.pad;
        if (!world->tileset_textures[slot])
            CLOG_ERROR("[WORLD] Warning: failed to load tileset %s", path);
        else
            CLOG_INFO("[WORLD] Tileset [%d] loaded: %s (%dx%d tiles)", slot, path, cols, rows);
    }

    /* Record file offsets for the layers the 3D view reads. The layers are
     * stored base, floor, interior, above, then collision; floor and interior
     * drew the flat top-down roofs and are skipped. */
    world->base_layer_offset    = ftell(world->world_file);
    long layer_bytes            = (long)world->world_width * world->world_height * (long)sizeof(uint16_t);
    world->overlay_above_offset = world->base_layer_offset + 3 * layer_bytes;
    world->collision_offset     = world->base_layer_offset + 4 * layer_bytes;

    for (int i = 0; i < MAX_LOADED_CHUNKS; i++)
        world->chunks[i].is_loaded = 0;

    // Companion overview for the full map screen; absence is not fatal.
    world_overview_load(world_file_path);

    CLOG_INFO("[WORLD] Initialized %dx%d tiles (%dx%d chunks), %d tilesets%s",
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
    world->center_chunk_x = player_chunk_x;
    world->center_chunk_y = player_chunk_y;
    world->has_center     = 1;

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

static void chunk_free_gpu(Chunk* c) {
    ground_renderer_release(&c->ground);
    ground_renderer_release(&c->structures);
    c->gpu_dirty = 1;
}

/** Find a resident chunk without loading it. */
static Chunk* peek_chunk(const WorldState* world, int chunk_x, int chunk_y) {
    for (int i = 0; i < MAX_LOADED_CHUNKS; i++) {
        const Chunk* c = &world->chunks[i];
        if (c->is_loaded && c->chunk_x == chunk_x && c->chunk_y == chunk_y)
            return (Chunk*)c;
    }
    return NULL;
}

/**
 * Close the world file and release chunk vertex buffers and tileset textures.
 *
 * A current OpenGL context must exist while buffers and textures are released.
 */
void world_cleanup(WorldState* world) {
    if (world->world_file) {
        fclose(world->world_file);
        world->world_file = NULL;
    }
    for (int i = 0; i < MAX_LOADED_CHUNKS; i++)
        if (world->chunks[i].is_loaded)
            chunk_free_gpu(&world->chunks[i]);
    for (int i = 1; i < MAX_TILESETS; i++) {
        if (world->tileset_textures[i]) {
            texture_unload(world->tileset_textures[i]);
            world->tileset_textures[i] = 0;
        }
    }
    world_overview_unload();
    world->loaded_chunk_count = 0;
    world->modification_count = 0;
    CLOG_INFO("[WORLD] Cleaned up");
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

        /* A short read fills the row rather than leaving it as whatever the
         * cache slot held last. Chunk slots are reused, so an unchecked read
         * near the end of a truncated file draws the previous occupant's
         * tiles at these coordinates -- a piece of another part of the map,
         * with no error anywhere. */
        size_t got = fread(&dst[ly * CHUNK_SIZE], sizeof(uint16_t), (size_t)n,
                           world->world_file);
        for (size_t lx = got; lx < (size_t)CHUNK_SIZE; lx++)
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
        /* The least recently used chunk, even if that was this frame. Only
         * taking chunks from earlier frames dead-ended whenever one frame
         * touched more than MAX_LOADED_CHUNKS -- e.g. a respawn, when chunks are
         * streamed round the new position while the camera, still gliding over,
         * draws round the old one -- and those chunks then did not draw.
         * Evicting one already drawn this frame just means reloading it later. */
        int oldest = INT_MAX;
        for (int i = 0; i < MAX_LOADED_CHUNKS; i++) {
            if (world->chunks[i].last_access_frame < oldest) {
                oldest = world->chunks[i].last_access_frame;
                target = &world->chunks[i];
            }
        }
        if (target) chunk_free_gpu(target);  // evict the old chunk's vertex buffers
    }
    if (!target) {
        CLOG_WARN("[WORLD] No chunk slot available!");
        return NULL;
    }

    target->chunk_x           = chunk_x;
    target->chunk_y           = chunk_y;
    target->is_loaded         = 1;
    target->last_access_frame = world->current_frame;
    /* An evicted slot was released above; a slot never used holds zeroes. */
    target->gpu_dirty         = 1;

    int tx0 = chunk_x * CHUNK_SIZE;
    int ty0 = chunk_y * CHUNK_SIZE;

    // Base layer: the ground
    load_layer_rows(world, world->base_layer_offset, tx0, ty0, target->tiles, TILE_EMPTY);

    // Roof layer: which solid tiles are buildings (structure_mesh.h)
    load_layer_rows(world, world->overlay_above_offset, tx0, ty0, target->overlay_above_tiles, TILE_EMPTY);

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

        /* Whatever was not read is solid. For collision that is the safe
         * direction to fail: a row that could not be loaded becomes wall
         * rather than becoming whatever the last chunk in this slot had
         * there, which could be open ground. */
        size_t got = fread(&target->collision[ly * CHUNK_SIZE], sizeof(uint8_t),
                           (size_t)n, world->world_file);
        memset(&target->collision[ly * CHUNK_SIZE + got], 1, (size_t)CHUNK_SIZE - got);
    }

    /* A building or wall that crosses into this chunk was built by its
     * neighbours without seeing this side of it; rebuild them now that they
     * can (structure_mesh.h reads across chunk borders). */
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            Chunk* n = (dx || dy) ? peek_chunk(world, chunk_x + dx, chunk_y + dy) : NULL;
            if (n) n->gpu_dirty = 1;
        }
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

/* Mark the chunk containing (tile_x, tile_y) as needing its vertex buffers
 * rebuilt, and its neighbours too: their structures read across the border
 * (a roof's slope, a wall's side), so a changed tile can change them. */
static void mark_chunk_dirty(WorldState* world, int tile_x, int tile_y) {
    int cx = tile_x / CHUNK_SIZE;
    int cy = tile_y / CHUNK_SIZE;
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            Chunk* c = peek_chunk(world, cx + dx, cy + dy);
            if (c) c->gpu_dirty = 1;
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
        CLOG_WARN("[WORLD] Max modifications reached!");
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
 * Clear every dynamic tile override and invalidate resident vertex buffers.
 */
void world_clear_modifications(WorldState* world) {
    // Mark all loaded chunks dirty since any could have had modifications
    for (int i = 0; i < MAX_LOADED_CHUNKS; i++)
        if (world->chunks[i].is_loaded)
            world->chunks[i].gpu_dirty = 1;
    world->modification_count = 0;
}

/**
 * Compute the inclusive chunk range holding every ground point on screen.
 *
 * Tilted and turned, what the camera sees is not a screen-aligned rectangle,
 * so this bounds the ground the view actually covers (D14).
 */
static void visible_chunk_range(const WorldState* world, const Camera* camera,
                                 int* sc_x, int* ec_x, int* sc_y, int* ec_y) {
    float x0, y0, x1, y1;
    camera_visible_ground(camera, &x0, &y0, &x1, &y1);

    /* The margin covers what stands: a roof just off the near edge of the
     * ground footprint still rises into the bottom of the screen. */
    const int margin = VISIBLE_STRUCTURE_MARGIN_TILES;
    int sx = (int)floorf(x0 / world->tile_size) - margin;
    int ex = (int)floorf(x1 / world->tile_size) + margin;
    int sy = (int)floorf(y0 / world->tile_size) - margin;
    int ey = (int)floorf(y1 / world->tile_size) + margin;

    if (sx < 0) sx = 0;
    if (sy < 0) sy = 0;
    if (ex >= world->world_width)  ex = world->world_width  - 1;
    if (ey >= world->world_height) ey = world->world_height - 1;

    *sc_x = sx / CHUNK_SIZE;
    *ec_x = ex / CHUNK_SIZE;
    *sc_y = sy / CHUNK_SIZE;
    *ec_y = ey / CHUNK_SIZE;

    /* A low camera sees to the horizon; draw only what is streamed around
     * the player, and let the fog (world_light.h) hide where it ends.
     * Loading every chunk out to the horizon would thrash the cache. */
    if (world->has_center) {
        int r = LOAD_RADIUS_CHUNKS;
        if (*sc_x < world->center_chunk_x - r) *sc_x = world->center_chunk_x - r;
        if (*ec_x > world->center_chunk_x + r) *ec_x = world->center_chunk_x + r;
        if (*sc_y < world->center_chunk_y - r) *sc_y = world->center_chunk_y - r;
        if (*ec_y > world->center_chunk_y + r) *ec_y = world->center_chunk_y + r;
    }
}

/** Classify a resident tile for the structure builder, without loading. */
static StructureTile structure_at(void* ctx, int tx, int ty) {
    const WorldState* world = (const WorldState*)ctx;
    const Chunk* c = peek_chunk(world, tx / CHUNK_SIZE, ty / CHUNK_SIZE);
    if (!c) {
        StructureTile none = { STRUCTURE_NONE, 0 };
        return none;
    }
    int slot = (ty % CHUNK_SIZE) * CHUNK_SIZE + (tx % CHUNK_SIZE);

    uint8_t  collision = c->collision[slot];
    uint16_t base      = c->tiles[slot];
    for (int i = 0; i < world->modification_count; i++) {
        const TileModification* m = &world->modifications[i];
        if (m->tile_x == tx && m->tile_y == ty) {
            collision = m->modified_collision;
            base      = m->modified_tile;
            break;
        }
    }
    return structure_tile_of(base, c->overlay_above_tiles[slot], collision);
}

/**
 * Build and upload a chunk's ground and structures.
 *
 * Live tile modifications are baked into the ground, as the display lists
 * this replaced did.
 */
static void chunk_build_gpu(const WorldState* world, Chunk* chunk) {
    static ChunkVertex scratch[STRUCTURE_MESH_MAX_VERTS > CHUNK_MESH_MAX_VERTS
                               ? STRUCTURE_MESH_MAX_VERTS : CHUNK_MESH_MAX_VERTS];

    ChunkMeshInput in;
    memset(&in, 0, sizeof(in));
    in.chunk_x    = chunk->chunk_x;
    in.chunk_y    = chunk->chunk_y;
    in.world_w    = world->world_width;
    in.world_h    = world->world_height;
    in.tile_size  = world->tile_size;
    in.flat_color = world->flat_color_mode;
    for (int t = 1; t < MAX_TILESETS; t++) {
        in.tilesets[t].present = world->tileset_textures[t] != 0;
        in.tilesets[t].cols    = world->tilesets[t].cols;
        in.tilesets[t].rows    = world->tilesets[t].rows;
        in.tilesets[t].tile_w  = world->tilesets[t].tile_w;
        in.tilesets[t].tile_h  = world->tilesets[t].tile_h;
        in.tilesets[t].pad     = world->tilesets[t].pad;
    }
    in.tiles     = chunk->tiles;
    in.mods      = world->modifications;
    in.mod_count = world->modification_count;

    ChunkMesh mesh;
    mesh.verts = scratch;
    chunk_mesh_build(&mesh, &in);
    ground_renderer_upload(&chunk->ground, &mesh);

    /* Structures are classified by palette index, so only a flat-colour world
     * has them; a textured one would need its own tile-to-structure table. */
    ChunkMesh solid;
    memset(&solid, 0, sizeof(solid));
    solid.verts = scratch;
    if (world->flat_color_mode) {
        StructureMeshInput sin = {
            chunk->chunk_x, chunk->chunk_y, world->world_width, world->world_height,
            world->tile_size, structure_at, (void*)world
        };
        solid.vert_count = structure_mesh_build(scratch, &sin);
        if (solid.vert_count > STRUCTURE_MESH_MAX_VERTS - 3)
            CLOG_WARN("[WORLD] Chunk (%d, %d) filled its structure budget; "
                      "some geometry was dropped", chunk->chunk_x, chunk->chunk_y);
        if (solid.vert_count > 0) {
            solid.range_count = 1;
            solid.ranges[0].tileset = 0;
            solid.ranges[0].first   = 0;
            solid.ranges[0].count   = solid.vert_count;
        }
    }
    ground_renderer_upload(&chunk->structures, &solid);
    chunk->gpu_dirty = 0;
}

/**
 * Draw one kind of chunk geometry for every visible chunk, building any that
 * are stale.
 */
static void render_chunks(const WorldState* world, const Camera* camera, int solid) {
    int sc_x, ec_x, sc_y, ec_y;
    visible_chunk_range(world, camera, &sc_x, &ec_x, &sc_y, &ec_y);

    CameraView view;
    camera_get_view(camera, &view);
    ground_renderer_begin(&view, solid, camera->x, camera->y);

    for (int cy = sc_y; cy <= ec_y; cy++) {
        for (int cx = sc_x; cx <= ec_x; cx++) {
            /* Structures are drawn from what the ground pass already made
             * resident; loading here would evict chunks mid-frame. */
            Chunk* chunk = solid ? peek_chunk(world, cx, cy)
                                 : world_get_chunk((WorldState*)world, cx, cy);
            if (!chunk) continue;
            if (chunk->gpu_dirty) chunk_build_gpu(world, chunk);
            ground_renderer_draw(solid ? &chunk->structures : &chunk->ground,
                                 world->tileset_textures);
        }
    }

    ground_renderer_end();
}

void world_render(const WorldState* world, const Camera* camera) {
    render_chunks(world, camera, 0);
}

void world_render_structures(const WorldState* world, const Camera* camera) {
    render_chunks(world, camera, 1);
}
