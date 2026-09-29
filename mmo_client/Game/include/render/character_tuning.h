#ifndef CHARACTER_TUNING_H
#define CHARACTER_TUNING_H

/**
 * @file
 * Every number that sets how the 3D characters look and move, in one place.
 *
 * Players and NPCs all wear one rigged mannequin (Quaternius Universal
 * Animation Library, CC0), tinted per race or NPC style. See
 * Next_steps/3d_refactor.md, Phase 2.
 */

/** The rig every character wears, relative to the client's working directory. */
#define CHARACTER_MODEL_PATH "Game/assets/models/characters/UAL1_Standard.glb"

/** Standing height in world units (D3: ~2.5 tiles of 16). The model is scaled
 *  from its bind-pose height to this. */
#define CHARACTER_HEIGHT 40.0f

/** How far from a character's upright centre line a click still hits it,
 *  world units. Generous: the body is ~11 wide, and a target that is hard to
 *  click is worse than one that is easy to click near. */
#define CHARACTER_PICK_RADIUS 16.0f

/** The blob shadow under every body, world units at scale 1. */
#define CHARACTER_SHADOW_RADIUS 12.0f
#define CHARACTER_SHADOW_ALPHA  0.32f

/** Clip names in the model file. model_test.c fails if one goes missing. */
#define CHARACTER_CLIP_IDLE  "Idle_Loop"
#define CHARACTER_CLIP_WALK  "Walk_Loop"
#define CHARACTER_CLIP_JOG   "Jog_Fwd_Loop"
#define CHARACTER_CLIP_RUN   "Sprint_Loop"
#define CHARACTER_CLIP_TALK  "Idle_Talking_Loop"
#define CHARACTER_CLIP_DEATH "Death01"

/** Ground speed, in world units per second, at which each clip's feet match
 *  the ground. Measured from the root motion of the same clips in
 *  UAL1_Standard_RM.glb (Walk_Loop 0.975 m/s, Jog_Fwd_Loop 5.36 m/s,
 *  Sprint_Loop 8.25 m/s) at the
 *  model's scale (CHARACTER_HEIGHT / 1.83 m = 21.9 units per metre). Playback
 *  rate is actual speed over this, clamped. */
#define CHARACTER_WALK_CLIP_SPEED 21.3f
#define CHARACTER_JOG_CLIP_SPEED  117.0f
#define CHARACTER_RUN_CLIP_SPEED  180.0f
#define CHARACTER_CLIP_RATE_MIN   0.6f
#define CHARACTER_CLIP_RATE_MAX   1.6f

/** Speed thresholds (world units per second) for choosing the clip. */
#define CHARACTER_MOVE_THRESHOLD 8.0f    /**< Below: standing. */
#define CHARACTER_JOG_THRESHOLD  100.0f  /**< Above: the jog clip. Walk mode is
                                          *  ~64, run mode ~150 (locomotion.h). */
#define CHARACTER_RUN_THRESHOLD  175.0f  /**< Above: the sprint clip (sprint
                                          *  mode, 200). */

/** How quickly the measured speed follows the real one (per second). Positions
 *  arrive in 20 Hz steps for other entities, so the raw per-frame speed
 *  flickers between zero and double. */
#define CHARACTER_SPEED_SMOOTHING 10.0f

/** Cross-fade between clips, seconds. */
#define CHARACTER_BLEND_TIME 0.2f

/** Turn rate toward the direction of travel, degrees per second (D23). */
#define CHARACTER_TURN_DEG_PER_SEC 720.0f

/** A standing quest NPC turns to face the local player inside this range. */
#define CHARACTER_NOTICE_RADIUS 160.0f

/** Bodies further than this from the camera target re-skin at a reduced rate:
 *  the CPU cost of skinning is per body per frame, and nobody reads the
 *  animation of a figure at the far edge of the view. */
#define CHARACTER_FULL_RATE_RADIUS 700.0f
#define CHARACTER_FAR_SKIN_HZ      15.0f

/** How many bodies can be tracked at once, and how many frames a body can go
 *  unseen before its slot is reclaimed. */
#define CHARACTER_MAX_BODIES      160
#define CHARACTER_FORGET_FRAMES   120

#endif /* CHARACTER_TUNING_H */
