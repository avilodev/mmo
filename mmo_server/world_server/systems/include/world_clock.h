#ifndef WORLD_CLOCK_H
#define WORLD_CLOCK_H

// The world's time of day. It is a formula of the real time, not a counter, so
// it needs no saving, a restart changes nothing, and every world server agrees.

#include "protocol.h"

#include <stdint.h>

// Unix seconds of a game midnight: 2026-01-01 00:00 UTC
#define WORLD_CLOCK_DEFAULT_EPOCH_S 1767225600LL

// Real seconds in one game day: 45 minutes
#define WORLD_CLOCK_DEFAULT_DAY_S 2700

// Real seconds a day may be set to by the config, at least and at most
#define WORLD_CLOCK_DAY_S_MIN 60
#define WORLD_CLOCK_DAY_S_MAX 86400

// Take the config values; 0 keeps the default of each. Run once at startup.
void world_clock_configure(int64_t epoch_s, int day_length_s);

// The epoch and day length in use
int64_t world_clock_epoch_s(void);
uint32_t world_clock_day_length_ms(void);

// Milliseconds since game midnight at a real time, in 0 up to day_length_ms
uint32_t world_clock_time_of_day_ms(int64_t unix_ms, int64_t epoch_s, uint32_t day_length_ms);

// Fill in the packet that tells a player the time at a real time
void world_clock_build_packet(WorldTimePacket* pkt, uint32_t character_id, int64_t unix_ms);

// Tell one connection the time now
void world_clock_send(int client_fd, uint32_t character_id);

#endif /* WORLD_CLOCK_H */
