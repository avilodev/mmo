// Drive the inspect handler against a stub player table and check its answers.

#define _POSIX_C_SOURCE 200809L

#include "inspect_handler.h"
#include "player_data.h"

#include <assert.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
	int fd;
	InspectResponsePacket pkt;
} sent_packet;

// The player table, stubbed.
ActivePlayer active_players[MAX_PLAYERS];

// What the server sent.
static sent_packet g_sent[16];
static int g_sent_count;

ActivePlayer* player_acquire_hint(uint32_t character_id, int slot) {
	if(slot < 0 || slot >= MAX_PLAYERS || !active_players[slot].is_loaded)
		return NULL;
	if(active_players[slot].character_id != character_id)
		return NULL;

	pthread_mutex_lock(&active_players[slot].lock);

	return &active_players[slot];
}

ActivePlayer* player_acquire(uint32_t character_id) {
	for(int i = 0; i < MAX_PLAYERS; i++) {
		if(active_players[i].character_id != character_id)
			continue;
		if(!active_players[i].client_fd)
			continue;

		pthread_mutex_lock(&active_players[i].lock);

		return &active_players[i];
	}

	return NULL;
}

void player_release(ActivePlayer* p) {
	pthread_mutex_unlock(&p->lock);
}

ssize_t outbox_send(int fd, void* buf, size_t len) {
	assert(len == sizeof(InspectResponsePacket));
	g_sent[g_sent_count].fd = fd;
	memcpy(&g_sent[g_sent_count].pkt, buf, len);
	g_sent_count++;

	return (ssize_t)len;
}

// Put a loaded player in a slot at a position.
static void add_player(int slot, uint32_t id, int fd, float x, float y) {
	ActivePlayer* p = &active_players[slot];

	memset(p, 0, sizeof(*p));
	pthread_mutex_init(&p->lock, NULL);
	p->character_id = id;
	p->client_fd = fd;
	p->is_loaded = 1;
	p->pos_x = x;
	p->pos_y = y;
}

static void ask(int slot, uint32_t target) {
	InspectRequestPacket pkt;
	ActivePlayer* p = &active_players[slot];

	memset(&pkt, 0, sizeof(pkt));
	pkt.target_id = htonl(target);
	g_sent_count = 0;
	handle_inspect_request(p->client_fd, p->character_id, slot, &pkt);
}

// Give the player in a slot something worth reading back.
static void dress(int slot) {
	ActivePlayer* p = &active_players[slot];

	snprintf(p->username, sizeof(p->username), "Mirelle");
	p->level = 12;
	p->race_id = 4;
	p->appearance = 0x2;
	p->form = FORM_ANIMAL;
	p->resource_type = RESOURCE_STAMINA;
	p->max_health = 300;
	p->max_resource = 90;
	p->weapon_damage = 17;
	p->health = 111;
	p->resource = 55;
	p->experience = 987654;
	p->currency[0] = 500;
	for(int i = 0; i < STAT_COUNT; i++)
		p->stats[i] = 10 + i;
	p->equipment[EQUIP_HELMET].item_id = 1001;
	p->equipment[EQUIP_BOOTS].item_id = 1002;
	p->equipment[EQUIP_BLESSING].item_id = 1999;
}

static const InspectResponsePacket* only_reply(int fd) {
	assert(g_sent_count == 1);
	assert(g_sent[0].fd == fd);
	assert(g_sent[0].pkt.header.type == PACKET_INSPECT_RESPONSE);
	assert(ntohs(g_sent[0].pkt.header.payload_size) ==
	       sizeof(InspectResponsePacket) - sizeof(PacketHeader));

	return &g_sent[0].pkt;
}

// Every byte past the result is zero when there is nothing to show.
static void expect_empty(const InspectResponsePacket* r) {
	const uint8_t* body = (const uint8_t*)r + offsetof(InspectResponsePacket, form);
	size_t len = sizeof(*r) - offsetof(InspectResponsePacket, form);

	for(size_t i = 0; i < len; i++)
		assert(body[i] == 0);
}

static void test_in_range_returns_gear_and_stats(void) {
	const InspectResponsePacket* r;

	ask(0, 2);
	r = only_reply(100);
	assert(r->result == INSPECT_OK);
	assert(ntohl(r->target_id) == 2);
	assert(strcmp(r->name, "Mirelle") == 0);
	assert(ntohl(r->level) == 12);
	assert(ntohl(r->race_id) == 4);
	assert(ntohl(r->appearance) == 0x2);
	assert(r->form == FORM_ANIMAL);
	assert(r->resource_type == RESOURCE_STAMINA);
	assert((int32_t)ntohl((uint32_t)r->max_health) == 300);
	assert((int32_t)ntohl((uint32_t)r->max_resource) == 90);
	assert((int32_t)ntohl((uint32_t)r->weapon_damage) == 17);
	for(int i = 0; i < STAT_COUNT; i++)
		assert((int32_t)ntohl((uint32_t)r->stats[i]) == 10 + i);
	assert(ntohl(r->equipment[EQUIP_HELMET]) == 1001);
	assert(ntohl(r->equipment[EQUIP_BOOTS]) == 1002);
	assert(ntohl(r->equipment[EQUIP_BLESSING]) == 1999);
	assert(ntohl(r->equipment[EQUIP_CHEST]) == 0);
}

static void test_private_state_is_not_sent(void) {
	const uint8_t* raw;
	uint32_t needle;

	ask(0, 2);
	only_reply(100);
	raw = (const uint8_t*)&g_sent[0].pkt;

	// experience 987654, currency 500, health 111 and resource 55 stay home
	needle = htonl(987654);
	for(size_t i = 0; i + sizeof(needle) <= sizeof(InspectResponsePacket); i++)
		assert(memcmp(raw + i, &needle, sizeof(needle)) != 0);
	needle = htonl(500);
	for(size_t i = 0; i + sizeof(needle) <= sizeof(InspectResponsePacket); i++)
		assert(memcmp(raw + i, &needle, sizeof(needle)) != 0);
	needle = htonl(111);
	for(size_t i = 0; i + sizeof(needle) <= sizeof(InspectResponsePacket); i++)
		assert(memcmp(raw + i, &needle, sizeof(needle)) != 0);
}

static void test_range_edge(void) {
	ActivePlayer* target = &active_players[1];

	target->pos_x = INSPECT_RANGE;
	target->pos_y = 0.0f;
	ask(0, 2);
	assert(only_reply(100)->result == INSPECT_OK);

	target->pos_x = INSPECT_RANGE + 1.0f;
	ask(0, 2);
	assert(only_reply(100)->result == INSPECT_TOO_FAR);
	expect_empty(only_reply(100));

	// distance is measured on both axes
	target->pos_x = INSPECT_RANGE * 0.8f;
	target->pos_y = INSPECT_RANGE * 0.8f;
	ask(0, 2);
	assert(only_reply(100)->result == INSPECT_TOO_FAR);

	target->pos_x = 0.0f;
	target->pos_y = 0.0f;
}

static void test_unavailable_targets(void) {
	// nobody has that id
	ask(0, 999);
	assert(only_reply(100)->result == INSPECT_UNAVAILABLE);
	assert(ntohl(only_reply(100)->target_id) == 999);
	expect_empty(only_reply(100));

	// id zero is nobody
	ask(0, 0);
	assert(only_reply(100)->result == INSPECT_UNAVAILABLE);

	// yourself is not inspected
	ask(0, 1);
	assert(only_reply(100)->result == INSPECT_UNAVAILABLE);
	expect_empty(only_reply(100));

	// still being loaded
	active_players[1].is_loaded = 0;
	ask(0, 2);
	assert(only_reply(100)->result == INSPECT_UNAVAILABLE);
	expect_empty(only_reply(100));
	active_players[1].is_loaded = 1;
}

static void test_unknown_asker_is_ignored(void) {
	InspectRequestPacket pkt;

	memset(&pkt, 0, sizeof(pkt));
	pkt.target_id = htonl(2);
	g_sent_count = 0;

	// nobody is in slot 7, and slot 0 holds a different character
	handle_inspect_request(555, 4242, 7, &pkt);
	assert(g_sent_count == 0);
	handle_inspect_request(555, 4242, 0, &pkt);
	assert(g_sent_count == 0);
}

static void test_dead_target_is_readable(void) {
	active_players[1].is_dead = 1;
	ask(0, 2);
	assert(only_reply(100)->result == INSPECT_OK);
	active_players[1].is_dead = 0;
}

int main(void) {
	add_player(0, 1, 100, 0.0f, 0.0f);
	add_player(1, 2, 101, 0.0f, 0.0f);
	dress(1);

	test_in_range_returns_gear_and_stats();
	test_private_state_is_not_sent();
	test_range_edge();
	test_unavailable_targets();
	test_unknown_asker_is_ignored();
	test_dead_target_is_readable();
	printf("inspect_handler_test: OK\n");

	return 0;
}
