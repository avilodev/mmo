// The world's time of day, a formula of the real time.

#include "world_clock.h"
#include "outbox.h"

#include <arpa/inet.h>
#include <string.h>
#include <time.h>

static int64_t g_epoch_s = WORLD_CLOCK_DEFAULT_EPOCH_S;
static uint32_t g_day_length_ms = WORLD_CLOCK_DEFAULT_DAY_S * 1000u;

void world_clock_configure(int64_t epoch_s, int day_length_s) {
	uint32_t day_s = day_length_s > 0 ? (uint32_t)day_length_s : WORLD_CLOCK_DEFAULT_DAY_S;

	g_epoch_s = epoch_s != 0 ? epoch_s : WORLD_CLOCK_DEFAULT_EPOCH_S;
	g_day_length_ms = day_s * 1000u;
}

int64_t world_clock_epoch_s(void) {
	return g_epoch_s;
}

uint32_t world_clock_day_length_ms(void) {
	return g_day_length_ms;
}

uint32_t world_clock_time_of_day_ms(int64_t unix_ms, int64_t epoch_s, uint32_t day_length_ms) {
	int64_t in_day;

	if(day_length_ms == 0)
		return 0;
	in_day = (unix_ms - epoch_s * 1000) % (int64_t)day_length_ms;
	if(in_day < 0)
		in_day += day_length_ms;
	return (uint32_t)in_day;
}

void world_clock_build_packet(WorldTimePacket* pkt, uint32_t character_id, int64_t unix_ms) {
	memset(pkt, 0, sizeof(*pkt));
	pkt->header.type = PACKET_WORLD_TIME;
	pkt->header.player_id = htonl(character_id);
	pkt->header.payload_size = htons(sizeof(*pkt) - sizeof(PacketHeader));
	pkt->time_of_day_ms = htonl(world_clock_time_of_day_ms(unix_ms, g_epoch_s, g_day_length_ms));
	pkt->day_length_ms = htonl(g_day_length_ms);
}

void world_clock_send(int client_fd, uint32_t character_id) {
	struct timespec now;
	WorldTimePacket pkt;

	clock_gettime(CLOCK_REALTIME, &now);
	world_clock_build_packet(&pkt, character_id, (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000);
	outbox_send(client_fd, &pkt, sizeof(pkt));
}
