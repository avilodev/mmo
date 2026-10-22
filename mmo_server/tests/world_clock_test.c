// Check the world clock: the time of day is a formula of the real time, so a
// restart changes nothing, and the packet carries it in network order.

#include "world_clock.h"
#include "outbox.h"
#include "protocol.h"

#include <arpa/inet.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define EPOCH_S 1767225600LL
#define DAY_MS 2700000u

static WorldTimePacket g_sent;
static int g_sent_fd;
static int g_sent_count;

static ssize_t fake_sender(int client_fd, void* packet, size_t length) {
	assert(length == sizeof(g_sent));
	memcpy(&g_sent, packet, length);
	g_sent_fd = client_fd;
	g_sent_count++;
	return (ssize_t)length;
}

static void test_midnight_is_the_epoch(void) {
	assert(world_clock_time_of_day_ms(EPOCH_S * 1000, EPOCH_S, DAY_MS) == 0);
}

static void test_time_runs_forward_from_the_epoch(void) {
	// Quarter of a day is six game hours
	assert(world_clock_time_of_day_ms(EPOCH_S * 1000 + 675000, EPOCH_S, DAY_MS) == 675000);
	assert(world_clock_time_of_day_ms(EPOCH_S * 1000 + 1, EPOCH_S, DAY_MS) == 1);
}

static void test_a_whole_day_later_is_midnight_again(void) {
	assert(world_clock_time_of_day_ms(EPOCH_S * 1000 + DAY_MS, EPOCH_S, DAY_MS) == 0);
	assert(world_clock_time_of_day_ms(EPOCH_S * 1000 + DAY_MS - 1, EPOCH_S, DAY_MS) == DAY_MS - 1);
}

static void test_the_same_real_time_always_gives_the_same_answer(void) {
	int64_t then = (EPOCH_S + 123456) * 1000 + 789;

	// A server that stopped and started is just asked again
	assert(world_clock_time_of_day_ms(then, EPOCH_S, DAY_MS) ==
	       world_clock_time_of_day_ms(then, EPOCH_S, DAY_MS));
	assert(world_clock_time_of_day_ms(then, EPOCH_S, DAY_MS) < DAY_MS);
}

static void test_time_before_the_epoch_still_counts_down_from_midnight(void) {
	assert(world_clock_time_of_day_ms(EPOCH_S * 1000 - 1000, EPOCH_S, DAY_MS) == DAY_MS - 1000);
	assert(world_clock_time_of_day_ms(EPOCH_S * 1000 - DAY_MS, EPOCH_S, DAY_MS) == 0);
}

static void test_a_zero_day_length_is_zero_not_a_crash(void) {
	assert(world_clock_time_of_day_ms(EPOCH_S * 1000, EPOCH_S, 0) == 0);
}

static void test_unset_config_uses_the_defaults(void) {
	world_clock_configure(0, 0);
	assert(world_clock_epoch_s() == WORLD_CLOCK_DEFAULT_EPOCH_S);
	assert(world_clock_day_length_ms() == 45u * 60u * 1000u);
}

static void test_configured_values_are_used(void) {
	world_clock_configure(1000, 600);
	assert(world_clock_epoch_s() == 1000);
	assert(world_clock_day_length_ms() == 600000);
	world_clock_configure(0, 0);
}

static void test_packet_carries_both_numbers_in_network_order(void) {
	WorldTimePacket pkt;

	world_clock_configure(EPOCH_S, 2700);
	world_clock_build_packet(&pkt, 42, EPOCH_S * 1000 + 675000);
	assert(pkt.header.type == PACKET_WORLD_TIME);
	assert(ntohl(pkt.header.player_id) == 42);
	assert(ntohs(pkt.header.payload_size) == sizeof(WorldTimePacket) - sizeof(PacketHeader));
	assert(ntohl(pkt.time_of_day_ms) == 675000);
	assert(ntohl(pkt.day_length_ms) == DAY_MS);
	world_clock_configure(0, 0);
}

static void test_send_hands_one_packet_to_that_connection(void) {
	g_sent_count = 0;
	outbox_set_sender(fake_sender);
	world_clock_send(7, 99);
	assert(g_sent_count == 1);
	assert(g_sent_fd == 7);
	assert(g_sent.header.type == PACKET_WORLD_TIME);
	assert(ntohl(g_sent.header.player_id) == 99);
	assert(ntohl(g_sent.time_of_day_ms) < ntohl(g_sent.day_length_ms));
	outbox_set_sender(NULL);
}

int main(void) {
	test_midnight_is_the_epoch();
	test_time_runs_forward_from_the_epoch();
	test_a_whole_day_later_is_midnight_again();
	test_the_same_real_time_always_gives_the_same_answer();
	test_time_before_the_epoch_still_counts_down_from_midnight();
	test_a_zero_day_length_is_zero_not_a_crash();
	test_unset_config_uses_the_defaults();
	test_configured_values_are_used();
	test_packet_carries_both_numbers_in_network_order();
	test_send_hands_one_packet_to_that_connection();
	printf("world_clock_test: all passed\n");
	return 0;
}
