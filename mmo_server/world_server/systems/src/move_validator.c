/**
 * @file
 * Validate client movement proposals against server limits and collision data.
 */

#include "move_validator.h"
#include "world_collision.h"
#include "world_spans.h"
#include "world_spans_format.h"
#include "world_motion.h"

#include <math.h>

/**
 * Return the stable log name for a movement verdict.
 *
 * @return      A static verdict name, or "unknown" for an unrecognized value.
 */
const char* move_verdict_name(MoveVerdict verdict) {
    switch (verdict) {
        case MOVE_ACCEPT:           return "accept";
        case MOVE_REJECT_COORD:     return "coord";
        case MOVE_REJECT_SPEED:     return "speed";
        case MOVE_REJECT_COLLISION: return "collision";
        case MOVE_REJECT_HEIGHT:    return "height";
        default:                    return "unknown";
    }
}

// Most vertical steps one validation will replay (a burst second plus slack).
#define MOVE_MAX_VSTEPS 128

// Pick the simulated state nearest to_z, ties to the smallest time warp. A state
// k is only allowed while the total drift (steps adopted minus real steps)
// stays within WORLD_AIR_SLACK_STEPS. -1 when none matches. Adopting the nearest
// height keeps the server on the client's own fall, so a body falling past a
// ledge is not refused for being a step behind.
static int match_state(const vertical_state* states, int top, float to_z,
                       float vdrift, float e) {
	int best = -1;
	float best_gap = 0.0f;
	float best_warp = 0.0f;

	for(int k = 0; k <= top; k++) {
		float tol = states[k].grounded ? WORLD_Z_TOLERANCE : WORLD_AIR_Z_TOLERANCE;
		float warp = fabsf(vdrift + (float)k - e);
		float gap = fabsf(to_z - states[k].z);

		if(gap > tol || warp > (float)WORLD_AIR_SLACK_STEPS)
			continue;
		if(best < 0 || gap < best_gap || (gap == best_gap && warp < best_warp)) {
			best = k;
			best_gap = gap;
			best_warp = warp;
		}
	}
	return best;
}

// A route: the straight line (corner == end) or one axis, then the other.
typedef struct {
	float fx, fy;
	float cx, cy;
	float tx, ty;
} move_route;

static float route_leg(float ax, float ay, float bx, float by) {
	return sqrtf((bx - ax) * (bx - ax) + (by - ay) * (by - ay));
}

// The point a fraction t of the way along the route.
static void route_at(const move_route* r, float t, float* x, float* y) {
	float l1 = route_leg(r->fx, r->fy, r->cx, r->cy);
	float l2 = route_leg(r->cx, r->cy, r->tx, r->ty);

	if(l2 <= 0.0f) {
		*x = r->fx + (r->tx - r->fx) * t;
		*y = r->fy + (r->ty - r->fy) * t;
		return;
	}

	float s = t * (l1 + l2);

	if(s <= l1) {
		*x = r->fx + (r->cx - r->fx) * (s / l1);
		*y = r->fy + (r->cy - r->fy) * (s / l1);
		return;
	}

	float u = (s - l1) / l2;

	if(u > 1.0f)
		u = 1.0f;
	*x = r->cx + (r->tx - r->cx) * u;
	*y = r->cy + (r->ty - r->cy) * u;
}

static int route_blocked(const move_route* r) {
	float half = MOVE_PLAYER_HALF_SIZE;

	if(route_leg(r->cx, r->cy, r->tx, r->ty) <= 0.0f) {
		return world_collision_check_box_path(r->fx, r->fy, r->tx, r->ty, half);
	}

	return world_collision_check_box_path(r->fx, r->fy, r->cx, r->cy, half) ||
	       world_collision_check_box_path(r->cx, r->cy, r->tx, r->ty, half);
}

// Collision along one route, then the vertical replay; commits only on accept.
static MoveVerdict check_route(const move_route* r, float to_z, int jump,
                               double elapsed, MoveBudget* budget,
                               vertical_state* vs) {
	if(route_blocked(r))
		return MOVE_REJECT_COLLISION;

	// the 2D checks above are unchanged; this replays the vertical motion
	vertical_state states[MOVE_MAX_VSTEPS + WORLD_AIR_SLACK_STEPS + 1];
	float length = route_leg(r->fx, r->fy, r->cx, r->cy) +
	               route_leg(r->cx, r->cy, r->tx, r->ty);
	float e = (float)(elapsed / WORLD_VSTEP);
	int steps = (int)(e + 0.5f);
	int need = (int)(length / (world_tile_size() * 0.5f)) + 1;
	int last = 0;

	if(steps < need)
		steps = need;
	// unreachable today: elapsed and distance are both capped by the budget
	if(steps > MOVE_MAX_VSTEPS)
		return MOVE_REJECT_SPEED;

	states[0] = *vs;
	float prev_x = r->fx;
	float prev_y = r->fy;
	int reached = 0;
	int arrive = -1;

	// Samples are passed in order; a body may wait a step where the client would
	for(int i = 1; i <= steps + WORLD_AIR_SLACK_STEPS; i++) {
		int at = reached < steps ? reached + 1 : steps;
		float x, y;
		vertical_state next = states[i - 1];

		route_at(r, (float)at / (float)steps, &x, &y);
		// a body stuck in solid where it stands is stood up there first, as on the
		// client; moving into solid is refused, never repaired
		vertical_unstick(&next, prev_x, prev_y, MOVE_PLAYER_HALF_SIZE);
		if(vertical_step(&next, x, y, MOVE_PLAYER_HALF_SIZE, i == 1 && jump)) {
			reached = at;
			prev_x = x;
			prev_y = y;
		} else {
			next = states[i - 1];
			vertical_unstick(&next, prev_x, prev_y, MOVE_PLAYER_HALF_SIZE);
			if(!vertical_step(&next, prev_x, prev_y, MOVE_PLAYER_HALF_SIZE, 0)) {
				if(arrive < 0)
					return MOVE_REJECT_HEIGHT;
				break;
			}
		}
		if(arrive < 0 && reached == steps)
			arrive = i;
		states[i] = next;
		last = i;
	}
	if(arrive < 0)
		return MOVE_REJECT_HEIGHT;

	int top = (int)ceilf(e) + WORLD_AIR_SLACK_STEPS;

	if(top > last)
		top = last;

	int k = match_state(states, top, to_z, budget->vdrift, e);

	if(k < 0)
		return MOVE_REJECT_HEIGHT;

	// a grounded player is exactly where the simulation put them
	if(states[k].grounded && states[arrive].grounded) {
		k = arrive;
		budget->vdrift = 0.0f;
	} else if(states[k].grounded) {
		budget->vdrift = 0.0f;
	} else {
		budget->vdrift = budget->vdrift + (float)k - e;
	}

	*vs = states[k];
	return MOVE_ACCEPT;
}

// Spend the time since the last proposal: distance credit, capped at a burst
static double budget_spend_time(MoveBudget* budget, float move_speed,
                                const struct timespec* now) {
	float speed = (isfinite(move_speed) && move_speed > 0.0f) ? move_speed : MOVE_DEFAULT_SPEED;
	float max_speed = speed * MOVE_SPEED_TOLERANCE;
	double elapsed = (double)(now->tv_sec - budget->last_tv.tv_sec) +
	                 (double)(now->tv_nsec - budget->last_tv.tv_nsec) / 1e9;

	// limit credit earned during idle intervals
	if(!(elapsed > 0.0))
		elapsed = 0.0;
	if(elapsed > (double)MOVE_BUDGET_BURST_SECONDS)
		elapsed = (double)MOVE_BUDGET_BURST_SECONDS;

	float ceiling = max_speed * MOVE_BUDGET_BURST_SECONDS;
	float credit = budget->credit + max_speed * (float)elapsed;

	if(credit > ceiling)
		credit = ceiling;
	budget->last_tv = *now;
	budget->credit = credit;
	budget->elapsed = (float)elapsed;

	return elapsed;
}

void move_hold(MoveBudget* budget, float x, float y, vertical_state* vs) {
	float e = budget->elapsed / WORLD_VSTEP;
	int steps = (int)floorf(e - budget->vdrift + 0.5f);

	if(steps < 0)
		steps = 0;
	if(steps > MOVE_MAX_VSTEPS)
		steps = MOVE_MAX_VSTEPS;
	for(int i = 0; i < steps; i++)
		vertical_advance(vs, x, y, MOVE_PLAYER_HALF_SIZE, 0);
	budget->vdrift = vs->grounded ? 0.0f : budget->vdrift + (float)steps - e;
}

int move_drop_stale(MoveBudget* budget, uint32_t echo, float x, float y,
                    float move_speed, const struct timespec* now,
                    vertical_state* vs) {
	if(echo == budget->correction_seq) {
		budget->stale_time = 0.0f;

		return 0;
	}
	// so long out of step that the client lost the correction: take its word
	if(budget->stale_time >= MOVE_STALE_SECONDS) {
		budget->correction_seq = echo;
		budget->stale_time = 0.0f;

		return 0;
	}
	budget_spend_time(budget, move_speed, now);
	budget->stale_time += budget->elapsed;
	move_hold(budget, x, y, vs);

	return 1;
}

/**
 * Validate a proposed position and debit its accepted travel distance.
 *
 * The time budget advances on every verdict, while distance credit is spent only on acceptance.
 *
 * @param budget  Mutable movement credit and timestamp state.
 * @param from_x  Current server x-coordinate in world units.
 * @param from_y  Current server y-coordinate in world units.
 * @param to_x  Proposed x-coordinate in world units.
 * @param to_y  Proposed y-coordinate in world units.
 * @param to_z  Proposed height in world units.
 * @param jump  Nonzero when the client started a jump on this move.
 * @param move_speed  Server-side movement speed in world units per second.
 * @param now  Current monotonic timestamp.
 * Vertical time is paid for with real time: budget->vdrift tracks simulation
 * steps adopted minus real elapsed steps, and may never leave the slack window.
 *
 * @param vs  Server vertical state (from_z is vs->z); replaced on MOVE_ACCEPT,
 *            untouched on every rejection.
 * @return      MOVE_ACCEPT or the first applicable rejection verdict.
 */
MoveVerdict move_validate(MoveBudget* budget,
                          float from_x, float from_y,
                          float to_x,   float to_y,
                          float to_z,   int jump,
                          float move_speed,
                          const struct timespec* now,
                          vertical_state* vs) {
    // reject coordinates before distance arithmetic
	if(!world_coord_is_valid(to_x, to_y) || !isfinite(to_z)) {
		budget->elapsed = 0.0f;
		return MOVE_REJECT_COORD;
	}

	// consume elapsed time for every verdict
	double elapsed = budget_spend_time(budget, move_speed, now);
	float credit = budget->credit;

    float dx = to_x - from_x;
    float dy = to_y - from_y;
    float distance = sqrtf(dx * dx + dy * dy);

    if (!(distance <= credit)) {
        return MOVE_REJECT_SPEED;
    }

	// The client slides an axis at a time: try the line, then each L route.
	move_route routes[3] = {
		{ from_x, from_y, to_x, to_y, to_x, to_y },
		{ from_x, from_y, to_x, from_y, to_x, to_y },
		{ from_x, from_y, from_x, to_y, to_x, to_y },
	};
	int count = (dx != 0.0f && dy != 0.0f) ? 3 : 1;
	MoveVerdict first = MOVE_ACCEPT;

	for(int i = 0; i < count; i++) {
		MoveVerdict v = check_route(&routes[i], to_z, jump, elapsed, budget, vs);

		if(v == MOVE_ACCEPT) {
			budget->credit = credit - distance;
			return MOVE_ACCEPT;
		}
		if(i == 0)
			first = v;
	}

	return first;
}
