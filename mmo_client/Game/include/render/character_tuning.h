#ifndef CHARACTER_TUNING_H
#define CHARACTER_TUNING_H

/**
 * @file
 * Every number that sets how the 3D characters look and move, in one place.
 *
 * NPCs wear a rigged mannequin (Quaternius Universal Animation Library, CC0),
 * tinted per NPC style; players wear their own male or female model below. See
 * Next_steps/3d_refactor.md, Phase 2.
 */

/** The character kit: both bodies and every race's parts on one rig, relative to
 *  the client's working directory. render/character_look.h says how a body picks
 *  its meshes from it. */
#define CHARACTER_MODEL_PATH "Game/assets/models/characters/work/Character_Kit.pre_vrm.glb"

// The players' own models: a mesh and a skeleton, with no clips. They share the
// kit's bone names, so the kit's clips play on them (render/character_retarget.h).
// A player wears the male one or the female one by the body in their look; the
// kit's mannequin stays the NPCs' body, and a player's if one of these is missing.
#define CHARACTER_MODEL_MALE_PATH   "Game/assets/models/characters/player_male.glb"
#define CHARACTER_MODEL_FEMALE_PATH "Game/assets/models/characters/player_female.glb"

// A forearm rolls this many degrees before its twist bones start to give way
#define CHARACTER_TWIST_DEADZONE_DEG 30.0f
// Of the roll beyond that, the share each twist bone takes back (near the elbow, near the wrist)
#define CHARACTER_TWIST_01_COUNTER   1.0f
#define CHARACTER_TWIST_02_COUNTER   0.5f

// 1 draws the kit's ears and tail on a body, 0 hides them
#define CHARACTER_SHOW_EARS_TAIL 0

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
#define CHARACTER_CLIP_SIT_ENTER "Sitting_Enter"
#define CHARACTER_CLIP_SIT_IDLE  "Sitting_Idle_Loop"
#define CHARACTER_CLIP_SIT_EXIT  "Sitting_Exit"
#define CHARACTER_CLIP_DANCE     "Dance_Loop"
#define CHARACTER_CLIP_FIDGET    "Idle_FoldArms_Loop"
// More idle variations, picked at random with the one above (authored in work/lean)
#define CHARACTER_CLIP_FIDGET_GLANCE   "Idle_GlanceBack"
#define CHARACTER_CLIP_FIDGET_KNUCKLES "Idle_KnuckleCrack"
#define CHARACTER_CLIP_FIDGET_SLEEVE   "Idle_SleeveBrush"
#define CHARACTER_CLIP_PIVOT_LEFT  "Pivot_Left_Loop"
#define CHARACTER_CLIP_PIVOT_RIGHT "Pivot_Right_Loop"
// The lean emotes (authored in assets/models/characters/work/lean)
#define CHARACTER_CLIP_LEAN_ENTER      "Lean_Enter"
#define CHARACTER_CLIP_LEAN_IDLE       "Lean_Idle_Loop"
#define CHARACTER_CLIP_LEAN_EXIT       "Lean_Exit"
#define CHARACTER_CLIP_LEAN_FOOT_ENTER "LeanFoot_Enter"
#define CHARACTER_CLIP_LEAN_FOOT_IDLE  "LeanFoot_Idle_Loop"
#define CHARACTER_CLIP_LEAN_FOOT_EXIT  "LeanFoot_Exit"

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
                                          *  ~30, run mode ~150 (locomotion.h). */
#define CHARACTER_RUN_THRESHOLD  175.0f  /**< Above: the sprint clip (sprint
                                          *  mode, 200). */

/** How quickly the measured speed follows the real one (per second). Positions
 *  arrive in 20 Hz steps for other entities, so the raw per-frame speed
 *  flickers between zero and double. */
#define CHARACTER_SPEED_SMOOTHING 10.0f

/** A pivot picks its clip from which way the facing turned; a step smaller
 *  than this, radians in one frame, says nothing about the direction. */
#define CHARACTER_PIVOT_DIR_EPSILON 1.0e-4f

/** How another player's body turns to the facing the server reports. These
 *  mirror the local player's own pivot (locomotion.h) so both look the same. */
#define CHARACTER_PIVOT_START_DEG 45.0f  /**< Standing: turn this far or more to pivot. */
#define CHARACTER_PIVOT_END_DEG   20.0f  /**< The pivot ends within this of the facing. */
#define CHARACTER_PIVOT_SPEED     40.0f  /**< At or below this speed the body can pivot. */
#define CHARACTER_PIVOT_DEG_PER_SEC 540.0f

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

// A body standing about plays an idle variation after this long, in seconds
#define CHARACTER_FIDGET_MIN_SEC       8.0f
#define CHARACTER_FIDGET_MAX_SEC       12.0f
// How many times the arms-folded variation's clip plays before the body settles again;
// the others are one gesture each and play once
#define CHARACTER_FIDGET_CYCLES        2

// Procedural body layer (character_procedural.h). Angles are degrees.
// How fast the lean, twist and head offsets chase their targets, per second
#define CHARACTER_PROC_FOLLOW          14.0f
// Acceleration is low-passed before it drives the lean (20 Hz steps flutter)
#define CHARACTER_PROC_ACCEL_FOLLOW    6.0f
// A yaw change faster than this is a snap, not a turn; ignore the excess
#define CHARACTER_PROC_MAX_YAW_RATE_DEG 1000.0f

// Forward lean at the run clip's speed, and the most it may tip either way
#define CHARACTER_LEAN_FWD_AT_RUN_DEG  4.7f
#define CHARACTER_LEAN_FWD_MAX_DEG     9.3f
#define CHARACTER_LEAN_BACK_MAX_DEG    6.7f
// Extra lean per 1000 units/s^2 of forward acceleration (negative: braking)
#define CHARACTER_LEAN_ACCEL_DEG_PER_K 6.0f
// Lean into a turn per 1000 units/s^2 of sideways acceleration, and its limit
#define CHARACTER_LEAN_SIDE_DEG_PER_K  6.7f
#define CHARACTER_LEAN_SIDE_MAX_DEG    8.0f

// The upper body leads a turn by this many seconds of the turn rate
#define CHARACTER_TWIST_LEAD_SEC       0.07f
#define CHARACTER_TWIST_MAX_DEG        40.0f
// The head leads further: its own lead in seconds, and its limit past the spine
#define CHARACTER_HEAD_LEAD_SEC        0.12f
#define CHARACTER_HEAD_MAX_DEG         65.0f
// Standing and looking at something: the share of the turn the spine takes
#define CHARACTER_LOOK_TWIST_SHARE     0.3f
// The upper body's reach toward an aim point; the head takes the rest
#define CHARACTER_AIM_TWIST_MAX_DEG    50.0f

// Rig bones the layer moves
#define CHARACTER_BONE_SPINE_1 "spine_01"
#define CHARACTER_BONE_SPINE_2 "spine_02"
#define CHARACTER_BONE_SPINE_3 "spine_03"
#define CHARACTER_BONE_NECK    "neck_01"
#define CHARACTER_BONE_HEAD    "Head"
// How lean and twist spread over the three spine bones (they sum to 1)
#define CHARACTER_SPINE_SHARE_1        0.25f
#define CHARACTER_SPINE_SHARE_2        0.35f
#define CHARACTER_SPINE_SHARE_3        0.40f
// How the head's own yaw splits between neck and head
#define CHARACTER_NECK_YAW_SHARE       0.35f

// Rig bones the foot IK moves, left then right
#define CHARACTER_BONE_THIGH_L "thigh_l"
#define CHARACTER_BONE_THIGH_R "thigh_r"
#define CHARACTER_BONE_CALF_L  "calf_l"
#define CHARACTER_BONE_CALF_R  "calf_r"
#define CHARACTER_BONE_FOOT_L  "foot_l"
#define CHARACTER_BONE_FOOT_R  "foot_r"

// 1: hair keeps out of the body, braids droop and lie on the back, and a free end has a speed
// limit. 0: the first version, hair that only swings (the code stays, switched off)
#define CHARACTER_HAIR_COLLIDE 0

// Hair that swings (character_spring.h); the hair bones start with this name
#define CHARACTER_HAIR_PREFIX    "J_Sec_Hair"
// Pull of a bone back to place, world units per second squared per bone length
#define CHARACTER_HAIR_STIFFNESS 1500.0f
// How fast a tip's speed relative to the head dies, per second
#define CHARACTER_HAIR_DRAG      28.0f
// Downward pull on the tips, world units per second squared
#define CHARACTER_HAIR_GRAVITY   60.0f

// A hair chain this long (metres) is a braid: it droops, to lie on the back
#define CHARACTER_HAIR_DRAPE_LENGTH    0.3f
// A braid's softer pull to the made pose and fuller gravity (units as above)
#define CHARACTER_HAIR_DRAPE_STIFFNESS 60.0f
#define CHARACTER_HAIR_DRAPE_GRAVITY   250.0f
// How hard a braid is drawn onto the body it hangs on, world units per second^2
#define CHARACTER_HAIR_DRAPE_PRESS     120.0f
// A braid is drawn to the body from this close (metres off its surface)
#define CHARACTER_HAIR_DRAPE_REACH     0.12f

// The tail swings the same way, softer and slower: its bones are long and heavy
#define CHARACTER_TAIL_PREFIX    "J_Opt_C_FoxTail"
#define CHARACTER_TAIL_STIFFNESS 3000.0f
#define CHARACTER_TAIL_DRAG      30.0f
#define CHARACTER_TAIL_GRAVITY   60.0f

// 1: the weather's wind leans the hair and tail (world/wind_gust.h). 0: off
#define CHARACTER_WIND 1
// How much of the lean shivers, and how much of the hair's lean the tail takes
#define CHARACTER_WIND_FLUTTER    0.25f
#define CHARACTER_TAIL_WIND_SHARE 0.5f

// Past this far from the camera focus (world units), hair ignores the body
#define CHARACTER_COLLIDE_RADIUS 250.0f
// The fastest a free end moves against its head, rig units (metres) a second
#define CHARACTER_SPRING_MAX_SPEED 3.0f

// The bones whose skin swinging bones stay out of; spheres go under it at load
#define CHARACTER_COLLIDER_BONES { "Head", "neck_01", "spine_03", "spine_02", "spine_01", \
	"clavicle_l", "clavicle_r", "upperarm_l", "upperarm_r", "lowerarm_l", "lowerarm_r", \
	"pelvis", "thigh_l", "thigh_r", "J_Opt_C_FoxTail1_01", "J_Opt_C_FoxTail2_01", \
	"J_Opt_C_FoxTail3_01", "J_Opt_C_FoxTail4_01", "J_Opt_C_FoxTail5_01", \
	"J_Opt_C_FoxTail5_end_01" }
// Sphere size and spacing over the skin, in the rig's units (metres)
#define CHARACTER_COLLIDER_RADIUS     0.04f
#define CHARACTER_COLLIDER_SPACING    0.04f
// Skin moved by bones with this prefix (ears, tail) is not kept out of
#define CHARACTER_COLLIDER_SKIP_PREFIX "J_Opt_"
// Skin moved by bones with this prefix (bust, coat) goes with the bone above
#define CHARACTER_COLLIDER_FOLLOW_PREFIX "J_Sec_"
// How far a sphere stands out past the skin, to leave room for a strand
#define CHARACTER_COLLIDER_MARGIN     0.004f
// A strand is held off the body by its own thickness, up to this much (metres)
#define CHARACTER_HAIR_CLEARANCE      0.03f

// Foot IK on stairs and slopes (character_feet.h); lengths are world units
// The most the body sinks to meet lower ground, and rises to meet higher
#define FOOT_IK_MAX_DROP        12.0f
#define FOOT_IK_MAX_RISE        12.0f
// Follow rates per second: the pelvis, each foot's ground, the whole effect
#define FOOT_IK_PELVIS_FOLLOW   18.0f
#define FOOT_IK_FOOT_FOLLOW     30.0f
#define FOOT_IK_WEIGHT_FOLLOW   8.0f
// A leg is never stretched past this share of its length
#define FOOT_IK_MAX_REACH       0.99f
// A foot off flat ground by PLANT or less is planted, FREE or more swinging
#define FOOT_IK_PLANT_LIFT      1.0f
#define FOOT_IK_FREE_LIFT       4.0f
// A swinging foot looks this far ahead for a riser to clear
#define FOOT_IK_AHEAD           6.0f
// The surface slope is read this far either side of a foot
#define FOOT_IK_SLOPE_REACH     2.0f
// A planted foot tilts to the surface by at most this
#define FOOT_IK_MAX_TILT_DEG    35.0f
// A body this high off its floor (a hop) gets no foot IK
#define FOOT_IK_AIRBORNE_LIFT   1.0f
// Foot IK fades out over this distance short of the full-rate radius
#define FOOT_IK_FADE_BAND       150.0f

#endif /* CHARACTER_TUNING_H */
