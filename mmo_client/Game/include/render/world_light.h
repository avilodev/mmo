#ifndef WORLD_LIGHT_H
#define WORLD_LIGHT_H

/**
 * @file
 * The one sun that lights the 3D world: structures bake it into their vertex
 * colours (structure_mesh.c) and characters apply it per pixel
 * (model_renderer.c), so a wall and the figure standing by it agree about
 * where the light comes from.
 *
 * Directions are GL world space: +X east, +Y up, +Z south (D1).
 */

#include <math.h>

/** Toward the sun: high, from the north-west, so south- and east-facing walls
 *  fall into shade and the default north-up view sees lit roofs. */
#define WORLD_SUN_X  -0.45f
#define WORLD_SUN_Y   0.80f
#define WORLD_SUN_Z  -0.40f

/** Brightness of a surface facing straight away from the sun. */
#define WORLD_AMBIENT 0.45f

/** Half-Lambert brightness of a surface with unit normal (nx, ny, nz),
 *  scaled so an upward-facing surface is exactly 1.0 -- the flat ground is
 *  drawn unlit, and a flat roof must match it. */
static inline float world_light(float nx, float ny, float nz) {
    const float len = sqrtf(WORLD_SUN_X * WORLD_SUN_X + WORLD_SUN_Y * WORLD_SUN_Y +
                            WORLD_SUN_Z * WORLD_SUN_Z);
    float d  = (nx * WORLD_SUN_X + ny * WORLD_SUN_Y + nz * WORLD_SUN_Z) / len;
    float up = WORLD_SUN_Y / len;
    float w  = d * 0.5f + 0.5f, wu = up * 0.5f + 0.5f;
    return (WORLD_AMBIENT + (1.0f - WORLD_AMBIENT) * w * w) /
           (WORLD_AMBIENT + (1.0f - WORLD_AMBIENT) * wu * wu);
}

/** The sky, and the fog that fades the world into it. A free camera can look
 * out toward the horizon, past the chunks streamed around the player (at
 * least LOAD_RADIUS_CHUNKS * 512 = 1536 units away); the fog closes before
 * that edge so it is never seen. Distances are on the ground, from the point
 * the camera orbits. */
#define WORLD_SKY_R 0.62f
#define WORLD_SKY_G 0.74f
#define WORLD_SKY_B 0.86f
#define WORLD_FOG_START 950.0f
#define WORLD_FOG_END   1450.0f

/** How far a ground offset (dx, dy) from the camera's centre is into the fog:
 *  0 is clear, 1 is sky. Matches the shaders. */
static inline float world_fog(float dx, float dy) {
    float d = sqrtf(dx * dx + dy * dy);
    float t = (d - WORLD_FOG_START) / (WORLD_FOG_END - WORLD_FOG_START);
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

#endif /* WORLD_LIGHT_H */
