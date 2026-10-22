#ifndef WORLD_MOTION_H
#define WORLD_MOTION_H

// Shared vertical movement: standing on surfaces, jumping and falling.
// Identical in both trees. The client predicts what the server validates.

#include <math.h>

#define WORLD_GRAVITY            420.0f
#define WORLD_JUMP_SPEED         160.0f
#define WORLD_TERMINAL_SPEED     900.0f
#define WORLD_VSTEP              (1.0f / 60.0f)
#define WORLD_AIR_SLACK_STEPS    5
#define WORLD_AIR_Z_TOLERANCE    8.0f
#define WORLD_VREACH             48.0f

typedef struct {
	float z;
	float vz;
	int grounded;
} vertical_state;

// Advance one fixed step with the body box now centred at (x, y). jump starts
// a jump when grounded. Returns 1 when the state advanced, and 0 when that
// position is blocked, in which case *s is left unchanged.
int vertical_step(vertical_state* s, float x, float y, float half, int jump);

// vertical_step, then unstick: advance bodies with it, probe moves with the step
int vertical_advance(vertical_state* s, float x, float y, float half, int jump);

// Stand an airborne body stuck inside solid on the floor below; 1 if it moved
int vertical_unstick(vertical_state* s, float x, float y, float half);

// Is something at b_z close enough in height to act on, from a_z?
static inline int world_vertical_ok(float a_z, float b_z) {
	return fabsf(a_z - b_z) <= WORLD_VREACH;
}

#endif /* WORLD_MOTION_H */
