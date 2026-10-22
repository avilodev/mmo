#ifndef PLAYER_VERTICAL_H
#define PLAYER_VERTICAL_H

/**
 * @file
 * The client's use of the shared vertical motion (world_motion.h): standing on
 * surfaces, jumping and falling at a fixed step, whatever the frame rate.
 */

#include "world_motion.h"

/** The leftover time and the remembered jump press between frames. */
typedef struct {
	float accum;
	float jump_wait;
} vertical_clock;

/** 1 when the body could stand or move at (x, y) in its current state: a
 *  vertical_step on a copy succeeds. Changes nothing. */
int vertical_probe(const vertical_state* s, float x, float y, float half);

/** Run as many fixed steps as dt allows (at most a quarter second), jumping on
 *  the first step in which the body is grounded while a jump is wanted. A press
 *  is remembered for 0.15 s. *jumped is 1 when a step started a jump. */
void vertical_update(vertical_state* s, vertical_clock* c, float x, float y,
                     float half, float dt, int jump_pressed, int* jumped);

#endif /* PLAYER_VERTICAL_H */
