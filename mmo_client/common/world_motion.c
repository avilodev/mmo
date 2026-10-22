// One fixed step of walking, jumping and falling against the span layer.

#include "world_motion.h"
#include "world_spans.h"
#include "world_spans_format.h"

// Follow the floor, walk off an edge, or refuse a wall.
static int step_grounded(vertical_state* n, float x, float y, float half) {
	float floor_z = 0.0f;

	if(world_spans_reach_box(x, y, half, n->z, &floor_z)) {
		n->z = floor_z;
		return 1;
	}
	// the ground ahead rises too steeply to walk up
	if(world_spans_steep_above(x, y, n->z))
		return 0;
	if(!world_spans_free_box(x, y, half, n->z))
		return 0;

	// part of the box still has a floor under it: keep standing on it
	if(world_spans_support(x, y, half, n->z - WORLD_STEP_HEIGHT,
	                       n->z + WORLD_STEP_HEIGHT, &floor_z)) {
		n->z = floor_z;
		return 1;
	}

	// the whole box is clear of the floor: start falling from rest
	n->grounded = 0;
	n->vz = 0.0f;
	return 1;
}

static int step_airborne(vertical_state* n, float x, float y, float half) {
	float land = 0.0f;

	if(!world_spans_free_box(x, y, half, n->z))
		return 0;

	// a body not rising that overlaps a ledge's face stands on it if it can
	if(n->vz <= 0.0f && !world_spans_clear_box(x, y, half, n->z) &&
	   world_spans_support(x, y, half, n->z, n->z + WORLD_STEP_HEIGHT, &land)) {
		n->z = land;
		n->vz = 0.0f;
		n->grounded = 1;
		return 1;
	}

	n->vz -= WORLD_GRAVITY * WORLD_VSTEP;
	if(n->vz < -WORLD_TERMINAL_SPEED)
		n->vz = -WORLD_TERMINAL_SPEED;

	float next_z = n->z + n->vz * WORLD_VSTEP;

	if(n->vz > 0.0f && !world_spans_free_box(x, y, half, next_z)) {
		// the head met a ceiling
		n->vz = 0.0f;
		next_z = n->z;
	}
	if(n->vz <= 0.0f &&
	   world_spans_floor_between(x, y, half, next_z, n->z + WORLD_STEP_HEIGHT, &land)) {
		n->z = land;
		n->vz = 0.0f;
		n->grounded = 1;
		return 1;
	}
	// falling into a ledge the box overlaps: rest on it instead of entering it
	if(n->vz <= 0.0f && !world_spans_clear_box(x, y, half, next_z) &&
	   world_spans_support(x, y, half, next_z, n->z + WORLD_STEP_HEIGHT, &land)) {
		n->z = land;
		n->vz = 0.0f;
		n->grounded = 1;
		return 1;
	}
	// never fall into solid: with nowhere to go the body waits where it fits
	if(!world_spans_free_box(x, y, half, next_z)) {
		n->vz = 0.0f;
		return 1;
	}
	n->z = next_z;
	return 1;
}

int vertical_step(vertical_state* s, float x, float y, float half, int jump) {
	vertical_state n = *s;

	if(!isfinite(x) || !isfinite(y) || !isfinite(n.z) || !isfinite(n.vz))
		return 0;

	if(n.grounded && jump) {
		n.vz = WORLD_JUMP_SPEED;
		n.grounded = 0;
	}

	int ok = n.grounded ? step_grounded(&n, x, y, half)
	                    : step_airborne(&n, x, y, half);

	if(!ok)
		return 0;
	*s = n;
	return 1;
}

// How far below a stuck body to look for a floor to stand it on
#define WORLD_UNSTICK_DEPTH 4096.0f

int vertical_unstick(vertical_state* s, float x, float y, float half) {
	world_span spans[WORLD_SPANS_MAX_PER_TILE];
	float land = 0.0f;
	int count;
	int best = -1;

	if(s->grounded || !isfinite(s->z) || world_spans_free_box(x, y, half, s->z))
		return 0;

	// prefer a floor the whole box fits on
	if(world_spans_floor_between(x, y, half, s->z - WORLD_UNSTICK_DEPTH, s->z, &land)) {
		s->z = land;
		s->vz = 0.0f;
		s->grounded = 1;

		return 1;
	}

	count = world_spans_at(x, y, spans, WORLD_SPANS_MAX_PER_TILE);
	for(int i = 0; i < count; i++) {
		if(spans[i].floor > s->z || spans[i].ceil - spans[i].floor < WORLD_BODY_HEIGHT)
			continue;
		if(best < 0 || spans[i].floor > spans[best].floor)
			best = i;
	}
	// a solid centre (a post in the box): any edge's floor, below first
	if(best < 0) {
		if(!world_spans_support(x, y, half, s->z - WORLD_UNSTICK_DEPTH, s->z, &land) &&
		   !world_spans_support(x, y, half, s->z, s->z + WORLD_BODY_HEIGHT, &land))
			return 0;
		s->z = land;
		s->vz = 0.0f;
		s->grounded = 1;

		return 1;
	}
	s->z = spans[best].floor;
	s->vz = 0.0f;
	s->grounded = 1;

	return 1;
}

int vertical_advance(vertical_state* s, float x, float y, float half, int jump) {
	if(vertical_step(s, x, y, half, jump))
		return 1;

	return vertical_unstick(s, x, y, half);
}
