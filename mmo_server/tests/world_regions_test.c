/**
 * @file
 * Exercise the shared city and currency table.
 *
 * This table is the one place the client's world generator and the server's
 * respawn and economy agree on where cities are and what they mint, so the
 * tests here are about that contract: every currency resolves to exactly one
 * city, nearest-city lookup is correct at the boundaries, and the table stays
 * usable when a kingdom is added.
 */

#include "world_regions.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

/** Record one assertion result and print it. */
static void check(int condition, const char* what) {
    if (condition) {
        printf("  ok   %s\n", what);
    } else {
        printf("  FAIL %s\n", what);
        g_failures++;
    }
}

/** Report the pixel centre of a city named by identifier. */
static void center_of(CityId id, float* x, float* y) {
    const WorldCity* c = world_city_find(id);
    world_city_center_px(c, x, y);
}

int main(void) {
    printf("TEST 1: every city is distinct and complete\n");
    check(WORLD_CITY_COUNT == CURRENCY_COUNT,
          "one currency per city");
    for (int i = 0; i < WORLD_CITY_COUNT; i++) {
        check(WORLD_CITIES[i].city != CITY_NONE, "city id is not CITY_NONE");
        check(WORLD_CITIES[i].city_name && WORLD_CITIES[i].city_name[0],
              "city has a display name");
        check(WORLD_CITIES[i].currency_name && WORLD_CITIES[i].currency_name[0],
              "city has a currency name");
        for (int j = i + 1; j < WORLD_CITY_COUNT; j++) {
            check(WORLD_CITIES[i].city != WORLD_CITIES[j].city,
                  "city identifiers are unique");
            check(WORLD_CITIES[i].currency != WORLD_CITIES[j].currency,
                  "currency identifiers are unique");
        }
    }

    printf("TEST 2: currency lookup covers the whole enum\n");
    for (int c = 0; c < CURRENCY_COUNT; c++) {
        check(world_currency_valid(c), "currency in range is valid");
        check(strcmp(world_currency_name(c), "Unknown Currency") != 0,
              "currency resolves to a real name");
    }
    check(!world_currency_valid(-1), "negative currency is rejected");
    check(!world_currency_valid(CURRENCY_COUNT), "past-the-end currency is rejected");
    check(strcmp(world_currency_name(CURRENCY_COUNT), "Unknown Currency") == 0,
          "out-of-range currency names a fallback rather than reading past the table");

    printf("TEST 3: Ennara is the starting city and mints its own coin\n");
    const WorldCity* ennara = world_city_find(CITY_ENNARA);
    check(ennara != NULL, "Ennara is in the table");
    check(ennara->currency == CURRENCY_ENNARA, "Ennara mints Ennara currency");
    check(strcmp(ennara->city_name, "Ennara") == 0, "Ennara keeps its name");
    check(world_city_find(CITY_NONE) == NULL, "CITY_NONE resolves to no city");

    printf("TEST 4: a player standing in a city respawns in that city\n");
    for (int i = 0; i < WORLD_CITY_COUNT; i++) {
        float cx, cy;
        world_city_center_px(&WORLD_CITIES[i], &cx, &cy);
        const WorldCity* near = world_nearest_city_px(cx, cy);
        check(near->city == WORLD_CITIES[i].city,
              "city centre resolves to its own city");
        check(world_local_currency_px(cx, cy) == WORLD_CITIES[i].currency,
              "local currency at a city centre is that city's");
    }

    printf("TEST 5: nearest city is the nearest one, not the first one\n");
    float ex, ey, k2x, k2y;
    center_of(CITY_ENNARA, &ex, &ey);
    center_of(CITY_K2,     &k2x, &k2y);
    // Ennara is first in the table, so a point hard against K2 proves the
    // search compares distances rather than returning the head of the list.
    check(world_nearest_city_px(k2x + 1.0f, k2y + 1.0f)->city == CITY_K2,
          "a point beside Kingdom 2 picks Kingdom 2");
    check(world_nearest_city_px(ex + 1.0f, ey + 1.0f)->city == CITY_ENNARA,
          "a point beside Ennara picks Ennara");

    printf("TEST 6: the midpoint between two cities resolves to one of them\n");
    float mx = (ex + k2x) * 0.5f, my = (ey + k2y) * 0.5f;
    CityId mid = world_nearest_city_px(mx, my)->city;
    check(mid != CITY_NONE, "the midpoint still resolves to a city");

    printf("TEST 7: extreme and degenerate positions still resolve\n");
    check(world_nearest_city_px(0.0f, 0.0f) != NULL,
          "the map origin resolves to a city");
    check(world_nearest_city_px(-1.0e9f, -1.0e9f) != NULL,
          "a position far off the map resolves rather than failing");
    check(world_nearest_city_px(1.0e9f, 1.0e9f) != NULL,
          "a position far past the map resolves rather than failing");

    printf("TEST 8: crediting a balance saturates instead of wrapping\n");
    {
        uint32_t bal[CURRENCY_COUNT] = {0};
        check(world_currency_credit(bal, CURRENCY_ENNARA, 250) == 250,
              "a credit returns the new balance");
        check(bal[CURRENCY_ENNARA] == 250, "the credit landed");
        check(bal[CURRENCY_K1] == 0, "other currencies were not touched");

        bal[CURRENCY_K1] = UINT32_MAX - 5;
        check(world_currency_credit(bal, CURRENCY_K1, 100) == UINT32_MAX,
              "a credit past the maximum clamps");
        check(bal[CURRENCY_K1] == UINT32_MAX,
              "a clamped credit never wraps to a smaller balance");

        check(world_currency_credit(bal, CURRENCY_COUNT, 10) == 0,
              "crediting an unknown currency does nothing");
        check(world_currency_credit(bal, -1, 10) == 0,
              "crediting a negative currency does nothing");
        check(world_currency_credit(NULL, CURRENCY_ENNARA, 10) == 0,
              "crediting through a null balance array does nothing");
    }

    printf("TEST 9: debiting a balance refuses rather than underflowing\n");
    {
        uint32_t bal[CURRENCY_COUNT] = {0};
        bal[CURRENCY_ENNARA] = 100;

        check(world_currency_debit(bal, CURRENCY_ENNARA, 40) == 1,
              "an affordable debit succeeds");
        check(bal[CURRENCY_ENNARA] == 60, "the debit came off the balance");

        check(world_currency_debit(bal, CURRENCY_ENNARA, 61) == 0,
              "a debit one over the balance is refused");
        check(bal[CURRENCY_ENNARA] == 60,
              "a refused debit leaves the balance untouched");

        check(world_currency_debit(bal, CURRENCY_ENNARA, 60) == 1,
              "spending the exact balance is allowed");
        check(bal[CURRENCY_ENNARA] == 0, "the balance is now empty");

        check(world_currency_debit(bal, CURRENCY_ENNARA, 1) == 0,
              "an empty balance cannot be debited");
        check(bal[CURRENCY_ENNARA] == 0,
              "a refused debit on an empty balance does not wrap to a fortune");

        check(world_currency_debit(bal, CURRENCY_COUNT, 1) == 0,
              "debiting an unknown currency is refused");
        check(world_currency_debit(NULL, CURRENCY_ENNARA, 1) == 0,
              "debiting through a null balance array is refused");
    }

    printf("TEST 10: currencies are independent of one another\n");
    {
        uint32_t bal[CURRENCY_COUNT] = {0};
        world_currency_credit(bal, CURRENCY_K2, 500);
        check(world_currency_debit(bal, CURRENCY_K3, 1) == 0,
              "coin in one kingdom cannot pay another kingdom's price");
        check(bal[CURRENCY_K2] == 500, "the funded currency is untouched");
    }

    if (g_failures == 0) {
        printf("ALL ASSERTIONS PASSED\n");
        return 0;
    }
    printf("%d ASSERTION(S) FAILED\n", g_failures);
    return 1;
}
