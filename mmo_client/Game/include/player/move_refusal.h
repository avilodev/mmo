#ifndef MOVE_REFUSAL_H
#define MOVE_REFUSAL_H

/**
 * @file
 * Log why the client refused a step of the body, so each place it stops can be
 * classified from the log: a solid tile, no room for the box, or a ledge face
 * a falling body cannot rest on. One line per spot, at most two a second.
 */

#include "world_motion.h"

/** Note a refused step from (x, y) to (to_x, to_y) of a body in state s.
 *  blocked_2d is 1 when the tile layer refused it before the spans did. */
void move_refusal_note(float x, float y, float to_x, float to_y, float half,
                       const vertical_state* s, int blocked_2d);

/** Forget the last spot and the second being counted, as at a fresh start. */
void move_refusal_reset(void);

#endif /* MOVE_REFUSAL_H */
