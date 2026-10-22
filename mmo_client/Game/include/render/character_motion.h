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

#include "render/character_procedural.h"

/** The clips a body chooses between. */
typedef enum {
    CHARACTER_ANIM_IDLE = 0,
    CHARACTER_ANIM_WALK,    /**< Walk mode. */
    CHARACTER_ANIM_RUN,     /**< Sprint mode: the sprint clip. */
    CHARACTER_ANIM_TALK,
    CHARACTER_ANIM_JOG,     /**< Run mode: the jog clip. */
	CHARACTER_ANIM_SIT_ENTER,   /**< Sitting down; plays once. Only as a pose. */
	CHARACTER_ANIM_SIT_IDLE,    /**< Seated; loops. Only as a pose. */
	CHARACTER_ANIM_SIT_EXIT,    /**< Getting up; plays once. Only as a pose. */
	CHARACTER_ANIM_DANCE,       /**< Dancing; loops. Only as a pose. */
	CHARACTER_ANIM_FIDGET,      // Idle variation; chosen by the body when idle
	CHARACTER_ANIM_PIVOT_LEFT,  // Turning on the spot to the left; loops
	CHARACTER_ANIM_PIVOT_RIGHT, // Turning on the spot to the right; loops
	CHARACTER_ANIM_CUSTOM,      // An emote's clip, picked at run time (CharacterHints.clip_custom); loops
	CHARACTER_ANIM_LEAN_ENTER,      // Leaning back on a wall; plays once. Only as a pose.
	CHARACTER_ANIM_LEAN_IDLE,       // Leaning; loops. Only as a pose.
	CHARACTER_ANIM_LEAN_EXIT,       // Pushing off the wall; plays once. Only as a pose.
	CHARACTER_ANIM_LEAN_FOOT_ENTER, // Leaning with a foot up on the wall; plays once. Only as a pose.
	CHARACTER_ANIM_LEAN_FOOT_IDLE,  // Leaning, foot up; loops. Only as a pose.
	CHARACTER_ANIM_LEAN_FOOT_EXIT,  // Foot down, pushing off; plays once. Only as a pose.
	CHARACTER_ANIM_FIDGET_GLANCE,   // Idle variation: a look back over the shoulder
	CHARACTER_ANIM_FIDGET_KNUCKLES, // Idle variation: cracking the knuckles
	CHARACTER_ANIM_FIDGET_SLEEVE,   // Idle variation: brushing off a sleeve
	CHARACTER_ANIM_CUSTOM_ENTER,    // A clip emote's intro, picked like CUSTOM; plays once. Only as a pose.
	CHARACTER_ANIM_CUSTOM_EXIT,     // A clip emote's exit, picked like CUSTOM; plays once. Only as a pose.
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
    int   pivoting;         /**< Turning on the spot: feet planted. */
	int   has_pose;         /**< Play `pose`, whatever the speed. The body is
                             *   held (a seat), not walking: its position
                             *   changes are not movement. */
	CharacterAnim pose;
	int   has_aim;          // The upper body faces (aim_x, aim_y); legs unchanged
	float aim_x, aim_y;
	int   airborne;         // Off the ground (a jump or fall): no foot IK
	int   still_idle;       // Standing about holds the first frame of idle: no breathing, no fidget
	int   has_speed;        // The ground speed is known (a remote player's, from the server)
	float speed;            // That speed, world units per second: used as is, not measured
	int   has_heading;      // The facing is known and reported (a remote player's): turn to it
	float heading;          // That facing, radians
	int   has_idle_state;   // The owner says when to play the idle variation: no timer of its own
	int   idle_fidget;      // 0 standing; else 1 + the variation the owner plays (CharacterMotion.fidget_kind)
	// Clips picked in the emote hub: an index in the emote clip table plus one, 0 for
	// the default. A clip plays in its slot's place; CUSTOM has no default clip.
	unsigned char clip_walk;
	unsigned char clip_jog;
	unsigned char clip_sit;
	unsigned char clip_custom;
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

	character_procedural proc;   // Lean, twist and head turn on top of the clip

	float idle_time;             // Seconds standing about since the last fidget
	float fidget_after;          // How long to stand before the next one
	float fidget_time;           // Seconds into the fidget being played
	int   fidgeting;             // The idle variation is playing
	int   fidget_kind;           // Which one: an index in the variation table (character_motion.c), the number sent; -1 none yet
	unsigned int rng;            // This body's own random stream
	int   pivot_dir;             // Which way the pivot turns: 1 left, -1 right, 0 not known
	int   remote_pivoting;       // Turning on the spot to a reported facing
	int   heading_known;         // A reported facing has been taken once: the first is not a turn
	int   started;               // The first update has run: a new body starts in the clip it wants
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
