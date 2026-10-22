#ifndef CHARACTER_FEET_H
#define CHARACTER_FEET_H

// Where the feet stand and how far the body sinks or rises, smoothed per body

#include "render/character_legs.h"

// One body's smoothing state. Zero-initialized is "not started".
typedef struct {
	float pelvis;       // how far the body is lifted, in rig units
	float raise[2];     // each foot's ground offset, in rig units
	float weight;       // 0..1, how much of the effect is showing
	int primed;         // pelvis and raise follow the ground from last frame
	int started;        // the first frame has been seen: a new body shows its full effect
} character_feet;

typedef struct {
	float x, y;         // world position of the body
	float floor_z;      // the height the body is drawn standing on
	float yaw;          // facing as drawn, radians
	float scale;        // world units per rig unit
	float min_y;        // the rig's floor in its own frame
	float want;         // 0..1: how much foot IK this body should get
	float rest_ankle[2];   // ankle height above the floor in the bind pose
} character_feet_input;

typedef struct {
	float lift;         // world units to raise the whole drawn body
	float raise[2];     // each foot target above the clip's, for the solver
	float tilt[2][4];   // each foot's tilt to the surface
	int active;         // false: change nothing about the pose or the body
} character_feet_output;

// How much foot IK a body gets, 0..1: none held, airborne or without a map
float character_feet_want(int usable, int held, int airborne, float lift,
                          float distance);

// Advance one frame. `clip` is the pose's legs before any foot IK.
void character_feet_update(character_feet* st, const character_legs_clip* clip,
                           const character_feet_input* in, float dt,
                           character_feet_output* out);

#endif /* CHARACTER_FEET_H */
