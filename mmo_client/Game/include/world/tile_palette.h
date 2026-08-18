#ifndef TILE_PALETTE_H
#define TILE_PALETTE_H

#include <stdint.h>
#include <stddef.h>

/**
 * Identify a flat-colour tile. Worlds that declare zero tilesets store these
 * values directly in their tile layers instead of packed tileset coordinates.
 */
enum TilePaletteIndex {
    PAL_EMPTY = 0,

    PAL_DEEP_OCEAN,
    PAL_SHALLOW_WATER,
    PAL_TROPICAL_GRASS,
    PAL_TROPICAL_JUNGLE,
    PAL_BEACH_SAND,
    PAL_SNOW,
    PAL_ICE,
    PAL_SNOW_ROCK,
    PAL_DESERT_SAND,
    PAL_DESERT_DUNE,
    PAL_DESERT_ROCK,
    PAL_TUNDRA,
    PAL_TUNDRA_FROST,
    PAL_MOUNTAIN_ROCK,

    PAL_ROAD,
    PAL_PLAZA_STONE,
    PAL_CITY_WALL,
    PAL_BUILDING_WALL,
    PAL_BUILDING_ROOF,
    PAL_BUILDING_FLOOR,

    PAL_DIST_COURTYARD,
    PAL_DIST_HARBOR,
    PAL_DIST_MARKET,
    PAL_DIST_BLESSED,
    PAL_DIST_HUMAN,
    PAL_DIST_GUILD,

    PAL_COUNT
};

/** Map palette indices to RGB. Index 0 is unused padding so lookups are direct. */
static const float TILE_PALETTE[PAL_COUNT][3] = {
    { 0.00f, 0.00f, 0.00f },  /* PAL_EMPTY (never drawn) */

    { 0.106f, 0.227f, 0.361f },  /* PAL_DEEP_OCEAN      #1B3A5C */
    { 0.180f, 0.420f, 0.588f },  /* PAL_SHALLOW_WATER   #2E6B96 */
    { 0.243f, 0.545f, 0.243f },  /* PAL_TROPICAL_GRASS  #3E8B3E */
    { 0.165f, 0.373f, 0.165f },  /* PAL_TROPICAL_JUNGLE #2A5F2A */
    { 0.851f, 0.769f, 0.541f },  /* PAL_BEACH_SAND      #D9C48A */
    { 0.910f, 0.933f, 0.949f },  /* PAL_SNOW            #E8EEF2 */
    { 0.722f, 0.831f, 0.878f },  /* PAL_ICE             #B8D4E0 */
    { 0.478f, 0.522f, 0.565f },  /* PAL_SNOW_ROCK       #7A8590 */
    { 0.851f, 0.718f, 0.478f },  /* PAL_DESERT_SAND     #D9B77A */
    { 0.769f, 0.631f, 0.392f },  /* PAL_DESERT_DUNE     #C4A164 */
    { 0.627f, 0.502f, 0.314f },  /* PAL_DESERT_ROCK     #A08050 */
    { 0.561f, 0.639f, 0.549f },  /* PAL_TUNDRA          #8FA38C */
    { 0.769f, 0.831f, 0.784f },  /* PAL_TUNDRA_FROST    #C4D4C8 */
    { 0.431f, 0.431f, 0.447f },  /* PAL_MOUNTAIN_ROCK   #6E6E72 */

    { 0.541f, 0.451f, 0.333f },  /* PAL_ROAD            #8A7355 */
    { 0.690f, 0.659f, 0.600f },  /* PAL_PLAZA_STONE     #B0A899 */
    { 0.420f, 0.420f, 0.439f },  /* PAL_CITY_WALL       #6B6B70 */
    { 0.478f, 0.361f, 0.259f },  /* PAL_BUILDING_WALL   #7A5C42 */
    { 0.549f, 0.231f, 0.180f },  /* PAL_BUILDING_ROOF   #8C3B2E */
    { 0.788f, 0.663f, 0.494f },  /* PAL_BUILDING_FLOOR  #C9A97E */

    { 0.753f, 0.706f, 0.608f },  /* PAL_DIST_COURTYARD  #C0B49B */
    { 0.624f, 0.690f, 0.722f },  /* PAL_DIST_HARBOR     #9FB0B8 */
    { 0.788f, 0.635f, 0.420f },  /* PAL_DIST_MARKET     #C9A26B */
    { 0.725f, 0.659f, 0.769f },  /* PAL_DIST_BLESSED    #B9A8C4 */
    { 0.749f, 0.663f, 0.549f },  /* PAL_DIST_HUMAN      #BFA98C */
    { 0.659f, 0.639f, 0.561f },  /* PAL_DIST_GUILD      #A8A38F */
};

/**
 * Look up a palette colour.
 *
 * @param index  Palette index; 0 and out-of-range values have no colour.
 * @return       Pointer to three floats, or NULL when the index is not drawable.
 */
static inline const float* tile_palette_rgb(uint16_t index) {
    if (index == PAL_EMPTY || index >= PAL_COUNT) return NULL;
    return TILE_PALETTE[index];
}

#endif // TILE_PALETTE_H
