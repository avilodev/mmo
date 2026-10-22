#include "render/height_smooth.h"

#include <math.h>

#define SMOOTH_RATE 14.0f
#define SMOOTH_SNAP_GAP 48.0f

float height_smooth_step(float current, float target, float dt) {
	if(!isfinite(current) || !isfinite(dt) || !isfinite(target))
		return target;
	if(fabsf(target - current) > SMOOTH_SNAP_GAP)
		return target;

	float k = SMOOTH_RATE * dt;

	if(k > 1.0f)
		k = 1.0f;
	if(k < 0.0f)
		k = 0.0f;
	return current + (target - current) * k;
}
