#ifndef NET_PING_H
#define NET_PING_H

// The arithmetic behind the ping figure; no sockets, no clock, no threads.

// A ping is one packet out and the same packet back. Neither the client's
// clock nor the server's can see just one leg, so the figure shown is half of
// the whole trip: the time to go client to server, or server to client.

#include <stddef.h>
#include <stdint.h>

// Sent as the reported ping while there is nothing measured to report
#define NET_PING_UNKNOWN 0xFFFF

// How many recent measurements the shown figure is the median of
#define PING_WINDOW 5

// The most bytes a ping packet takes: the header, the report, and the number
#define PING_PACKET_MAX 16

typedef struct {
	double one_way_ms[PING_WINDOW]; // oldest overwritten first
	int count;
	int next;
} ping_window;

// Forgets every measurement, as on a new connection.
void ping_window_reset(ping_window* window);

// Adds one measurement, dropping the oldest once the window is full.
void ping_window_add(ping_window* window, double one_way_ms);

// The median of the window, or -1 when nothing has been measured.
double ping_window_median(const ping_window* window);

// The shown figure: the median rounded to a whole millisecond, or -1.
int ping_window_shown(const ping_window* window);

// Half the trip from the send time to the arrival time, in milliseconds.
double ping_one_way_ms(double sent_seconds, double arrived_seconds);

// Writes a ping packet into out; returns its size in bytes.
// reported_ms is the figure the server shows to other players.
size_t ping_build(uint8_t* out, uint32_t account_id, uint16_t reported_ms,
		uint32_t number);

// The number a ping packet carries, or 0 and no result when it carries none.
int ping_read_number(const uint8_t* packet, size_t length, uint32_t* number);

// Whether the needle appears whole inside the bytes.
int ping_bytes_contain(const uint8_t* bytes, size_t length,
		const uint8_t* needle, size_t needle_length);

// How many levels of ping there are, for the bars shown beside the figure
#define NET_PING_LEVELS 4

// How good a ping is: NET_PING_LEVELS - 1 is the best (green, up to 80 ms) down
// to 0, the worst (red, over 240 ms); yellow is up to 160 and orange up to 240.
// A ping that is not known, negative or NET_PING_UNKNOWN, is the worst.
int net_ping_level(int ping_ms);

#endif /* NET_PING_H */
