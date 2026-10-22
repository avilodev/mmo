#ifndef CHARACTER_PROCEDURAL_H
#define CHARACTER_PROCEDURAL_H

// Procedural body layer: lean, upper-body twist, head turn (no GL, no rig)

// Offsets in radians, in the body's own frame (before the yaw is applied).
typedef struct {
	float lean_forward;   // + tips the head forward
	float lean_left;      // + tips the head to the character's left
	float spine_twist;    // + turns the upper body toward increasing yaw
	float head_yaw;       // like spine_twist, added on top of it
} character_offsets;

// One body's layer state. Zero-initialized is "not yet primed, no offsets".
typedef struct {
	character_offsets now;   // the smoothed offsets to apply
	float last_yaw;
	float last_speed;
	float accel;             // forward acceleration, low-passed
	int primed;              // last_yaw and last_speed are from the last frame
} character_procedural;

typedef struct {
	float yaw;         // facing as drawn, radians
	float speed;       // smoothed ground speed, world units per second
	int held;          // a pose or a teleport holds the body: no offsets
	int has_aim;       // the upper body faces aim_yaw
	float aim_yaw;     // world heading toward the aim point
	int has_look;      // the head turns toward look_yaw (standing bodies)
	float look_yaw;
} character_procedural_input;

// Start a body with no offsets, facing `yaw`.
void character_procedural_reset(character_procedural* p, float yaw);

// Advance one frame. dt must be positive.
void character_procedural_update(character_procedural* p,
                                 const character_procedural_input* in,
                                 float dt);

#endif /* CHARACTER_PROCEDURAL_H */
