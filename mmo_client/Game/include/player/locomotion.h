#ifndef LOCOMOTION_H
#define LOCOMOTION_H

/**
 * @file
 * How the local player's body moves: GTA-style locomotion.
 *
 * The keys say where the player wants to go; the body gets there the way a
 * person does, at the pace of its movement mode -- walk, run (the default)
 * or sprint. It speeds up and slows down rather than snapping to speed. It always moves the
 * way it faces, and turns toward the keys at a rate, so changing direction
 * curves instead of sliding sideways. Asked to go the other way it does not
 * moonwalk: from a standstill it pivots on the spot first, and at a run it
 * skids to a stop, turns, and sets off again.
 *
 * Pure arithmetic with no GL, input or world, so locomotion_test.c pins it.
 * player.c feeds it the camera-relative key direction and resolves collision
 * on what it returns. The server only ever sees positions no faster than
 * max_speed, so its move validation is unaffected.
 */

/** How fast the body means to go. The player toggles walk and run (Ctrl);
 *  sprint has no key yet. */
typedef enum {
    LOCO_MODE_WALK = 0,
    LOCO_MODE_RUN,
    LOCO_MODE_SPRINT,
    LOCO_MODE_COUNT
} LocoMode;

/** Each mode's speed as a fraction of the character's top speed -- the
 *  server's figure, which nothing may exceed. Sprint is the top speed, so run
 *  sits below it. At the default 200: walk 64, run 150, sprint 200. */
#define LOCO_WALK_FRACTION   0.32f
#define LOCO_RUN_FRACTION    0.75f
#define LOCO_SPRINT_FRACTION 1.00f

/** Ground acceleration and braking, world units per second squared. Braking is
 *  harder than speeding up, so a stop reads as a stop. */
#define LOCO_ACCEL           900.0f
#define LOCO_DECEL           1400.0f

/** Turn rates toward the keys, degrees per second. Slower at a run, so a
 *  running turn is a curve. */
#define LOCO_WALK_TURN_DEG   540.0f
#define LOCO_RUN_TURN_DEG    360.0f

/** Turning on the spot: started when the keys point more than PIVOT_START
 *  away while (nearly) standing, ended once within PIVOT_END. */
#define LOCO_PIVOT_TURN_DEG  900.0f
#define LOCO_PIVOT_START_DEG 100.0f
#define LOCO_PIVOT_END_DEG   20.0f
#define LOCO_PIVOT_SPEED     40.0f   /**< At or below this the body can pivot. */

/** One body's locomotion. Zero-initialized is standing still facing south. */
typedef struct {
    float heading;    /**< Facing, radians: world direction (sin, cos). */
    float speed;      /**< Current ground speed, world units per second. */
    int   pivoting;   /**< Turning on the spot. */
} Locomotion;

/** Advance one frame.
 *
 * @param in_x, in_y  Wanted direction in world space; any length (only the
 *                    direction is used), (0, 0) for no input.
 * @param mode        Walk, run or sprint.
 * @param max_speed   The character's top speed (the server's figure).
 * @param out_dx, out_dy  Displacement this frame, before collision.
 */
void locomotion_step(Locomotion* l, float in_x, float in_y, LocoMode mode,
                     float max_speed, float dt, float* out_dx, float* out_dy);

/** A mode's target speed for a character whose top speed is `max_speed`. */
float locomotion_mode_speed(LocoMode mode, float max_speed);

/** Stop dead where the body stands (a wall, a teleport). Keeps the heading. */
void locomotion_halt(Locomotion* l);

#endif /* LOCOMOTION_H */
