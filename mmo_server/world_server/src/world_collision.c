/**
 * @file
 * Load the world.dat collision layer and answer bounded world-space collision queries.
 */

#define _POSIX_C_SOURCE 200809L

#include "world_collision.h"
#include "world_format.h"
#include "log.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// collision bytes follow four uint16 tile layers

/** The collision layer, flat [world_height * world_width].
 *
 * Mapped, not copied. It is read-only for the process's whole life and
 * byte-identical across every world server on the host, so ten worlds used to
 * hold ten private copies of it and pay ten cold reads at startup -- at the
 * shipped world size, roughly a ninth of a gigabyte, ten times over. A shared
 * read-only mapping is one physical copy behind all of them, backed by the page
 * cache, and startup stops reading it at all.
 *
 * const because that is now enforced by the kernel: the mapping is PROT_READ
 * and a write would fault rather than silently diverge one world's idea of the
 * map from the rest.
 */
static const uint8_t* g_collision = NULL;

/** The mapping to release, page-aligned and therefore usually not g_collision. */
static void*  g_map     = NULL;
static size_t g_map_len = 0;

/** Set instead of g_map when the mapping failed and the layer was read. */
static uint8_t* g_heap  = NULL;

static int      g_width     = 0;
static int      g_height    = 0;
static float    g_tile_size = 16.0f;  // pixels per tile
static int      g_loaded    = 0;

/** Map the collision layer, or fall back to reading it.
 *
 * @param fd      The open world file. Not consumed: mmap keeps its own
 *                reference, so the caller still closes it.
 * @param offset  Byte offset of the collision layer within the file.
 * @param total   Its length in bytes.
 * @return 1 when g_collision points at the layer, otherwise 0.
 */
static int map_collision_layer(int fd, long offset, size_t total) {
    /* mmap takes a page-aligned offset, and the collision layer starts wherever
     * the tileset table and the tile layers happen to end. So the mapping
     * starts at the page below it and the layer is found at the remainder. */
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) page = 4096;

    off_t  aligned = (off_t)(offset - (offset % page));
    size_t slack   = (size_t)(offset - aligned);
    size_t length  = total + slack;

    /* An escape hatch, and the only way to reach the fallback deliberately.
     * A path that runs only when mmap fails is a path that runs only in
     * production, on somebody else's filesystem, the first time it matters --
     * so it is reachable here, and world_collision_test uses it. */
    const char* no_mmap = getenv("MMO_COLLISION_NO_MMAP");
    int forced_copy = no_mmap && no_mmap[0] && no_mmap[0] != '0';

    void* base = forced_copy ? MAP_FAILED
                             : mmap(NULL, length, PROT_READ, MAP_SHARED, fd, aligned);
    if (base != MAP_FAILED) {
        g_map       = base;
        g_map_len   = length;
        g_collision = (const uint8_t*)base + slack;

        /* Read once, in tile order, and never in a hot path again. Without it
         * the first player to walk into an untouched region pays a page fault
         * inside the movement check, on the simulation thread. The page cache
         * is shared, so only the first world server on the host pays even this,
         * and it does so during startup where the cost is invisible. */
        posix_madvise(base, length, POSIX_MADV_WILLNEED);
        return 1;
    }

    /* Not fatal. A filesystem that will not map (some network mounts, some
     * container layers) should still run a world, just with the private copy
     * this whole path exists to avoid. */
    if (forced_copy)
        LOG_WARN("[COLLISION] MMO_COLLISION_NO_MMAP is set; reading a private "
                 "%zu-byte copy of the collision layer", total);
    else
        LOG_WARN("[COLLISION] Cannot map the collision layer (%s); falling back "
                 "to a private %zu-byte copy. Ten worlds on this host will hold "
                 "ten of them.", strerror(errno), total);

    g_heap = malloc(total);
    if (!g_heap) {
        LOG_ERROR("[COLLISION] Out of memory (%zu bytes)", total);
        return 0;
    }

    if (lseek(fd, (off_t)offset, SEEK_SET) != (off_t)offset) {
        LOG_ERROR("[COLLISION] Cannot seek to the collision layer: %s", strerror(errno));
        free(g_heap);
        g_heap = NULL;
        return 0;
    }

    size_t got = 0;
    while (got < total) {
        ssize_t n = read(fd, g_heap + got, total - got);
        if (n > 0)                       { got += (size_t)n; continue; }
        if (n < 0 && errno == EINTR)     continue;
        break;
    }

    if (got != total) {
        LOG_ERROR("[COLLISION] Short read: expected %zu bytes, got %zu", total, got);
        free(g_heap);
        g_heap = NULL;
        return 0;
    }

    g_collision = g_heap;
    return 1;
}

/**
 * Load the collision layer from a client-format world.dat file.
 *
 * Reads native-width integer fields and treats short or malformed input as failure.
 *
 * @param path  Path to the binary world file.
 * @return      1 on success, or 0 on I/O, format, or allocation failure.
 */
int world_collision_init(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        LOG_ERROR("[COLLISION] Cannot open world.dat at '%s' — collision disabled", path);
        return 0;
    }

    /* The file says what it is, and this refuses anything that does not.
     *
     * There used to be no magic and no version: the header was three int32s
     * and the reader trusted them. Anything at all in that path -- a partial
     * copy, a file from a different generator, a world.dat whose layer count
     * had changed on the client -- was read as coordinates, and whatever
     * followed was loaded as the collision map that every movement check is
     * validated against. Failing here is the only safe answer. */
    char magic[WORLD_FORMAT_MAGIC_LEN];
    if (fread(magic, 1, sizeof(magic), f) != sizeof(magic) ||
        memcmp(magic, WORLD_FORMAT_MAGIC, sizeof(magic)) != 0) {
        LOG_ERROR("[COLLISION] '%s' is not a world file (bad magic). "
                  "Regenerate it with `make world` in the client tree, or "
                  "`make setup` here.", path);
        fclose(f);
        return 0;
    }

    uint32_t version = 0, tile_layers = 0;
    if (fread(&version,     sizeof(version),     1, f) != 1 ||
        fread(&tile_layers, sizeof(tile_layers), 1, f) != 1) {
        LOG_ERROR("[COLLISION] '%s' ends inside its header", path);
        fclose(f);
        return 0;
    }

    if (version != WORLD_FORMAT_VERSION) {
        LOG_ERROR("[COLLISION] '%s' is world format version %u; this server reads "
                  "version %d. Regenerate the world.",
                  path, version, WORLD_FORMAT_VERSION);
        fclose(f);
        return 0;
    }

    if (tile_layers == 0 || tile_layers > WORLD_FORMAT_MAX_TILE_LAYERS) {
        LOG_ERROR("[COLLISION] '%s' declares %u tile layers, which is not usable",
                  path, tile_layers);
        fclose(f);
        return 0;
    }

    int32_t w = 0, h = 0, ts = 0;
    if (fread(&w,  sizeof(int32_t), 1, f) != 1 ||
        fread(&h,  sizeof(int32_t), 1, f) != 1 ||
        fread(&ts, sizeof(int32_t), 1, f) != 1 ||
        w <= 0 || h <= 0) {
        LOG_ERROR("[COLLISION] world.dat header invalid");
        fclose(f);
        return 0;
    }

    /* The dimensions size an allocation, so an implausible header is a request
     * to allocate an implausible amount of memory. */
    if (w > WORLD_FORMAT_MAX_DIMENSION || h > WORLD_FORMAT_MAX_DIMENSION) {
        LOG_ERROR("[COLLISION] '%s' declares %dx%d tiles, past the %d per-axis limit",
                  path, w, h, WORLD_FORMAT_MAX_DIMENSION);
        fclose(f);
        return 0;
    }

    /* And the product, which is the number actually being allocated: two
     * dimensions that each pass the check above can still multiply into
     * something no machine will hand over. */
    if ((uint64_t)w * (uint64_t)h > WORLD_FORMAT_MAX_TILES) {
        LOG_ERROR("[COLLISION] '%s' declares %dx%d = %llu tiles, past the %llu limit",
                  path, w, h, (unsigned long long)((uint64_t)w * (uint64_t)h),
                  (unsigned long long)WORLD_FORMAT_MAX_TILES);
        fclose(f);
        return 0;
    }

    g_tile_size = (ts > 0) ? (float)ts : 16.0f;

    // Skip tileset table (variable length)
    uint8_t ts_count = 0;
    if (fread(&ts_count, 1, 1, f) != 1) {
        LOG_ERROR("[COLLISION] Failed to read tileset count");
        fclose(f);
        return 0;
    }
    for (int i = 0; i < ts_count; i++) {
        uint8_t path_len = 0;
        if (fread(&path_len, 1, 1, f) != 1) {
            LOG_ERROR("[COLLISION] Failed to read tileset path_len");
            fclose(f);
            return 0;
        }
        // skip path + cols (uint16) + rows (uint16)
        if (fseek(f, path_len + 4, SEEK_CUR) != 0) {
            LOG_ERROR("[COLLISION] Failed to seek past tileset entry");
            fclose(f);
            return 0;
        }
    }

    /* Skip the tile layers, by the count the FILE declares rather than by a
     * constant compiled into this reader. That constant was the defect: the
     * client could add a fifth layer and this seek would land inside the tile
     * data, loading it as collision. */
    long tile_bytes = (long)w * h * (long)sizeof(uint16_t) * (long)tile_layers;
    if (fseek(f, tile_bytes, SEEK_CUR) != 0) {
        LOG_ERROR("[COLLISION] Failed to seek past tile data");
        fclose(f);
        return 0;
    }

    /* Where the collision layer begins. The header walk above is what
     * establishes it -- the tileset table is variable length -- so it is read
     * off the stream rather than computed a second time here. */
    long offset = ftell(f);
    if (offset < 0) {
        LOG_ERROR("[COLLISION] Cannot locate the collision layer in '%s'", path);
        fclose(f);
        return 0;
    }

    size_t total = (size_t)w * (size_t)h;

    /* The collision layer is the last thing in the file, so the file is exactly
     * as long as the header says it should be. Bytes missing means a truncated
     * copy; bytes left over mean the reader and the generator disagree about
     * the layout -- the drift the version field exists to catch, in the case
     * where the sizes happen to line up anyway.
     *
     * This replaces reading to EOF and checking for one more byte. It has to:
     * a mapping past the end of a file is not a short read, it is a SIGBUS on
     * the simulation thread the first time somebody walks there. The size is
     * checked here so that cannot arise. */
    struct stat st;
    if (fstat(fileno(f), &st) != 0) {
        LOG_ERROR("[COLLISION] Cannot stat '%s': %s", path, strerror(errno));
        fclose(f);
        return 0;
    }

    if ((uint64_t)st.st_size != (uint64_t)offset + (uint64_t)total) {
        LOG_ERROR("[COLLISION] '%s' is %llu bytes; its header describes %llu. "
                  "The file is truncated, or the reader and the generator "
                  "disagree about the layout.",
                  path, (unsigned long long)st.st_size,
                  (unsigned long long)((uint64_t)offset + (uint64_t)total));
        fclose(f);
        return 0;
    }

    if (!map_collision_layer(fileno(f), offset, total)) {
        fclose(f);
        return 0;
    }

    /* The mapping holds its own reference to the file, so the descriptor has
     * done its job. Unlinking or replacing world.dat from here on leaves this
     * process reading the bytes it mapped, which is what a running world
     * wants. */
    fclose(f);

    g_width  = w;
    g_height = h;
    g_loaded = 1;

    LOG_INFO("[COLLISION] %s %dx%d tile map (tile=%gpx, %u tile layers, %zu "
             "collision bytes) from %s",
             g_map ? "Mapped" : "Read", g_width, g_height, g_tile_size,
             tile_layers, total, path);
    return 1;
}

/**
 * Determine whether collision data is loaded.
 *
 * @return 1 when loaded, or 0 otherwise.
 */
int world_collision_is_loaded(void) {
    return g_loaded;
}

/**
 * Report the loaded world's pixel extent.
 *
 * @param out_width   Receives width in pixels; may be NULL.
 * @param out_height  Receives height in pixels; may be NULL.
 */
float world_tile_size(void) {
    return g_tile_size;
}

void world_collision_extent(float* out_width, float* out_height) {
    if (out_width)  *out_width  = g_loaded ? (float)g_width  * g_tile_size : 0.0f;
    if (out_height) *out_height = g_loaded ? (float)g_height * g_tile_size : 0.0f;
}

/**
 * Check that a finite position lies within the loaded world extent.
 *
 * @return 1 when valid, or 0 otherwise.
 */
int world_coord_is_valid(float x, float y) {
    if (!g_loaded) return 0;
    if (!isfinite(x) || !isfinite(y)) return 0;

    float w, h;
    world_collision_extent(&w, &h);
    return x >= 0.0f && x < w && y >= 0.0f && y < h;
}

/**
 * Test a world position against the collision tile layer.
 *
 * Unloaded maps, non-finite coordinates, and out-of-bounds positions fail closed.
 *
 * @return 1 for solid or invalid space, or 0 for open space.
 */
int world_collision_check(float x, float y) {
    if (!g_loaded) return 1;  // No collision data — nothing is walkable

    // validate floats before integer conversion
    if (!isfinite(x) || !isfinite(y)) return 1;

    float tx = x / g_tile_size;
    float ty = y / g_tile_size;

    if (!(tx >= 0.0f) || !(ty >= 0.0f) ||
        !(tx < (float)g_width) || !(ty < (float)g_height))
        return 1;  // Out of bounds = solid

    return g_collision[(int)ty * g_width + (int)tx];
}

/**
 * Test the four corners of an axis-aligned box for collision.
 *
 * @param half_size  Box half-width and half-height in pixels.
 * @return           1 when any corner is solid, or 0 otherwise.
 */
int world_collision_check_box(float x, float y, float half_size) {
    return world_collision_check(x - half_size, y - half_size) ||
           world_collision_check(x + half_size, y - half_size) ||
           world_collision_check(x - half_size, y + half_size) ||
           world_collision_check(x + half_size, y + half_size);
}

/** Refuse collision paths requiring more than this many samples. */
#define COLLISION_PATH_MAX_STEPS 256

/**
 * Test an axis-aligned box along a movement segment.
 *
 * Samples at half-tile intervals, excludes the origin, and fails closed for invalid or excessive paths.
 *
 * @param x0         Origin X coordinate in pixels.
 * @param y0         Origin Y coordinate in pixels.
 * @param x1         Destination X coordinate in pixels.
 * @param y1         Destination Y coordinate in pixels.
 * @param half_size  Box half-width and half-height in pixels.
 * @return           1 when the path collides or is invalid, or 0 when clear.
 */
int world_collision_check_box_path(float x0, float y0,
                                   float x1, float y1,
                                   float half_size) {
    if (!g_loaded) return 1;
    if (!isfinite(x0) || !isfinite(y0) || !isfinite(x1) || !isfinite(y1))
        return 1;

    float dx = x1 - x0;
    float dy = y1 - y0;
    float distance = sqrtf(dx * dx + dy * dy);

    // sample at half-tile intervals to prevent tunneling
    float interval = g_tile_size * 0.5f;
    int steps = (int)(distance / interval) + 1;
    if (steps > COLLISION_PATH_MAX_STEPS) return 1;

    for (int i = 1; i <= steps; i++) {
        float t = (float)i / (float)steps;
        if (world_collision_check_box(x0 + dx * t, y0 + dy * t, half_size))
            return 1;
    }
    return 0;
}

int world_collision_is_mapped(void) {
    return g_loaded && g_map != NULL;
}

/**
 * Release the loaded collision layer.
 */
void world_collision_shutdown(void) {
    if (g_map) munmap(g_map, g_map_len);
    free(g_heap);

    g_map       = NULL;
    g_map_len   = 0;
    g_heap      = NULL;
    g_collision = NULL;
    g_loaded    = 0;
}
