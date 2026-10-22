#ifndef CHARACTER_FOOT_IK_H
#define CHARACTER_FOOT_IK_H

// Leg solver and pelvis rule for stairs and slopes, in the rig's own space

// One leg as the clip poses it: the hip, knee and ankle joints
typedef struct {
	float hip[3];
	float knee[3];
	float ankle[3];
} foot_ik_leg;

typedef struct {
	float knee[3];
	float ankle[3];
} foot_ik_result;

// The pelvis follows the lower foot's ground offset, within the two limits
float foot_ik_pelvis_target(float raise_a, float raise_b, float max_drop,
                            float max_rise);

// Solve one leg to reach `raise` above the clip's ankle, hip unmoved
void foot_ik_solve_leg(const foot_ik_leg* clip, float raise, float max_reach,
                       foot_ik_result* out);

// value moved toward target by an exponential follow of `rate` per second
float foot_ik_chase(float value, float target, float rate, float dt);

#endif /* CHARACTER_FOOT_IK_H */
