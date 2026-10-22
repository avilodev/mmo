// Check the ping arithmetic: the window, the half trip, and the packet.

#include "network/net_ping.h"
#include "protocol.h"

#include <winsock2.h>

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int close_to(double got, double want) {
	return fabs(got - want) < 1e-9;
}

static void test_empty_window_is_unknown(void) {
	ping_window window;

	ping_window_reset(&window);
	assert(ping_window_median(&window) < 0.0);
	assert(ping_window_shown(&window) == -1);
}

static void test_zero_is_a_real_figure(void) {
	ping_window window;

	ping_window_reset(&window);
	ping_window_add(&window, 0.2);
	assert(ping_window_shown(&window) == 0);
	assert(close_to(ping_window_median(&window), 0.2));
}

static void test_shown_figure_rounds(void) {
	ping_window window;

	ping_window_reset(&window);
	ping_window_add(&window, 2.4);
	assert(ping_window_shown(&window) == 2);

	ping_window_reset(&window);
	ping_window_add(&window, 2.6);
	assert(ping_window_shown(&window) == 3);
}

static void test_median_ignores_a_spike(void) {
	ping_window window;
	double samples[] = { 1.0, 1.0, 90.0, 1.0, 2.0 };

	ping_window_reset(&window);
	for(int i = 0; i < 5; i++)
		ping_window_add(&window, samples[i]);

	assert(close_to(ping_window_median(&window), 1.0));
}

static void test_even_count_takes_the_middle_pair(void) {
	ping_window window;

	ping_window_reset(&window);
	ping_window_add(&window, 4.0);
	ping_window_add(&window, 2.0);

	assert(close_to(ping_window_median(&window), 3.0));
}

static void test_oldest_sample_leaves_first(void) {
	ping_window window;

	ping_window_reset(&window);
	for(int i = 0; i < PING_WINDOW; i++)
		ping_window_add(&window, 100.0);
	// five fresh low figures push every old one out
	for(int i = 0; i < PING_WINDOW; i++)
		ping_window_add(&window, 5.0);

	assert(window.count == PING_WINDOW);
	assert(close_to(ping_window_median(&window), 5.0));
}

static void test_reset_forgets(void) {
	ping_window window;

	ping_window_reset(&window);
	ping_window_add(&window, 30.0);
	ping_window_reset(&window);

	assert(ping_window_shown(&window) == -1);
}

static void test_half_trip(void) {
	// a 40 ms trip is 20 ms each way
	assert(close_to(ping_one_way_ms(10.000, 10.040), 20.0));
	assert(close_to(ping_one_way_ms(5.0, 5.0), 0.0));
	// a clock that stepped backwards is no negative latency
	assert(close_to(ping_one_way_ms(5.0, 4.9), 0.0));
}

static void test_build_and_read_back(void) {
	uint8_t packet[PING_PACKET_MAX];
	uint32_t number = 0;
	size_t size = ping_build(packet, 77, 12, 0xA1B2C3D4u);
	const PacketHeader* header = (const PacketHeader*)packet;

	assert(size == sizeof(PacketHeader) + 6);
	assert(size <= PING_PACKET_MAX);
	assert(header->type == PACKET_PING);
	assert(ntohl(header->player_id) == 77);
	assert(ntohs(header->payload_size) == 6);

	assert(ping_read_number(packet, size, &number) == 1);
	assert(number == 0xA1B2C3D4u);
}

static void test_read_refuses_a_bare_ping(void) {
	// what an older client sent: the header and the two byte report only
	uint8_t packet[PING_PACKET_MAX];
	uint32_t number = 5;
	size_t size = ping_build(packet, 1, 0, 9);

	assert(ping_read_number(packet, sizeof(PacketHeader) + 2, &number) == 0);
	assert(ping_read_number(packet, size - 1, &number) == 0);
	assert(ping_read_number(packet, 0, &number) == 0);
	assert(number == 5);
}

static void test_read_refuses_other_packets(void) {
	uint8_t packet[PING_PACKET_MAX];
	uint32_t number = 0;
	size_t size = ping_build(packet, 1, 0, 9);

	packet[0] = PACKET_PING + 1;
	assert(ping_read_number(packet, size, &number) == 0);
}

static void test_contain(void) {
	uint8_t packet[PING_PACKET_MAX];
	uint8_t stream[64];
	size_t size = ping_build(packet, 3, 4, 0x01020304u);
	uint8_t other[PING_PACKET_MAX];
	size_t other_size = ping_build(other, 3, 4, 0x01020305u);

	memset(stream, 0xEE, sizeof(stream));
	assert(!ping_bytes_contain(stream, sizeof(stream), packet, size));

	// found at the front, in the middle, and flush against the end
	memcpy(stream, packet, size);
	assert(ping_bytes_contain(stream, sizeof(stream), packet, size));

	memset(stream, 0xEE, sizeof(stream));
	memcpy(stream + 20, packet, size);
	assert(ping_bytes_contain(stream, sizeof(stream), packet, size));

	memset(stream, 0xEE, sizeof(stream));
	memcpy(stream + sizeof(stream) - size, packet, size);
	assert(ping_bytes_contain(stream, sizeof(stream), packet, size));

	// the packet one number along is not this packet
	assert(!ping_bytes_contain(stream, sizeof(stream), other, other_size));
}

static void test_contain_needs_the_whole_packet(void) {
	uint8_t packet[PING_PACKET_MAX];
	size_t size = ping_build(packet, 3, 4, 0x01020304u);

	// only the first part has arrived
	assert(!ping_bytes_contain(packet, size - 1, packet, size));
	assert(!ping_bytes_contain(packet, 0, packet, size));
	assert(!ping_bytes_contain(packet, size, packet, 0));
}

static void test_ping_level_bands(void) {
	// Green to 80, yellow to 160, orange to 240, red beyond
	assert(net_ping_level(0) == 3);
	assert(net_ping_level(80) == 3);
	assert(net_ping_level(81) == 2);
	assert(net_ping_level(160) == 2);
	assert(net_ping_level(161) == 1);
	assert(net_ping_level(240) == 1);
	assert(net_ping_level(241) == 0);
	assert(net_ping_level(5000) == 0);
}

static void test_unknown_ping_is_the_worst_level(void) {
	assert(net_ping_level(-1) == 0);
	assert(net_ping_level(NET_PING_UNKNOWN) == 0);
}

static void test_every_level_is_in_range(void) {
	for(int ping = -1; ping <= 1000; ping++) {
		int level = net_ping_level(ping);

		assert(level >= 0 && level < NET_PING_LEVELS);
	}
}

int main(void) {
	test_empty_window_is_unknown();
	test_zero_is_a_real_figure();
	test_shown_figure_rounds();
	test_median_ignores_a_spike();
	test_even_count_takes_the_middle_pair();
	test_oldest_sample_leaves_first();
	test_reset_forgets();
	test_half_trip();
	test_build_and_read_back();
	test_read_refuses_a_bare_ping();
	test_read_refuses_other_packets();
	test_contain();
	test_contain_needs_the_whole_packet();
	test_ping_level_bands();
	test_unknown_ping_is_the_worst_level();
	test_every_level_is_in_range();

	puts("net_ping_test: all passed");
	return 0;
}
