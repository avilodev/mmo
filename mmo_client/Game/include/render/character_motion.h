#ifndef CHARACTER_MOTION_H
#define CHARACTER_MOTION_H

/**
 * @file
 * Decide what a character's body is doing from where it has been drawn.
 *
 * The server sends positions, not animation states, so every body -- the local
 * player, other players, NPCs -- is animated the same way: measure how fast
 * its drawn position moves, pick idle/walk/run from that, face the way it is
 * going (D23), and cross-fade when the choice changes.
 *
 * Pure arithmetic with no GL, so character_motion_test.c pins it headless.
 */

/** The clips a body chooses between. */
typedef enum {
    CHARACTER_ANIM_IDLE = 0,
    CHARACTER_ANIM_WALK,    /**< Walk mode. */
    CHARACTER_ANIM_RUN,     /**< Sprint mode: the sprint clip. */
    CHARACTER_ANIM_TALK,
    CHARACTER_ANIM_JOG,     /**< Run mode: the jog clip. */
    CHARACTER_ANIM_COUNT
} CharacterAnim;

/** What the body knows beyond its own position. Zero-initialized is "nothing". */
typedef struct {
    int   talking;          /**< Standing in conversation: play the talk clip. */
    int   has_look;         /**< When standing, turn to face (look_x, look_y). */
    float look_x, look_y;
    int   has_facing;       /**< The facing is known (the local player's
                             *   locomotion): use facing_yaw as is. */
    float facing_yaw;
    int   pivoting;         /**< Turning on the spot: step, do not stand. */
} CharacterHints;

/** One body's animation state. */
typedef struct {
    float last_x, last_y;   /**< Drawn position last update. */
    float speed;            /**< Smoothed ground speed, world units per second. */
    float yaw;              /**< Facing, radians: world direction (sin yaw, cos yaw). */

    CharacterAnim anim;     /**< Clip being faded in. */
    CharacterAnim prev;     /**< Clip being faded out. */
    float anim_time;        /**< Seconds into `anim`, wrapped to its duration. */
    float prev_time;
    float blend;            /**< 0 shows `prev`, 1 shows `anim`. */
} CharacterMotion;

/** Start a body standing at a point, facing `yaw`. */
void character_motion_reset(CharacterMotion* m, float x, float y, float yaw);

/** Advance one frame.
 *
 * @param durations  Length of each CharacterAnim's clip in seconds; a
 *                   non-positive entry never wraps.
 */
void character_motion_update(CharacterMotion* m, float x, float y, float dt,
                             const CharacterHints* hints,
                             const float durations[CHARACTER_ANIM_COUNT]);

/** Playback rate for a clip at a ground speed, so feet roughly match it. */
float character_motion_clip_rate(CharacterAnim anim, float speed);

#endif /* CHARACTER_MOTION_H */
