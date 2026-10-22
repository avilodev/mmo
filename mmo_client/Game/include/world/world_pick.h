#ifndef WORLD_PICK_H
#define WORLD_PICK_H

/**
 * @file
 * Pick a point on the world's surface under a ray, using the same spans the
 * body walks on, so a click lands on a ramp or a bridge, not under it.
 */

/** March a ray in world coordinates (x, y, z up) until it enters solid.
 *
 * Marches in 2-unit steps and bisects (8 iterations) between the last free
 * point and the first solid one; `out` is that surface point.
 *
 * @return 0 when the origin is inside solid, the ray leaves the world, or it
 *         travels `max_dist` without touching anything; 1 otherwise. */
int world_pick_ground(const float origin[3], const float dir[3], float max_dist,
                      float out[3]);

#endif /* WORLD_PICK_H */
