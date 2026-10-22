// The ground speed other players animate a body by.

#include "anim_speed.h"
#include "move_validator.h"

#include <math.h>

float anim_speed_reported(float vel_x, float vel_y, float move_speed) {
	float speed = hypotf(vel_x, vel_y);
	float base = (isfinite(move_speed) && move_speed > 0.0f) ? move_speed : MOVE_DEFAULT_SPEED;
	float ceiling = base * MOVE_SPEED_TOLERANCE;

	if(!isfinite(speed))
		return 0.0f;
	if(speed > ceiling)
		return ceiling;

	return speed;
}

float anim_speed_current(float speed, double set_at, double now) {
	if(now - set_at > ANIM_SPEED_STALE_SECONDS)
		return 0.0f;

	return speed;
}
