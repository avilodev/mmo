#ifndef CHARACTER_SPRING_H
#define CHARACTER_SPRING_H

// Spring bones: hair that trails the body's motion and settles back (no GL)

#include "model/mat4.h"
#include "model/skinned_model.h"

// The most joints one rig holds; the players' hair has 39 and 40
#define SPRING_MAX_JOINTS 96

// The most colliders one rig holds
#define SPRING_MAX_COLLIDERS 1024

// A sphere riding a bone of the body, laid just under the skin
typedef struct {
	int node;           // the bone it rides
	float center[3];    // in that bone's frame
	float normal[3];    // the way out of the body there, in that bone's frame
	float radius;
	int blocks_only;    // nonzero: it keeps a strand out but does not draw one in (the tail)
} character_spring_collider;

// The most bones' worth of colliders one rig holds
#define SPRING_MAX_CLUSTERS 32

// The colliders on one bone and a sphere around them, so far strands skip them
typedef struct {
	int node;
	int first;          // the index of its first collider; a bone's colliders are together
	int count;
	float center[3];    // in the bone's frame
	float radius;
} character_spring_cluster;

// A bone that swings, with the bone hanging off it that is its free end
typedef struct {
	int node;      // the bone that is turned
	int tip_node;  // the bone whose joint is this one's free end
	int parent;    // the joint above this one, or -1 when a plain bone holds it
	float chain;   // the length of the whole chain of joints it is part of, metres
	float thickness; // how far its skin stands off the bone on average, metres
} character_spring_joint;

// The joints of one group of bones, parents before children
typedef struct {
	character_spring_joint joint[SPRING_MAX_JOINTS];
	int count;
	character_spring_collider collider[SPRING_MAX_COLLIDERS];
	int collider_count;
	character_spring_cluster cluster[SPRING_MAX_CLUSTERS];
	int cluster_count;
	// How much of collider k a tip keeps out of, of 255; less where it rests in it
	unsigned char collider_reach[SPRING_MAX_JOINTS][SPRING_MAX_COLLIDERS];
} character_spring_rig;

typedef struct {
	float stiffness;  // pull back to the animated pose, world units per second squared
	float drag;       // how fast the tip's speed relative to the head dies, per second
	float gravity;    // world units per second squared, downward
	// Chains at least `drape_length` (metres; 0 for none) use these two instead
	float drape_length;
	float drape_stiffness;
	float drape_gravity;
	float drape_press;  // and are drawn onto the body they hang against, units as gravity
	float drape_reach;  // from this far off its surface, metres
	int skip_colliders; // nonzero: leave the body out of it (a figure too far off to see)
	float max_speed;    // how fast an end may move against its head, metres a second; 0 is the
	                    // default limit and below 0 is none
	float wind[3];       // how far the wind leans an end: a share of bone length, world axes
	float wind_flutter;  // the share of that lean which shivers, joint by joint
	float wind_time;     // shared seconds, for the shiver
} character_spring_params;

// One body's swinging state. Zero-initialized is "not yet primed".
typedef struct {
	float tip[SPRING_MAX_JOINTS][3];       // the free ends, from their joints, along the world's axes
	float velocity[SPRING_MAX_JOINTS][3];  // their speeds in the world, units per second
	float head[SPRING_MAX_JOINTS][3];      // where each joint sat last update, around the body
	float origin[3];                       // where the body stood last update
	int stage;                             // 0 unprimed, 1 primed at rest, 2 swinging
} character_spring_state;

// Find every bone named `prefix`... with such a bone below it; returns how many
int character_spring_find(const SkinnedModel* model, const char* prefix,
                          character_spring_rig* rig);

// Lay spheres `spacing` apart under the named bones' skin; returns how many
int character_spring_fit_colliders(const SkinnedModel* model, const char* const* bones,
                                   int bone_count, float spacing, float radius, float margin,
                                   character_spring_rig* rig);

// Where each collider and cluster is, and its way out, given node matrices
void character_spring_collider_centers(const character_spring_rig* rig, const Mat4* global,
                                       float centers[][3], float normals[][3],
                                       float cluster_centers[][3]);

// The colliders within `reach` of a point, as indices in `out`; returns count
int character_spring_nearby_colliders(const character_spring_rig* rig, const float centers[][3],
                                      const float cluster_centers[][3], const float point[3],
                                      float reach, int* out);

// Push a point `extra` clear of the `count` colliders in `nearby`; 1 if moved
int character_spring_push_out(const character_spring_rig* rig, int joint,
                              const float centers[][3], const float normals[][3],
                              const int* nearby, int count, float extra, float point[3]);

// Forget the swing; the next update starts the hair at rest
void character_spring_reset(character_spring_state* state);

// Swing the rig's bones in `pose` for a body drawn with `to_world` this frame
void character_spring_update(const SkinnedModel* model, const character_spring_rig* rig,
                             const character_spring_params* params, const Mat4* to_world,
                             float dt, character_spring_state* state, Pose* pose);

#endif /* CHARACTER_SPRING_H */
