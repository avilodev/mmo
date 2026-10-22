// The time of day in the world, kept from the server's word at join.

#include "world/world_clock.h"

#include <stdio.h>

static int g_synced;
static uint32_t g_base_ms;
static uint32_t g_day_ms;
static double g_base_s;
static double g_tick_s;

void world_clock_reset(void) {
	g_synced = 0;
	g_base_ms = 0;
	g_day_ms = 0;
	g_base_s = 0.0;
}

int world_clock_sync(uint32_t time_of_day_ms, uint32_t day_length_ms, double now_s) {
	if(day_length_ms == 0 || time_of_day_ms >= day_length_ms)
		return 0;
	g_base_ms = time_of_day_ms;
	g_day_ms = day_length_ms;
	g_base_s = now_s;
	g_synced = 1;
	return 1;
}

int world_clock_synced(void) {
	return g_synced;
}

uint32_t world_clock_time_ms(double now_s) {
	double gone_ms = (now_s - g_base_s) * 1000.0;

	if(!g_synced)
		return 0;
	if(gone_ms < 0.0)
		gone_ms = 0.0;
	return (uint32_t)(((uint64_t)gone_ms + g_base_ms) % g_day_ms);
}

float world_clock_fraction(double now_s) {
	if(!g_synced)
		return 0.0f;
	return (float)world_clock_time_ms(now_s) / (float)g_day_ms;
}

float world_clock_hour(double now_s) {
	return world_clock_fraction(now_s) * 24.0f;
}

void world_clock_tick(double now_s) {
	g_tick_s = now_s;
}

void world_clock_text(double now_s, int hour24, char* out, size_t size) {
	uint32_t minutes;
	uint32_t hour;
	uint32_t minute;

	if(size == 0)
		return;
	if(!g_synced) {
		snprintf(out, size, "--:--");
		return;
	}
	minutes = (uint32_t)((uint64_t)world_clock_time_ms(now_s) * 1440u / g_day_ms);
	hour = minutes / 60;
	minute = minutes % 60;
	if(hour24)
		snprintf(out, size, "%02u:%02u", (unsigned)hour, (unsigned)minute);
	else
		snprintf(out, size, "%u:%02u %s", (unsigned)(hour % 12 == 0 ? 12 : hour % 12),
		         (unsigned)minute, hour < 12 ? "AM" : "PM");
}

float world_clock_hour_now(void) {
	return world_clock_hour(g_tick_s);
}

void world_clock_text_now(int hour24, char* out, size_t size) {
	world_clock_text(g_tick_s, hour24, out, size);
}
