// Why a step of the body was refused, logged once per spot. See move_refusal.h.

#include "player/move_refusal.h"
#include "world_spans.h"
#include "world_spans_format.h"
#include "world_regions.h"
#include "core/client_log.h"

#include <math.h>
#include <stdio.h>
#include <time.h>

// A refusal this near the last one logged, for the same reason, is that spot
#define REFUSAL_SAME_SPOT 8.0f
#define REFUSAL_PER_SECOND 2

static float g_last_x = 0.0f;
static float g_last_y = 0.0f;
static const char* g_last_reason = NULL;
static time_t g_window = 0;
static int g_in_window = 0;

// The test in world_motion.c that failed, by the same checks in the same order
static const char* refusal_reason(float x, float y, float half, const vertical_state* s,
                                  int blocked_2d) {
	float land = 0.0f;

	if(blocked_2d)
		return "solid tile";
	if(s->grounded && world_spans_steep_above(x, y, s->z))
		return "too steep";
	if(!world_spans_free_box(x, y, half, s->z))
		return "no room";
	if(!s->grounded && s->vz <= 0.0f && !world_spans_clear_box(x, y, half, s->z) &&
	   !world_spans_support(x, y, half, s->z, s->z + WORLD_STEP_HEIGHT, &land))
		return "ledge face";
	return "other";
}

static int already_logged(float x, float y, const char* reason) {
	time_t now = time(NULL);
	float dx = x - g_last_x;
	float dy = y - g_last_y;

	if(reason == g_last_reason && dx * dx + dy * dy < REFUSAL_SAME_SPOT * REFUSAL_SAME_SPOT)
		return 1;
	if(now != g_window) {
		g_window = now;
		g_in_window = 0;
	}
	if(g_in_window >= REFUSAL_PER_SECOND)
		return 1;
	g_in_window++;
	g_last_x = x;
	g_last_y = y;
	g_last_reason = reason;

	return 0;
}

// " | c0 [floor..ceil] ..." for corner k of the box, appended to buf at `at`
static int describe_corner(char* buf, int len, int at, int k, float x, float y) {
	world_span spans[WORLD_SPANS_MAX_PER_TILE];
	int count = world_spans_at(x, y, spans, WORLD_SPANS_MAX_PER_TILE);
	int n = snprintf(buf + at, (size_t)(len - at), " | c%d%s", k, count ? "" : " none");

	for(int i = 0; i < count && at + n < len; i++) {
		char ceil[16] = "open";

		if(spans[i].ceil < WORLD_SPAN_OPEN_CEIL)
			snprintf(ceil, sizeof(ceil), "%.1f", spans[i].ceil);
		n += snprintf(buf + at + n, (size_t)(len - at - n), " [%.1f..%s]", spans[i].floor, ceil);
	}

	return at + n < len ? at + n : len - 1;
}

void move_refusal_note(float x, float y, float to_x, float to_y, float half,
                       const vertical_state* s, int blocked_2d) {
	const char* reason = refusal_reason(to_x, to_y, half, s, blocked_2d);
	char corners[512];
	int at = 0;

	if(already_logged(to_x, to_y, reason))
		return;
	corners[0] = '\0';
	for(int k = 0; k < 4; k++) {
		at = describe_corner(corners, (int)sizeof(corners), at, k,
		                     to_x + ((k & 1) ? half : -half), to_y + ((k & 2) ? half : -half));
	}
	CLOG_INFO("[MOVE] refused (%s) (%.1f, %.1f) -> (%.1f, %.1f) scene (%.2f, %.2f) "
	          "z %.1f vz %.0f %s%s",
	          reason, x, y, to_x, to_y, to_x / 16.0f - (float)CITY_ENNARA_TILE_X,
	          to_y / 16.0f - (float)CITY_ENNARA_TILE_Y, s->z, s->vz,
	          s->grounded ? "grounded" : "airborne", corners);
}

void move_refusal_reset(void) {
	g_last_x = 0.0f;
	g_last_y = 0.0f;
	g_last_reason = NULL;
	g_window = 0;
	g_in_window = 0;
}
