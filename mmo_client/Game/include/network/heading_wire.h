#ifndef HEADING_WIRE_H
#define HEADING_WIRE_H

/**
 * @file
 * A facing as one byte on the wire: 0-255 for a full turn.
 *
 * Under 1.5 degrees a step, which is finer than anyone can see on a body.
 */

#include <math.h>
#include <stdint.h>

#define HEADING_WIRE_STEPS 256
#define HEADING_WIRE_TWO_PI 6.28318530718f

// A yaw in radians, any range, as its byte
static inline uint8_t heading_to_wire(float yaw) {
	float turns = yaw / HEADING_WIRE_TWO_PI;

	turns -= floorf(turns);

	return (uint8_t)((int)(turns * HEADING_WIRE_STEPS + 0.5f) & (HEADING_WIRE_STEPS - 1));
}

// A byte as a yaw in radians, in (-pi, pi]
static inline float heading_from_wire(uint8_t wire) {
	float yaw = (float)wire * (HEADING_WIRE_TWO_PI / HEADING_WIRE_STEPS);

	if(yaw > HEADING_WIRE_TWO_PI * 0.5f)
		yaw -= HEADING_WIRE_TWO_PI;

	return yaw;
}

// How many steps apart two facings are, the short way round
static inline int heading_wire_steps(uint8_t a, uint8_t b) {
	int diff = (int)a - (int)b;

	if(diff < 0)
		diff = -diff;

	return diff > HEADING_WIRE_STEPS / 2 ? HEADING_WIRE_STEPS - diff : diff;
}

#endif // HEADING_WIRE_H
