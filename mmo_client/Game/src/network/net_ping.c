// The arithmetic behind the ping figure; see net_ping.h.

#include "network/net_ping.h"
#include "protocol.h"

#include <winsock2.h>

#include <math.h>
#include <string.h>

// What follows the header: the report, then the number
#define PING_PAYLOAD_SIZE 6

void ping_window_reset(ping_window* window) {
	memset(window, 0, sizeof(*window));
}

void ping_window_add(ping_window* window, double one_way_ms) {
	window->one_way_ms[window->next] = one_way_ms;
	window->next = (window->next + 1) % PING_WINDOW;
	if(window->count < PING_WINDOW)
		window->count++;
}

// Orders the figures smallest first; the window is a handful, so insertion.
static void sort_figures(double* figures, int count) {
	for(int i = 1; i < count; i++) {
		double figure = figures[i];
		int j = i - 1;

		while(j >= 0 && figures[j] > figure) {
			figures[j + 1] = figures[j];
			j--;
		}
		figures[j + 1] = figure;
	}
}

double ping_window_median(const ping_window* window) {
	double figures[PING_WINDOW];
	int count = window->count;

	if(count <= 0)
		return -1.0;

	memcpy(figures, window->one_way_ms, sizeof(figures));
	sort_figures(figures, count);

	if(count % 2 == 1)
		return figures[count / 2];

	return (figures[count / 2 - 1] + figures[count / 2]) / 2.0;
}

int ping_window_shown(const ping_window* window) {
	double median = ping_window_median(window);

	if(median < 0.0)
		return -1;

	return (int)floor(median + 0.5);
}

double ping_one_way_ms(double sent_seconds, double arrived_seconds) {
	double trip_ms = (arrived_seconds - sent_seconds) * 1000.0;

	if(trip_ms < 0.0)
		return 0.0;

	return trip_ms / 2.0;
}

size_t ping_build(uint8_t* out, uint32_t account_id, uint16_t reported_ms,
		uint32_t number) {
	PacketHeader header;
	uint16_t reported = htons(reported_ms);
	uint32_t numbered = htonl(number);

	memset(&header, 0, sizeof(header));
	header.type = PACKET_PING;
	header.player_id = htonl(account_id);
	header.payload_size = htons(PING_PAYLOAD_SIZE);

	// a byte buffer, because the packed header leaves the payload unaligned
	memcpy(out, &header, sizeof(header));
	memcpy(out + sizeof(header), &reported, sizeof(reported));
	memcpy(out + sizeof(header) + sizeof(reported), &numbered, sizeof(numbered));

	return sizeof(header) + PING_PAYLOAD_SIZE;
}

int ping_read_number(const uint8_t* packet, size_t length, uint32_t* number) {
	uint32_t numbered;

	if(length < sizeof(PacketHeader) + PING_PAYLOAD_SIZE)
		return 0;
	if(packet[offsetof(PacketHeader, type)] != PACKET_PING)
		return 0;

	memcpy(&numbered, packet + sizeof(PacketHeader) + sizeof(uint16_t),
			sizeof(numbered));
	*number = ntohl(numbered);

	return 1;
}

int ping_bytes_contain(const uint8_t* bytes, size_t length,
		const uint8_t* needle, size_t needle_length) {
	if(needle_length == 0 || length < needle_length)
		return 0;

	for(size_t start = 0; start + needle_length <= length; start++)
		if(bytes[start] == needle[0] &&
				memcmp(bytes + start, needle, needle_length) == 0)
			return 1;

	return 0;
}

// The highest ping of each level, from the best down; anything past the last is the worst
static const int PING_LEVEL_LIMIT_MS[NET_PING_LEVELS - 1] = { 80, 160, 240 };

int net_ping_level(int ping_ms) {
	if(ping_ms < 0 || ping_ms == NET_PING_UNKNOWN)
		return 0;

	for(int i = 0; i < NET_PING_LEVELS - 1; i++) {
		if(ping_ms <= PING_LEVEL_LIMIT_MS[i])
			return NET_PING_LEVELS - 1 - i;
	}

	return 0;
}
