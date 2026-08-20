#ifndef WORLD_REGIONS_H
#define WORLD_REGIONS_H

/**
 * @file
 * Own the continent's scale, its capital cities, and the kingdom currencies.
 *
 * This is the single definition shared by the client (world generation, UI) and
 * the world server (respawn, economy). The two copies — mmo_client/common and
 * mmo_server/common/include — must stay byte-identical, exactly like protocol.h.
 *
 * Adding a kingdom means adding one row to WORLD_CITIES. Nothing in either tree
 * hardcodes the city or currency count: the wire format, the database, and the
 * currency panel all size themselves from this table.
 */

#include <stddef.h>
#include <stdint.h>

/**
 * World scale divisor.
 *
 * Every distance is written at full continent scale and divided by this, so the
 * map, capitals, districts, and buildings shrink together and keep their
 * proportions. 3 is the working dev world; set to 1 for the full-size build.
 */
#ifndef WORLD_SCALE_DIV
#define WORLD_SCALE_DIV 3
#endif

/** Scale a full-size tile distance to the current world, rounding to nearest. */
#define WG_SCALE(v)  (((v) + WORLD_SCALE_DIV / 2) / WORLD_SCALE_DIV)
#define WG_SCALEF(v) ((v) / (float)WORLD_SCALE_DIV)

/** Tile edge length in world pixels. Does not scale: it is the art grid. */
#define WORLD_TILE_PX 16

/** Identify a capital. Zero means "no city". */
typedef enum {
    CITY_NONE = 0,
    CITY_K1,
    CITY_K2,
    CITY_K3,
    CITY_ENNARA
} CityId;

/**
 * Identify a kingdom currency.
 *
 * Values are dense from zero because they index the player's balance array on
 * the wire and in the database. Never renumber a currency that has shipped:
 * stored balances are keyed by these values.
 */
typedef enum {
    CURRENCY_ENNARA = 0,
    CURRENCY_K1,
    CURRENCY_K2,
    CURRENCY_K3,
    CURRENCY_COUNT
} CurrencyId;

/**
 * Capital centre coordinates in tiles.
 *
 * Macros rather than table lookups because generation code needs them in static
 * initializers, where a const object read is not a constant expression.
 */
#define CITY_ENNARA_TILE_X WG_SCALE(13900)
#define CITY_ENNARA_TILE_Y WG_SCALE(5580)
#define CITY_K1_TILE_X     WG_SCALE(8100)
#define CITY_K1_TILE_Y     WG_SCALE(3200)
#define CITY_K2_TILE_X     WG_SCALE(4600)
#define CITY_K2_TILE_Y     WG_SCALE(900)
#define CITY_K3_TILE_X     WG_SCALE(7300)
#define CITY_K3_TILE_Y     WG_SCALE(6550)

/** Describe one capital, its currency, and where it sits in tiles. */
typedef struct {
    CityId      city;
    CurrencyId  currency;
    int         tile_x;
    int         tile_y;
    const char* city_name;
    const char* currency_name;
} WorldCity;

/**
 * List every capital.
 *
 * Ennara is the Kingdom IV capital and the starting city; the other three are
 * reserved footprints with no interior design yet, so they carry placeholder
 * names until the world bible names them.
 */
static const WorldCity WORLD_CITIES[] = {
    { CITY_ENNARA, CURRENCY_ENNARA, CITY_ENNARA_TILE_X, CITY_ENNARA_TILE_Y,
      "Ennara",     "Ennara Currency"     },
    { CITY_K1,     CURRENCY_K1,     CITY_K1_TILE_X,     CITY_K1_TILE_Y,
      "Kingdom 1",  "Kingdom 1 Currency"  },
    { CITY_K2,     CURRENCY_K2,     CITY_K2_TILE_X,     CITY_K2_TILE_Y,
      "Kingdom 2",  "Kingdom 2 Currency"  },
    { CITY_K3,     CURRENCY_K3,     CITY_K3_TILE_X,     CITY_K3_TILE_Y,
      "Kingdom 3",  "Kingdom 3 Currency"  },
};

#define WORLD_CITY_COUNT ((int)(sizeof(WORLD_CITIES) / sizeof(WORLD_CITIES[0])))

/** Report whether a currency identifier is one this build knows. */
static inline int world_currency_valid(int currency) {
    return currency >= 0 && currency < CURRENCY_COUNT;
}

/**
 * Look up a currency's display name.
 *
 * @return The name, or "Unknown Currency" for an out-of-range identifier.
 */
static inline const char* world_currency_name(int currency) {
    for (int i = 0; i < WORLD_CITY_COUNT; i++)
        if ((int)WORLD_CITIES[i].currency == currency)
            return WORLD_CITIES[i].currency_name;
    return "Unknown Currency";
}

/**
 * Look up the city that mints a currency, by name.
 *
 * Shorter than the currency's own name, for places too tight to spell out
 * "Ennara Currency".
 *
 * @return The city name, or "Unknown" for an out-of-range identifier.
 */
static inline const char* world_currency_city_name(int currency) {
    for (int i = 0; i < WORLD_CITY_COUNT; i++)
        if ((int)WORLD_CITIES[i].currency == currency)
            return WORLD_CITIES[i].city_name;
    return "Unknown";
}

/**
 * Find a city by identifier.
 *
 * @return The city record, or NULL when no city carries that identifier.
 */
static inline const WorldCity* world_city_find(CityId city) {
    for (int i = 0; i < WORLD_CITY_COUNT; i++)
        if (WORLD_CITIES[i].city == city) return &WORLD_CITIES[i];
    return NULL;
}

/** Report a city's centre in world pixels. */
static inline void world_city_center_px(const WorldCity* c,
                                        float* out_x, float* out_y) {
    if (out_x) *out_x = (float)c->tile_x * WORLD_TILE_PX;
    if (out_y) *out_y = (float)c->tile_y * WORLD_TILE_PX;
}

/**
 * Find the capital closest to a world-pixel position.
 *
 * Distance is squared Euclidean in pixels, computed in double so a full-scale
 * continent cannot overflow the intermediate product.
 *
 * @return The nearest city; never NULL, because the table is never empty.
 */
static inline const WorldCity* world_nearest_city_px(float x, float y) {
    const WorldCity* best = &WORLD_CITIES[0];
    double best_d2 = -1.0;

    for (int i = 0; i < WORLD_CITY_COUNT; i++) {
        float cx, cy;
        world_city_center_px(&WORLD_CITIES[i], &cx, &cy);
        double dx = (double)x - (double)cx;
        double dy = (double)y - (double)cy;
        double d2 = dx * dx + dy * dy;
        if (best_d2 < 0.0 || d2 < best_d2) {
            best_d2 = d2;
            best    = &WORLD_CITIES[i];
        }
    }
    return best;
}

/**
 * Credit a balance, saturating instead of wrapping.
 *
 * Wrapping would let a large payment reduce a rich player's balance, so the
 * arithmetic clamps at the maximum a balance can hold.
 *
 * @param balances  Array of CURRENCY_COUNT balances.
 * @return The new balance, or 0 for an unknown currency.
 */
static inline uint32_t world_currency_credit(uint32_t* balances, int currency,
                                             uint32_t amount) {
    if (!balances || !world_currency_valid(currency)) return 0;

    uint32_t balance = balances[currency];
    balances[currency] = (balance > UINT32_MAX - amount) ? UINT32_MAX
                                                         : balance + amount;
    return balances[currency];
}

/**
 * Debit a balance, refusing rather than going negative.
 *
 * An unsigned underflow here would hand the player a fortune, so a short
 * balance is left untouched and the caller must treat the refusal as fatal to
 * whatever transaction it was part of.
 *
 * @param balances  Array of CURRENCY_COUNT balances.
 * @return 1 after debiting, or 0 when the balance will not cover the amount.
 */
static inline int world_currency_debit(uint32_t* balances, int currency,
                                       uint32_t amount) {
    if (!balances || !world_currency_valid(currency)) return 0;
    if (balances[currency] < amount) return 0;

    balances[currency] -= amount;
    return 1;
}

/** Report the currency minted by the kingdom whose capital is nearest. */
static inline CurrencyId world_local_currency_px(float x, float y) {
    return world_nearest_city_px(x, y)->currency;
}

#endif // WORLD_REGIONS_H
