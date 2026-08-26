/**
 * @file
 * Keep the race list the server sent, and derive presentation from it.
 */

#include "core/race_registry.h"

#include <math.h>
#include <string.h>

/* Written on the network thread, read on the render thread.
 *
 * `g_count` is the publication point: entries are copied first and the count
 * written last, so a reader either sees no registry or a complete one. There
 * is no lock because there is no mutation after publication -- a later list
 * replaces the entries and republishes, and the only way a reader can be
 * wrong is by using a race that has just stopped existing, which resolves to
 * the neutral fallback below. */
static RaceInfo g_races[MAX_RACE_LIST];
static volatile int g_count = 0;

void client_races_store(const RaceListResponsePacket* packet) {
    if (!packet) { g_count = 0; return; }

    int count = packet->count;
    if (count < 0) count = 0;
    if (count > MAX_RACE_LIST) count = MAX_RACE_LIST;

    /* Cleared, then filled, then counted. */
    g_count = 0;
    memset(g_races, 0, sizeof(g_races));
    for (int i = 0; i < count; i++) g_races[i] = packet->races[i];
    g_count = count;
}

int client_races_count(void) { return g_count; }

const RaceInfo* client_race_find(uint32_t race_id) {
    int count = g_count;
    for (int i = 0; i < count; i++) {
        if (g_races[i].race_id == race_id) return &g_races[i];
    }
    return NULL;
}

/** FNV-1a over the race's stable key. */
static uint32_t key_hash(const char* key) {
    uint32_t h = 2166136261u;
    for (const unsigned char* p = (const unsigned char*)key; *p; p++)
        h = (h ^ *p) * 16777619u;
    return h;
}

/**
 * Hue, saturation and value to RGB.
 *
 * Fixed saturation and value are what make the derivation usable: every colour
 * it can produce is mid-bright and clearly coloured, so no race can come out
 * black on a dark map or white on a bright one. Only the hue varies.
 */
static void hsv_to_rgb(float h, float s, float v, float* r, float* g, float* b) {
    float c = v * s;
    float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
    float m = v - c;

    float rr = 0.0f, gg = 0.0f, bb = 0.0f;
    if      (h <  60.0f) { rr = c; gg = x; }
    else if (h < 120.0f) { rr = x; gg = c; }
    else if (h < 180.0f) { gg = c; bb = x; }
    else if (h < 240.0f) { gg = x; bb = c; }
    else if (h < 300.0f) { rr = x; bb = c; }
    else                 { rr = c; bb = x; }

    *r = rr + m;
    *g = gg + m;
    *b = bb + m;
}

void client_race_color(uint32_t race_id, float* r, float* g, float* b) {
    /* Neutral grey. Reached before the registry arrives and for a race it does
     * not list; presentation must never be a reason not to draw a player. */
    *r = 0.45f; *g = 0.45f; *b = 0.50f;

    const RaceInfo* race = client_race_find(race_id);
    if (!race || race->key[0] == '\0') return;

    /* The hue is quantised to 24 steps around the circle. Un-quantised, two
     * races could land three degrees apart and read as the same colour; at 15
     * degrees apart they are always tellable, and 24 slots is comfortably more
     * than MAX_RACE_LIST is ever likely to hold at once. */
    uint32_t h = key_hash(race->key);
    float hue = (float)(h % 24u) * 15.0f;

    hsv_to_rgb(hue, 0.62f, 0.80f, r, g, b);
}

int client_race_uses_energy(uint32_t race_id) {
    const RaceInfo* race = client_race_find(race_id);
    if (!race) return 0;

    /* Mana belongs to the healer role; every other role runs on something
     * else. This was `player_class == 2` -- one hardcoded race number -- so a
     * second non-healer race drew a mana bar for a resource that is not mana. */
    return race->default_role != ROLE_HEALER;
}
