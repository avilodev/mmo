#ifndef HEIGHT_SMOOTH_H
#define HEIGHT_SMOOTH_H

/**
 * @file
 * Ease a drawn height toward the true one, so a body climbs a stair or a ramp
 * smoothly instead of in steps, and snaps across a fall or a teleport.
 */

/** Move `current` toward `target` at 14 per second of dt. Snaps to the target
 *  when the gap exceeds 48 units; never overshoots; returns `target` for a
 *  non-finite `dt`, `current` or `target`. */
float height_smooth_step(float current, float target, float dt);

#endif /* HEIGHT_SMOOTH_H */
