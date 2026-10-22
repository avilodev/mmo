#ifndef CHARACTER_RETARGET_H
#define CHARACTER_RETARGET_H

// Plays the kit's clips on a model that shares the kit's bone names

#include "model/skinned_model.h"

// Forearm twist bones: two on each arm
#define RETARGET_TWIST_MAX 4

// A forearm twist bone and the kit bone whose roll it answers
typedef struct {
	int node;
	int source;
	float counter;
} retarget_twist;

typedef struct {
	// For each node of the target: the kit node it copies the rotation of, or -1
	int source[SKIN_MAX_NODES];
	// The nodes that also copy the kit's translation, as target and kit indices
	int moved_node[2];
	int moved_source[2];
	int moved_count;
	// How much taller the target's pelvis stands than the kit's
	float move_scale;
	retarget_twist twist[RETARGET_TWIST_MAX];
	int twist_count;
} character_retarget;

// Match the target's bones to the kit's by name.
// Returns how many bones copy a rotation; 0 when the two share none.
int character_retarget_build(const SkinnedModel* kit, const SkinnedModel* target,
                             character_retarget* out);

// Pose the target as the kit stands in `kit_pose`.
// Bones the kit has no match for stay as the target was made.
void character_retarget_apply(const character_retarget* retarget, const SkinnedModel* kit,
                              const SkinnedModel* target, const Pose* kit_pose, Pose* out);

// How far a rotation rolls about a bone's own long axis, in radians (-pi, pi]
float character_retarget_roll(const float rotation[4]);

#endif /* CHARACTER_RETARGET_H */
