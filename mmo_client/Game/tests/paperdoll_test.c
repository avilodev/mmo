/**
 * @file
 * Check the character layer stack against the art on disk.
 *
 * The layer scheme rests on one invariant that nothing at runtime can notice
 * going wrong: every layer file is the same untrimmed square, so a part's
 * position inside its own image is its position on the character. Trim one
 * export, or resize one, and the game does not crash or log anything -- the
 * hat just sits beside the head. That is exactly the kind of breakage a test
 * should catch and a human reviewing a sprite will not.
 *
 * So this reads the PNG headers directly. No OpenGL, no stb_image, no image
 * decode: an IHDR is at a fixed offset and carries the dimensions in the first
 * eight bytes, which is all that needs checking.
 *
 * Draw order is asserted separately, as relations rather than as a copy of the
 * enum -- restating the enum would only test that the file was pasted twice.
 */

#include "player/paperdoll.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (cond) {                                                         \
            printf("  ok   "); printf(__VA_ARGS__); printf("\n");           \
        } else {                                                            \
            printf("  FAIL "); printf(__VA_ARGS__); printf("\n");           \
            g_failures++;                                                   \
        }                                                                   \
    } while (0)

/* paperdoll.c reaches OpenGL through these; the test never calls a function
 * that uses them, but the linker still wants the symbols. */
unsigned int texture_load(const char* path) { (void)path; return 0; }
void texture_unload(unsigned int id) { (void)id; }
void texture_get_size(unsigned int id, int* w, int* h) { (void)id; (void)w; (void)h; }
void renderer_draw_sprite(float x, float y, float w, float h, unsigned int t) {
    (void)x; (void)y; (void)w; (void)h; (void)t;
}

/** Read a PNG's declared width and height from its IHDR. */
static int png_size(const char* path, int* w, int* h) {
    static const unsigned char sig[8] = { 0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A };
    unsigned char head[24];

    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(head, 1, sizeof(head), f);
    fclose(f);

    if (n != sizeof(head)) return 0;
    if (memcmp(head, sig, 8) != 0) return 0;
    if (memcmp(head + 12, "IHDR", 4) != 0) return 0;

    *w = (head[16] << 24) | (head[17] << 16) | (head[18] << 8) | head[19];
    *h = (head[20] << 24) | (head[21] << 16) | (head[22] << 8) | head[23];
    return 1;
}

/* Draw order matters in ways that are about the character, not about the enum:
 * these are the relations that would produce a visibly wrong picture. */
static void check_draw_order(void) {
    printf("draw order\n");
    CHECK(PAPERDOLL_TAIL  < PAPERDOLL_BODY,  "tail is behind the body");
    CHECK(PAPERDOLL_BODY  < PAPERDOLL_EYES,  "eyes are on top of the face");
    CHECK(PAPERDOLL_BODY  < PAPERDOLL_CHEST, "the chest piece covers the body");
    CHECK(PAPERDOLL_BODY  < PAPERDOLL_LEGS,  "leggings cover the body");
    CHECK(PAPERDOLL_LEGS  < PAPERDOLL_CHEST, "a long coat covers the trousers");
    CHECK(PAPERDOLL_LEGS  < PAPERDOLL_BOOTS, "boots cover the trouser hem");
    CHECK(PAPERDOLL_EARS  < PAPERDOLL_HAIR,  "hair falls over the ears");
    CHECK(PAPERDOLL_EYES  < PAPERDOLL_HAIR,  "a fringe falls over the eyes");
}

static void check_layer_table(void) {
    printf("layer table\n");
    for (int i = 0; i < PAPERDOLL_LAYERS; i++) {
        const char* path = paperdoll_layer_path((PaperdollLayer)i);
        const char* name = paperdoll_layer_name((PaperdollLayer)i);
        CHECK(path != NULL && path[0] != '\0', "layer %d (%s) names a file", i, name);
        CHECK(name != NULL && strcmp(name, "?") != 0, "layer %d has a name", i);
    }
    CHECK(paperdoll_layer_path((PaperdollLayer)-1) == NULL, "below range is NULL");
    CHECK(paperdoll_layer_path(PAPERDOLL_LAYERS) == NULL, "above range is NULL");
}

/* Run from the client root, where the game's own asset paths resolve. */
static void check_assets_on_disk(void) {
    printf("assets (relative to the client root)\n");
    for (int i = 0; i < PAPERDOLL_LAYERS; i++) {
        const char* path = paperdoll_layer_path((PaperdollLayer)i);
        const char* name = paperdoll_layer_name((PaperdollLayer)i);
        int w = 0, h = 0;

        if (!png_size(path, &w, &h)) {
            CHECK(0, "%s: readable PNG at %s", name, path);
            continue;
        }
        CHECK(w == PAPERDOLL_FRAME_PX && h == PAPERDOLL_FRAME_PX,
              "%s: %dx%d, untrimmed %dx%d frame",
              name, w, h, PAPERDOLL_FRAME_PX, PAPERDOLL_FRAME_PX);
    }
}

/* An unloaded doll must be safe to draw: it is what a player sees for the
 * frames between entering the world and the textures existing. */
static void check_empty_doll_is_inert(void) {
    printf("empty doll\n");
    Paperdoll doll;
    memset(&doll, 0, sizeof(doll));

    paperdoll_render(&doll, 0.0f, 0.0f, 64.0f);
    paperdoll_render(NULL, 0.0f, 0.0f, 64.0f);
    paperdoll_unload(&doll);
    paperdoll_unload(NULL);
    CHECK(doll.loaded == 0, "drawing and unloading an empty doll is a no-op");

    CHECK(paperdoll_shared() != NULL, "the shared doll always exists");
    CHECK(paperdoll_shared()->loaded == 0, "the shared doll starts unloaded");
}

int main(void) {
    printf("paperdoll\n");
    check_draw_order();
    check_layer_table();
    check_assets_on_disk();
    check_empty_doll_is_inert();

    if (g_failures) {
        printf("FAILED: %d check(s)\n", g_failures);
        return 1;
    }
    printf("PASSED\n");
    return 0;
}
