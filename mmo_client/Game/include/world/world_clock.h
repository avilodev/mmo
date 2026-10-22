#ifndef WORLD_CLOCK_H
#define WORLD_CLOCK_H

// The time of day in the world, kept from the server's word at join. No GL.
// The caller passes the local clock in seconds, so tests need no real time.

#include <stddef.h>
#include <stdint.h>

// Forget the server's time, as at the end of a session
void world_clock_reset(void);

// Take the server's time of day. Returns nonzero when it was accepted, zero
// when the numbers make no sense; a rejected sync changes nothing.
int world_clock_sync(uint32_t time_of_day_ms, uint32_t day_length_ms, double now_s);

// Nonzero once the server has given the time
int world_clock_synced(void);

// Milliseconds since midnight, 0 until synced
uint32_t world_clock_time_ms(double now_s);

// Hours since midnight, 0 up to 24, 0 until synced
float world_clock_hour(double now_s);

// Fraction of the day gone, 0 up to 1, 0 until synced
float world_clock_fraction(double now_s);

// Note the local clock in seconds, once a frame, for readers with no clock of their own
void world_clock_tick(double now_s);

// The time of day as text: "2:30 PM", or "14:30" when hour24 is nonzero.
// "--:--" until synced. The text is cut to fit size, and nothing is written for 0.
void world_clock_text(double now_s, int hour24, char* out, size_t size);

// As world_clock_hour, at the time last given to world_clock_tick
float world_clock_hour_now(void);

// As world_clock_text, at the time last given to world_clock_tick
void world_clock_text_now(int hour24, char* out, size_t size);

#endif /* WORLD_CLOCK_H */
