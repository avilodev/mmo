#define _POSIX_C_SOURCE 200809L

/**
 * @file
 * Check the collision loader: what it accepts, what it refuses, and that it
 * maps the layer rather than copying it.
 *
 * The collision layer is read-only for a world server's whole life and
 * byte-identical across every world on the host, so ten worlds used to hold ten
 * private copies and pay ten cold reads at startup. It is mapped shared now,
 * which is one physical copy behind all of them.
 *
 * That change moves a class of failure. A short read used to be a short read;
 * against a mapping, a file shorter than its header claims is a SIGBUS on the
 * simulation thread the first time somebody walks into the missing region --
 * far from the load that caused it and with nothing in the log to connect the
 * two. The size check that replaced reading to EOF is what keeps that from
 * arising, so the truncation cases below are the point of this file as much as
 * the sharing is.
 *
 * Everything is built against a synthetic world.dat written here, so the cases
 * that matter -- a truncated file, a file with trailing bytes, a fifth tile
 * layer -- are constructible. The shipped world.dat is a gigabyte and has none
 * of them.
 */

#include "world_collision.h"
#include "world_format.h"
#include "log.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int passed = 0;

static void check(int condition, const char* what) {
    printf("  %-4s %s\n", condition ? "ok" : "FAIL", what);
    if (!condition) exit(1);
    passed++;
}

/* --- A world.dat, written to order -------------------------------------- */

/* Big enough that the collision layer spans several pages -- the truncation
 * case is about a mapping running past the end of a file, which is a
 * page-granularity fact. 256x192 is 48 KB of collision bytes, twelve pages. */
#define W        256
#define H        192
#define TILE_PX  16
#define PATH     "/tmp/mmo_collision_test.dat"

/** What the generator would write. Index [y * W + x]. */
static uint8_t g_expected[W * H];

/** Fill the collision layer with a pattern no accidental read would produce. */
static void build_expected(void) {
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            /* A border of solid tiles and a diagonal through the middle, so a
             * layer read at the wrong offset does not happen to look right. */
            g_expected[y * W + x] =
                (x == 0 || y == 0 || x == W - 1 || y == H - 1 || x == y) ? 1 : 0;
}

/** How the file should be malformed, if at all. */
typedef enum {
    FILE_GOOD,
    FILE_BAD_MAGIC,
    FILE_BAD_VERSION,
    FILE_ZERO_LAYERS,
    FILE_TRUNCATED,      /**< Collision layer stops short of what the header claims. */
    FILE_TRAILING,       /**< Bytes after the collision layer. */
    FILE_FIVE_LAYERS,    /**< A layer the reader was not compiled expecting. */
} Flaw;

static void put_u32(FILE* f, uint32_t v) { fwrite(&v, sizeof(v), 1, f); }
static void put_i32(FILE* f, int32_t v)  { fwrite(&v, sizeof(v), 1, f); }

/** Write a world.dat carrying the requested flaw. */
static void write_world(Flaw flaw) {
    FILE* f = fopen(PATH, "wb");
    if (!f) { perror("fopen"); exit(1); }

    const char* magic = (flaw == FILE_BAD_MAGIC) ? "NOTWORLD" : WORLD_FORMAT_MAGIC;
    fwrite(magic, 1, WORLD_FORMAT_MAGIC_LEN, f);

    put_u32(f, (flaw == FILE_BAD_VERSION) ? WORLD_FORMAT_VERSION + 1u
                                          : (uint32_t)WORLD_FORMAT_VERSION);

    uint32_t layers = WORLD_FORMAT_TILE_LAYERS;
    if (flaw == FILE_ZERO_LAYERS) layers = 0;
    if (flaw == FILE_FIVE_LAYERS) layers = 5;
    put_u32(f, layers);

    put_i32(f, W);
    put_i32(f, H);
    put_i32(f, TILE_PX);

    /* A tileset table with two entries, so the collision layer does not start
     * at a fixed offset and certainly not at a page boundary -- which is the
     * arithmetic the mapping has to get right. */
    uint8_t tileset_count = 2;
    fwrite(&tileset_count, 1, 1, f);
    for (int i = 0; i < tileset_count; i++) {
        const char* tp = i == 0 ? "tiles/ground.png" : "tiles/walls.png";
        uint8_t path_len = (uint8_t)strlen(tp);
        fwrite(&path_len, 1, 1, f);
        fwrite(tp, 1, path_len, f);
        uint16_t cols = 16, rows = 16;
        fwrite(&cols, sizeof(cols), 1, f);
        fwrite(&rows, sizeof(rows), 1, f);
    }

    /* Tile layers, filled with a value that is not a plausible collision byte,
     * so a reader that lands inside them is caught by the content check. */
    uint16_t tile = 0x2A2A;
    for (uint32_t l = 0; l < layers; l++)
        for (int i = 0; i < W * H; i++)
            fwrite(&tile, sizeof(tile), 1, f);

    size_t collision_bytes = (size_t)W * H;
    if (flaw == FILE_TRUNCATED) collision_bytes -= 8192;   /* two whole pages short */
    fwrite(g_expected, 1, collision_bytes, f);

    if (flaw == FILE_TRAILING) {
        const char junk[] = "unexpected";
        fwrite(junk, 1, sizeof(junk), f);
    }

    fclose(f);
}

/* --- Tests --------------------------------------------------------------- */

static void test_a_well_formed_world_loads(void) {
    printf("a well-formed world loads and answers what the file says\n");

    write_world(FILE_GOOD);
    check(world_collision_init(PATH) == 1, "the loader accepted it");
    check(world_collision_is_loaded(), "and reports itself loaded");

    float ew = 0.0f, eh = 0.0f;
    world_collision_extent(&ew, &eh);
    check(ew == (float)W * TILE_PX && eh == (float)H * TILE_PX,
          "the extent is the header's dimensions in pixels");

    /* Every tile, not a sample. A mapping that started one page early would
     * agree with the file over most of it and disagree at one edge, which is
     * exactly what a spot check misses. */
    int mismatches = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            float px = (float)x * TILE_PX + TILE_PX / 2.0f;
            float py = (float)y * TILE_PX + TILE_PX / 2.0f;
            int want = g_expected[y * W + x] ? 1 : 0;
            int got  = world_collision_check(px, py) ? 1 : 0;
            if (want != got) mismatches++;
        }
    }
    check(mismatches == 0, "every one of its tiles reads back as written");

    world_collision_shutdown();
}

static void test_the_layer_is_mapped_not_copied(void) {
    printf("the collision layer is mapped rather than copied\n");

    write_world(FILE_GOOD);
    check(world_collision_init(PATH) == 1, "the world loaded");

    /* The whole of finding 07. Ten world servers on a host each held a private
     * malloc of a layer none of them ever writes to -- at the shipped world
     * size, roughly a ninth of a gigabyte, ten times over, plus ten cold reads
     * at startup. A shared read-only mapping is one physical copy behind all of
     * them. */
    check(world_collision_is_mapped(),
          "so ten worlds on one host share one physical copy");

    world_collision_shutdown();
    check(!world_collision_is_mapped(), "and shutdown drops the mapping");
}

static void test_the_copy_fallback_still_works(void) {
    printf("a host that will not map the layer still loads it correctly\n");

    /* The fallback runs when mmap fails, which on any machine anyone tests on
     * is never -- so without a way to force it, it is a path that first runs in
     * production, on somebody else's filesystem, the day it matters.
     * MMO_COLLISION_NO_MMAP is that way. */
    write_world(FILE_GOOD);

    setenv("MMO_COLLISION_NO_MMAP", "1", 1);
    check(world_collision_init(PATH) == 1, "the world loaded without a mapping");
    check(!world_collision_is_mapped(), "and says so");

    int mismatches = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            float px = (float)x * TILE_PX + TILE_PX / 2.0f;
            float py = (float)y * TILE_PX + TILE_PX / 2.0f;
            if ((g_expected[y * W + x] ? 1 : 0) !=
                (world_collision_check(px, py) ? 1 : 0)) mismatches++;
        }
    }
    check(mismatches == 0, "the copied layer reads back exactly as the mapped one does");

    world_collision_shutdown();

    /* A truncated file must be refused on this path too. It is refused before
     * either branch is reached, which is the point -- the size check is what
     * makes the mapping safe, and it protects the copy as well. */
    write_world(FILE_TRUNCATED);
    check(world_collision_init(PATH) == 0, "and a truncated file is still refused");
    world_collision_shutdown();

    unsetenv("MMO_COLLISION_NO_MMAP");

    /* And the mapping comes back once the override is gone, so the variable
     * does not silently stick. */
    write_world(FILE_GOOD);
    check(world_collision_init(PATH) == 1, "the world loads again without the override");
    check(world_collision_is_mapped(), "and is mapped once more");
    world_collision_shutdown();
}

static void test_a_truncated_world_is_refused(void) {
    printf("a world shorter than its header claims is refused at load\n");

    write_world(FILE_TRUNCATED);

    /* Against the old private copy this was a short read, caught immediately.
     * Against a mapping it is worse than a wrong answer: reading a page past
     * the end of the file raises SIGBUS, on the simulation thread, the first
     * time a player walks into the missing region -- a crash with no visible
     * connection to the world file that caused it. Refusing at load is what
     * keeps the failure where it can be understood. */
    check(world_collision_init(PATH) == 0, "the loader refused it");
    check(!world_collision_is_loaded(), "and nothing is loaded");

    /* Fail closed, not open: with no map, everywhere is solid. A loader that
     * refused the file but left the queries answering "walkable" would hand
     * every player a world with no walls. */
    check(world_collision_check(100.0f, 100.0f) == 1,
          "and every position reads as solid rather than open");

    world_collision_shutdown();
}

static void test_a_world_with_trailing_bytes_is_refused(void) {
    printf("a world with bytes after its collision layer is refused\n");

    write_world(FILE_TRAILING);
    check(world_collision_init(PATH) == 0, "the loader refused it");
    check(!world_collision_is_loaded(), "and nothing is loaded");
    world_collision_shutdown();
}

static void test_the_header_gates_the_layout(void) {
    printf("magic, version and layer count each gate the load\n");

    write_world(FILE_BAD_MAGIC);
    check(world_collision_init(PATH) == 0, "a file that is not a world is refused");
    world_collision_shutdown();

    write_world(FILE_BAD_VERSION);
    check(world_collision_init(PATH) == 0, "a future format version is refused");
    world_collision_shutdown();

    write_world(FILE_ZERO_LAYERS);
    check(world_collision_init(PATH) == 0, "a zero tile-layer count is refused");
    world_collision_shutdown();
}

static void test_a_fifth_tile_layer_is_seeked_past(void) {
    printf("a fifth tile layer is skipped by count, not by assumption\n");

    /* The defect world_format.h was written to close: the reader knew about
     * four layers, the generator wrote five, and the seek landed inside the
     * tile data -- which was then loaded as the collision map. Not a crash and
     * not an error, just a world whose walls are in the wrong places while
     * movement validation quietly agrees. The tile layers here are filled with
     * 0x2A2A precisely so that mistake would show. */
    write_world(FILE_FIVE_LAYERS);
    check(world_collision_init(PATH) == 1, "the extra layer did not stop the load");

    int mismatches = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            float px = (float)x * TILE_PX + TILE_PX / 2.0f;
            float py = (float)y * TILE_PX + TILE_PX / 2.0f;
            if ((g_expected[y * W + x] ? 1 : 0) !=
                (world_collision_check(px, py) ? 1 : 0)) mismatches++;
        }
    }
    check(mismatches == 0, "and the collision layer still read back correctly");

    world_collision_shutdown();
}

static void test_queries_outside_the_world_fail_closed(void) {
    printf("positions outside the world, and non-numbers, are solid\n");

    write_world(FILE_GOOD);
    check(world_collision_init(PATH) == 1, "the world loaded");

    float ew = 0.0f, eh = 0.0f;
    world_collision_extent(&ew, &eh);

    check(world_collision_check(-1.0f, 10.0f) == 1, "a negative coordinate is solid");
    check(world_collision_check(ew, 10.0f) == 1, "one tile past the east edge is solid");
    check(world_collision_check(10.0f, eh) == 1, "one past the south edge is solid");
    check(world_collision_check(NAN, 10.0f) == 1, "NaN is solid");
    check(world_collision_check(INFINITY, 10.0f) == 1, "infinity is solid");

    check(!world_coord_is_valid(-1.0f, 10.0f), "and the same positions are not valid");
    check(!world_coord_is_valid(NAN, NAN), "nor is a NaN pair");

    world_collision_shutdown();
}

static void test_load_shutdown_load(void) {
    printf("a world can be loaded, dropped and loaded again\n");

    /* Ten cycles: a mapping leaked per load would show as a growing address
     * space, and one released twice would abort here rather than in a world
     * server shutting down. */
    write_world(FILE_GOOD);
    int all_ok = 1, all_mapped = 1;
    for (int i = 0; i < 10; i++) {
        if (world_collision_init(PATH) != 1) all_ok = 0;
        if (!world_collision_is_mapped())    all_mapped = 0;
        if (world_collision_check(TILE_PX / 2.0f, TILE_PX / 2.0f) != 1) all_ok = 0;
        world_collision_shutdown();
    }
    check(all_ok, "ten load/shutdown cycles each loaded and answered");
    check(all_mapped, "and each one mapped rather than copied");

    /* Shutdown without a load, and twice over: both are what a failed startup
     * does on its way out. */
    world_collision_shutdown();
    world_collision_shutdown();
    check(!world_collision_is_loaded(), "a shutdown with nothing loaded is harmless");
}

int main(void) {
    log_init();
    log_set_level(LOG_LEVEL_ERROR);   // the refusal cases log deliberately

    build_expected();

    test_a_well_formed_world_loads();
    test_the_layer_is_mapped_not_copied();
    test_the_copy_fallback_still_works();
    test_a_truncated_world_is_refused();
    test_a_world_with_trailing_bytes_is_refused();
    test_the_header_gates_the_layout();
    test_a_fifth_tile_layer_is_seeked_past();
    test_queries_outside_the_world_fail_closed();
    test_load_shutdown_load();

    unlink(PATH);
    printf("\n%d checks passed\n", passed);
    return 0;
}
