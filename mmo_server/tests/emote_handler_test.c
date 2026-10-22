// Drive the emote handler against a stub player table and check its answers.

#define _POSIX_C_SOURCE 200809L

#include "emote_handler.h"
#include "combat_stats.h"
#include "player_data.h"
#include "world_regions.h"
#include "world_seats.h"

#include <assert.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TILE ((float)WORLD_TILE_PX)
#define SCRATCH "/tmp/emote_handler_seats.bin"

static const float OX = CITY_ENNARA_TILE_X * TILE;
static const float OY = CITY_ENNARA_TILE_Y * TILE;

typedef struct {
	int fd;
	EmoteStatePacket pkt;
} sent_packet;

// The player table, stubbed.
ActivePlayer active_players[MAX_PLAYERS];
static int g_online[MAX_PLAYERS];
static int g_online_count;
static double g_now = 1000.0;

// What the server sent.
static sent_packet g_sent[64];
static int g_sent_count;

double get_time_seconds(void) {
	return g_now;
}

void player_registry_rdlock(void) {
}

void player_registry_unlock(void) {
}

const int* player_active_list_locked(int* out_count) {
	*out_count = g_online_count;

	return g_online;
}

ActivePlayer* player_acquire_hint(uint32_t character_id, int slot) {
	if(slot < 0 || slot >= MAX_PLAYERS || !active_players[slot].is_loaded)
		return NULL;
	if(active_players[slot].character_id != character_id)
		return NULL;

	pthread_mutex_lock(&active_players[slot].lock);

	return &active_players[slot];
}

void player_release(ActivePlayer* p) {
	pthread_mutex_unlock(&p->lock);
}

ssize_t outbox_send(int fd, void* buf, size_t len) {
	assert(len == sizeof(EmoteStatePacket));
	g_sent[g_sent_count].fd = fd;
	memcpy(&g_sent[g_sent_count].pkt, buf, len);
	g_sent_count++;

	return (ssize_t)len;
}

// Two seats, at tiles (0, 0) and (10, 0).
static void load_two_seats(void) {
	FILE* f = fopen(SCRATCH, "wb");
	int32_t head[2] = { 1, 2 };
	float rec[2][4] = { { 0.0f, 0.0f, 10.0f, 1.0f }, { 10.0f, 0.0f, 10.0f, 2.0f } };
	char name[32] = { 0 };

	assert(f);
	fwrite("CSIT", 1, 4, f);
	fwrite(head, 4, 2, f);
	for(int i = 0; i < 2; i++) {
		fwrite(rec[i], 4, 4, f);
		fwrite(name, 1, sizeof(name), f);
	}
	fclose(f);
	assert(world_seats_load(SCRATCH) == 1);
}

// Put a loaded player in a slot, standing at an offset from the centre.
static void add_player(int slot, uint32_t id, int fd, float dx, float dy) {
	ActivePlayer* p = &active_players[slot];

	memset(p, 0, sizeof(*p));
	pthread_mutex_init(&p->lock, NULL);
	p->character_id = id;
	p->client_fd = fd;
	p->is_loaded = 1;
	p->pos_x = OX + dx;
	p->pos_y = OY + dy;
	g_online[g_online_count++] = slot;
}

static void ask(int slot, uint8_t emote, uint8_t arg) {
	EmoteRequestPacket pkt;
	ActivePlayer* p = &active_players[slot];

	memset(&pkt, 0, sizeof(pkt));
	pkt.emote = emote;
	pkt.arg = arg;
	g_sent_count = 0;
	handle_emote_request(p->client_fd, p->character_id, slot, &pkt);
}

static void expect_reply(int fd, uint8_t emote, uint8_t arg, uint8_t result,
		uint8_t requested) {
	uint16_t payload = sizeof(EmoteStatePacket) - sizeof(PacketHeader);

	assert(g_sent_count == 1);
	assert(g_sent[0].fd == fd);
	assert(g_sent[0].pkt.header.type == PACKET_EMOTE_STATE);
	assert(g_sent[0].pkt.emote == emote && g_sent[0].pkt.arg == arg);
	assert(g_sent[0].pkt.result == result && g_sent[0].pkt.requested == requested);
	assert(ntohs(g_sent[0].pkt.header.payload_size) == payload);
}

static void test_one_seat_one_sitter(void) {
	ask(0, EMOTE_SIT, 0);
	expect_reply(100, EMOTE_SIT, 0, EMOTE_RESULT_STARTED, EMOTE_SIT);
	assert(active_players[0].emote == EMOTE_SIT && active_players[0].emote_arg == 0);
	assert(active_players[0].emote_started_at == g_now);

	// another player at the same seat is refused, and nothing changes for them
	ask(1, EMOTE_SIT, 0);
	expect_reply(101, EMOTE_NONE, 0, EMOTE_REFUSED_SEAT_TAKEN, EMOTE_SIT);
	assert(active_players[1].emote == EMOTE_NONE);

	// the sitter asking again is not "taken by someone else"
	ask(0, EMOTE_SIT, 0);
	expect_reply(100, EMOTE_SIT, 0, EMOTE_RESULT_STARTED, EMOTE_SIT);

	// the other seat is free, but only from where they stand
	ask(1, EMOTE_SIT, 1);
	expect_reply(101, EMOTE_NONE, 0, EMOTE_REFUSED_NO_SEAT, EMOTE_SIT);
	active_players[1].pos_x = OX + 10.0f * TILE;
	ask(1, EMOTE_SIT, 1);
	expect_reply(101, EMOTE_SIT, 1, EMOTE_RESULT_STARTED, EMOTE_SIT);

	// getting up frees the seat for the other
	ask(0, EMOTE_NONE, 0);
	expect_reply(100, EMOTE_NONE, 0, EMOTE_RESULT_STOPPED, EMOTE_NONE);
	assert(active_players[0].emote == EMOTE_NONE);
	active_players[1].emote = EMOTE_NONE;
	active_players[1].pos_x = OX;
	ask(1, EMOTE_SIT, 0);
	expect_reply(101, EMOTE_SIT, 0, EMOTE_RESULT_STARTED, EMOTE_SIT);
	ask(1, EMOTE_NONE, 0);
}

static void test_refusals_change_nothing(void) {
	ask(0, EMOTE_DANCE, 0);
	expect_reply(100, EMOTE_DANCE, 0, EMOTE_RESULT_STARTED, EMOTE_DANCE);

	// seat 1 is 160 units away
	ask(0, EMOTE_SIT, 1);
	expect_reply(100, EMOTE_DANCE, 0, EMOTE_REFUSED_NO_SEAT, EMOTE_SIT);
	assert(active_players[0].emote == EMOTE_DANCE);

	// not an emote
	ask(0, 77, 0);
	expect_reply(100, EMOTE_DANCE, 0, EMOTE_REFUSED_UNKNOWN, 77);
	assert(active_players[0].emote == EMOTE_DANCE);

	active_players[0].is_dead = 1;
	ask(0, EMOTE_SIT, 0);
	expect_reply(100, EMOTE_DANCE, 0, EMOTE_REFUSED_DEAD, EMOTE_SIT);
	active_players[0].is_dead = 0;

	// fought a second ago
	active_players[0].last_combat_time = g_now - 1.0;
	ask(0, EMOTE_SIT, 0);
	expect_reply(100, EMOTE_DANCE, 0, EMOTE_REFUSED_COMBAT, EMOTE_SIT);
	active_players[0].last_combat_time = g_now - OOC_THRESHOLD_SECONDS - 1.0;
	ask(0, EMOTE_NONE, 0);
	assert(active_players[0].emote == EMOTE_NONE);
}

static void test_unknown_player_is_ignored(void) {
	EmoteRequestPacket pkt;

	memset(&pkt, 0, sizeof(pkt));
	pkt.emote = EMOTE_DANCE;
	g_sent_count = 0;

	// nobody is in slot 5, and slot 0 holds a different character
	handle_emote_request(555, 4242, 5, &pkt);
	assert(g_sent_count == 0);
	handle_emote_request(555, 4242, 0, &pkt);
	assert(g_sent_count == 0);
	assert(active_players[0].emote == EMOTE_NONE);
}

static void test_upkeep_ends_emotes(void) {
	// nothing to end: nothing sent
	g_sent_count = 0;
	emote_upkeep_tick();
	assert(g_sent_count == 0);

	ask(0, EMOTE_DANCE, 0);
	active_players[1].pos_x = OX;
	ask(1, EMOTE_SIT, 0);
	assert(active_players[0].emote == EMOTE_DANCE && active_players[1].emote == EMOTE_SIT);

	// still fine a moment later
	g_now += 5.0;
	g_sent_count = 0;
	emote_upkeep_tick();
	assert(g_sent_count == 0);
	assert(active_players[0].emote == EMOTE_DANCE);

	// one fights: only theirs ends, and they are told why
	active_players[0].last_combat_time = g_now;
	emote_upkeep_tick();
	assert(g_sent_count == 1);
	assert(g_sent[0].fd == 100 && g_sent[0].pkt.emote == EMOTE_NONE);
	assert(g_sent[0].pkt.result == EMOTE_ENDED_COMBAT);
	assert(g_sent[0].pkt.requested == EMOTE_DANCE);
	assert(active_players[0].emote == EMOTE_NONE && active_players[1].emote == EMOTE_SIT);

	// one dies: their seat is freed, and they are told
	active_players[1].is_dead = 1;
	g_sent_count = 0;
	emote_upkeep_tick();
	assert(g_sent_count == 1 && g_sent[0].fd == 101);
	assert(g_sent[0].pkt.result == EMOTE_ENDED_DEAD);
	assert(g_sent[0].pkt.requested == EMOTE_SIT);
	assert(active_players[1].emote == EMOTE_NONE);

	// and it is not told twice
	g_sent_count = 0;
	emote_upkeep_tick();
	assert(g_sent_count == 0);
}

int main(void) {
	double calm = OOC_THRESHOLD_SECONDS + 10.0;

	load_two_seats();
	add_player(0, 1, 100, 10.0f, 0.0f);
	add_player(1, 2, 101, 20.0f, 0.0f);
	g_now += 100.0;
	active_players[0].last_combat_time = g_now - calm;
	active_players[1].last_combat_time = g_now - calm;

	test_one_seat_one_sitter();
	test_refusals_change_nothing();
	test_unknown_player_is_ignored();
	test_upkeep_ends_emotes();
	world_seats_free();
	printf("emote_handler_test: OK\n");

	return 0;
}
