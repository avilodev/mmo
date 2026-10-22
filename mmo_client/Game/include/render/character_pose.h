#ifndef CHARACTER_POSE_H
#define CHARACTER_POSE_H

// Applies the procedural offsets to a sampled pose of the character rig

#include "model/skinned_model.h"
#include "render/character_procedural.h"

// The bones the layer moves, as skeleton node indices; -1 when missing.
typedef struct {
	int spine[3];
	int neck;
	int head;
} character_rig;

// Find the bones by name. Nonzero when every one exists.
int character_rig_find(const SkinnedModel* model, character_rig* rig);

// Rotate the rig's bones by `offsets`. A missing bone is skipped.
void character_pose_apply(const SkinnedModel* model, const character_rig* rig,
                          const character_offsets* offsets, Pose* pose);

// The skeleton node named `name`, or -1
int character_pose_find_node(const SkinnedModel* model, const char* name);

// The rotation that takes the model's frame to `node`'s parent's frame
void character_pose_parent_rotation(const SkinnedModel* model, const Pose* pose,
                                    int node, float out[4]);

// Turn `node` by the model-frame rotation `q` about its own joint
void character_pose_turn_node(const SkinnedModel* model, Pose* pose, int node,
                              const float q[4]);

#endif /* CHARACTER_POSE_H */
