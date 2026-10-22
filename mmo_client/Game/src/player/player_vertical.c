#include "player/player_vertical.h"

#define JUMP_BUFFER_SECONDS 0.15f
#define MAX_CATCH_UP_SECONDS 0.25f

int vertical_probe(const vertical_state* s, float x, float y, float half) {
	vertical_state copy = *s;

	return vertical_step(&copy, x, y, half, 0);
}

void vertical_update(vertical_state* s, vertical_clock* c, float x, float y,
                     float half, float dt, int jump_pressed, int* jumped) {
	*jumped = 0;

	if(jump_pressed)
		c->jump_wait = JUMP_BUFFER_SECONDS;
	else if(c->jump_wait > 0.0f)
		c->jump_wait -= dt;

	c->accum += dt;
	if(c->accum > MAX_CATCH_UP_SECONDS)
		c->accum = MAX_CATCH_UP_SECONDS;

	while(c->accum >= WORLD_VSTEP) {
		int want = c->jump_wait > 0.0f && s->grounded;

		if(vertical_advance(s, x, y, half, want) && want) {
			*jumped = 1;
			c->jump_wait = 0.0f;
		}
		c->accum -= WORLD_VSTEP;
	}
}
