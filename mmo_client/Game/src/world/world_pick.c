#include "world/world_pick.h"
#include "world_spans.h"

#include <math.h>

#define PICK_STEP 2.0f
#define PICK_BISECT 8

static int free_at(const float p[3]) {
	return world_spans_free_point(p[0], p[1], p[2]);
}

int world_pick_ground(const float origin[3], const float dir[3], float max_dist,
                      float out[3]) {
	float prev[3] = {origin[0], origin[1], origin[2]};

	if(!free_at(prev))
		return 0;

	for(float t = PICK_STEP; t <= max_dist; t += PICK_STEP) {
		float p[3] = {origin[0] + dir[0] * t, origin[1] + dir[1] * t,
		              origin[2] + dir[2] * t};

		if(free_at(p)) {
			prev[0] = p[0];
			prev[1] = p[1];
			prev[2] = p[2];
			continue;
		}

		float lo = t - PICK_STEP;
		float hi = t;

		for(int i = 0; i < PICK_BISECT; i++) {
			float mid = (lo + hi) * 0.5f;
			float m[3] = {origin[0] + dir[0] * mid, origin[1] + dir[1] * mid,
			              origin[2] + dir[2] * mid};

			if(free_at(m))
				lo = mid;
			else
				hi = mid;
		}
		out[0] = origin[0] + dir[0] * lo;
		out[1] = origin[1] + dir[1] * lo;
		out[2] = origin[2] + dir[2] * lo;
		return 1;
	}
	return 0;
}
