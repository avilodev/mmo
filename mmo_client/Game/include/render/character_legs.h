#ifndef CHARACTER_LEGS_H
#define CHARACTER_LEGS_H

// The rig's legs: find the bones, read them off a pose, bend them to a solution

#include "model/skinned_model.h"
#include "render/character_foot_ik.h"

// The leg bones as skeleton node indices (0 left, 1 right); -1 when missing
typedef struct {
	int thigh[2];
	int calf[2];
	int foot[2];
	float rest_ankle[2];   // ankle height above the floor in the bind pose
} character_legs_rig;

// A pose's legs in the model's frame
typedef struct {
	foot_ik_leg leg[2];     // hip = thigh, knee = calf, ankle = foot joint
	float foot_rot[2][4];   // each foot's orientation in the model's frame
} character_legs_clip;

// Find the bones and measure the bind pose. Nonzero when every bone exists.
int character_legs_rig_find(const SkinnedModel* model, character_legs_rig* rig);

void character_legs_measure(const SkinnedModel* model, const character_legs_rig* rig,
                            const Pose* pose, character_legs_clip* out);

// Bend the legs to `result`; each foot gets tilt[leg] times its `clip` rotation
void character_legs_apply(const SkinnedModel* model, const character_legs_rig* rig,
                          const character_legs_clip* clip,
                          const foot_ik_result result[2], const float tilt[2][4],
                          Pose* pose);

#endif /* CHARACTER_LEGS_H */
