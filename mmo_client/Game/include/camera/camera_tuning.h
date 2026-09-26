#ifndef CAMERA_TUNING_H
#define CAMERA_TUNING_H

/**
 * @file
 * Every number that sets how the 3D camera feels, in one place.
 *
 * See Next_steps/3d_refactor.md: the decisions these implement are D5-D10 and
 * D13. Change them here, not at the call sites.
 */

/** Tilt below the horizon, fixed (D6). 90 would be straight down. */
#define CAMERA_PITCH_DEG 55.0f

/** Vertical field of view (D5). Narrow keeps the League-like low distortion. */
#define CAMERA_FOVY_DEG 40.0f

/** Zoom limits (D9). Zoom is expressed as the pixel scale at the player, so
 *  1.0 is the same scale the top-down camera had at its furthest. Distance is
 *  derived from it by camera_math_distance_for_zoom().
 *
 *  The minimum is 1.05, not 1.0: tilted and turned 45 degrees, the far corners
 *  of a 1920x1080 view at zoom 1.0 reach ~1550 units from the player, just past
 *  the three chunks streamed around them (D15; camera_math_test TEST 7). */
#define CAMERA_ZOOM_MIN 1.05f
#define CAMERA_ZOOM_MAX 3.5f

/** Where the camera starts and where Home returns it: closer than the minimum,
 *  so the player reads as a figure in a place rather than a dot on a map.
 *  Scrolling out still reaches CAMERA_ZOOM_MIN. */
#define CAMERA_ZOOM_DEFAULT 1.6f

/** Scroll step, as a fraction of the current zoom per wheel notch. */
#define CAMERA_ZOOM_STEP 0.1f

/** Q/E rotation speed while held (D8). */
#define CAMERA_ROTATE_DEG_PER_SEC 120.0f

/** Middle-mouse drag rotation, per logical pixel of horizontal movement (D8). */
#define CAMERA_DRAG_DEG_PER_PIXEL 0.25f

/** Default yaw: north up, the same framing as the top-down view (D7). */
#define CAMERA_YAW_DEFAULT_DEG 0.0f

/** Roof fade around the player while indoors (D13): fully hidden inside the
 *  inner radius, fully drawn beyond the outer one, in world units. */
#define CAMERA_ROOF_FADE_INNER 96.0f
#define CAMERA_ROOF_FADE_OUTER 192.0f

/** How far above the ground a damage number starts, in world units: the middle
 *  of a standard two-tile body, where the top-down view started it. */
#define CAMERA_DAMAGE_TEXT_RISE 16.0f

/** How high a projectile flies above the ground, in world units. */
#define CAMERA_PROJECTILE_HEIGHT 16.0f

#endif /* CAMERA_TUNING_H */
