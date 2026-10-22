#ifndef MOVE_CORRECTION_H
#define MOVE_CORRECTION_H

#include <stdint.h>

// A refused move: where the server holds the body and how it is falling
typedef struct {
	float x, y, z;
	float vz;
	int grounded;
	uint32_t sequence;   // the move refused
} move_correction;

#endif /* MOVE_CORRECTION_H */
