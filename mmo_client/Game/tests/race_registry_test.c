/**
 * @file
 * Check the client-side race registry and the presentation derived from it.
 *
 * The point of the registry is that the client holds no table of races: it
 * asks the server, and everything it draws for a race follows from the answer.
 * What is checked here is that following-from: a race the server sends gets a
 * colour without any client change, two races get different colours, the same
 * race gets the same colour twice, and a race the server has never mentioned
 * still draws as something rather than nothing.
 */

#include "core/race_registry.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond, what)                                                   \
    do {                                                                    \
        if (cond) {                                                         \
            printf("  ok   %s\n", (what));                                  \
        } else {                                                            \
            printf("  FAIL %s  (%s:%d)\n", (what), __FILE__, __LINE__);     \
            g_failures++;                                                   \
        }                                                                   \
    } while (0)

/** Build a race list the way the realm would send one. */
static void build(RaceListResponsePacket* pkt, int count) {
    memset(pkt, 0, sizeof(*pkt));
    pkt->count = (uint8_t)count;

    static const char* keys[] = { "wolf", "bear", "fox", "crow", "hawk" };
    for (int i = 0; i < count; i++) {
        pkt->races[i].race_id = (uint32_t)(i + 1);
        snprintf(pkt->races[i].key, sizeof(pkt->races[i].key), "%s",
                 keys[i % (int)(sizeof(keys) / sizeof(keys[0]))]);
        snprintf(pkt->races[i].name, sizeof(pkt->races[i].name), "Race %d", i + 1);
        pkt->races[i].playable     = 1;
        pkt->races[i].default_role = (i == 1) ? ROLE_HEALER : ROLE_DPS;
    }
}

static int color_differs(const float a[3], const float b[3]) {
    for (int i = 0; i < 3; i++) {
        float d = a[i] - b[i];
        if (d > 0.02f || d < -0.02f) return 1;
    }
    return 0;
}

int main(void) {
    printf("=== client race registry ===\n");

    printf("\nTEST 1: nothing is known before the server answers\n");
    {
        client_races_store(NULL);
        CHECK(client_races_count() == 0, "the registry starts empty");
        CHECK(client_race_find(1) == NULL, "and resolves no race");

        float c[3];
        client_race_color(1, &c[0], &c[1], &c[2]);
        CHECK(c[0] > 0.0f && c[1] > 0.0f && c[2] > 0.0f,
              "an unknown race still gets a drawable colour");
    }

    printf("\nTEST 2: a stored list is what the client answers from\n");
    {
        RaceListResponsePacket pkt;
        build(&pkt, 5);
        client_races_store(&pkt);

        CHECK(client_races_count() == 5, "all five races are held");

        const RaceInfo* wolf = client_race_find(1);
        CHECK(wolf != NULL, "the first race resolves");
        CHECK(wolf && strcmp(wolf->key, "wolf") == 0, "with its key");
        CHECK(client_race_find(99) == NULL, "and a race not sent does not");
    }

    printf("\nTEST 3: colour follows the race, not a table\n");
    {
        float first[3], again[3], other[3];
        client_race_color(1, &first[0], &first[1], &first[2]);
        client_race_color(1, &again[0], &again[1], &again[2]);
        client_race_color(2, &other[0], &other[1], &other[2]);

        CHECK(!color_differs(first, again), "the same race is the same colour twice");
        CHECK(color_differs(first, other), "two races are different colours");

        for (int i = 0; i < 3; i++) {
            CHECK(first[i] >= 0.0f && first[i] <= 1.0f,
                  "every channel is inside the drawable range");
        }

        /* The reason the derivation fixes saturation and value: nothing it can
         * produce may come out invisible against the world. */
        float brightest = first[0];
        for (int i = 1; i < 3; i++) if (first[i] > brightest) brightest = first[i];
        CHECK(brightest > 0.3f, "no race can come out too dark to see");
    }

    printf("\nTEST 4: a race added later needs no client change\n");
    {
        RaceListResponsePacket pkt;
        build(&pkt, 5);
        /* A sixth race, of a kind this client has never heard of. */
        pkt.count = 6;
        pkt.races[5].race_id = 6;
        snprintf(pkt.races[5].key, sizeof(pkt.races[5].key), "pangolin");
        pkt.races[5].playable     = 1;
        pkt.races[5].default_role = ROLE_TANK;
        client_races_store(&pkt);

        CHECK(client_races_count() == 6, "the new race is held");
        CHECK(client_race_find(6) != NULL, "and resolves");

        float known[3], added[3];
        client_race_color(1, &known[0], &known[1], &known[2]);
        client_race_color(6, &added[0], &added[1], &added[2]);
        CHECK(color_differs(known, added),
              "and gets a colour of its own with no table entry");
    }

    printf("\nTEST 5: the resource bar follows the role the server gave\n");
    {
        CHECK(!client_race_uses_energy(2), "a healer race draws a mana bar");
        CHECK(client_race_uses_energy(1),  "a dps race does not");
        CHECK(client_race_uses_energy(6),  "and neither does a tank");
        CHECK(!client_race_uses_energy(99),
              "an unknown race falls back to mana rather than guessing");
    }

    printf("\nTEST 6: an oversized list is clamped, not trusted\n");
    {
        RaceListResponsePacket pkt;
        build(&pkt, 5);
        pkt.count = 200;   /* past MAX_RACE_LIST; a hostile server can say this */
        client_races_store(&pkt);
        CHECK(client_races_count() <= MAX_RACE_LIST,
              "the count is clamped to what the packet can hold");
    }

    printf("\n");
    if (g_failures) {
        printf("%d ASSERTION%s FAILED\n", g_failures, g_failures == 1 ? "" : "S");
        return 1;
    }
    printf("ALL ASSERTIONS PASSED\n");
    return 0;
}
