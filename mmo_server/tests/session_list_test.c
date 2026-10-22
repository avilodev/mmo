// Check the online roster page byte for byte: framing, entries, status.

#define _POSIX_C_SOURCE 200809L

#include "player_directory.h"
#include "status_rules.h"
#include "player_data.h"

#include <arpa/inet.h>
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// The player table, stubbed.
ActivePlayer active_players[MAX_PLAYERS];
static int g_online[MAX_PLAYERS];
static int g_online_count;
static double g_now = 10000.0;

// What the server sent.
static uint8_t g_sent[sizeof(SessionListResponsePacket)];
static size_t g_sent_len;

double get_time_seconds(void) {
	return g_now;
}

void log_emit(int level, const char* file, int line, const char* fmt, ...) {
	(void)level;
	(void)file;
	(void)line;
	(void)fmt;
}

int log_level_enabled(int level) {
	(void)level;

	return 0;
}

void player_registry_rdlock(void) {
}

void player_registry_unlock(void) {
}

const int* player_active_list_locked(int* out_count) {
	*out_count = g_online_count;

	return g_online;
}

ActivePlayer* player_acquire(uint32_t character_id) {
	(void)character_id;

	return NULL;
}

ActivePlayer* player_acquire_hint(uint32_t character_id, int slot) {
	(void)character_id;
	(void)slot;

	return NULL;
}

void player_release(ActivePlayer* p) {
	(void)p;
}

void player_send_data_response(int fd, uint32_t character_id) {
	(void)fd;
	(void)character_id;
}

ssize_t outbox_send(int fd, void* buf, size_t len) {
	(void)fd;
	assert(len <= sizeof(g_sent));
	memcpy(g_sent, buf, len);
	g_sent_len = len;

	return (ssize_t)len;
}

static void add_player(uint32_t id, const char* name, int level, int race,
		int ping, double last_activity, int manual) {
	int slot = g_online_count;
	ActivePlayer* p = &active_players[slot];

	pthread_mutex_init(&p->lock, NULL);
	p->is_loaded = 1;
	p->is_ready = 1;
	p->character_id = id;
	snprintf(p->username, sizeof(p->username), "%s", name);
	p->level = level;
	p->race_id = race;
	p->ping_ms = (uint16_t)ping;
	p->last_activity_at = last_activity;
	p->status_choice = (uint8_t)manual;
	g_online[g_online_count++] = slot;
}

static void reset(void) {
	memset(active_players, 0, sizeof(active_players));
	g_online_count = 0;
	g_sent_len = 0;
}

// The header must describe the bytes that follow it, exactly.
static const SessionListResponsePacket* ask(uint16_t page) {
	const SessionListResponsePacket* pkt = (const SessionListResponsePacket*)g_sent;

	player_directory_session_page(9, page);
	assert(g_sent_len >= offsetof(SessionListResponsePacket, entries));
	assert(g_sent[0] == PACKET_SESSION_LIST_RESPONSE);
	assert(ntohs(pkt->header.payload_size) == g_sent_len - sizeof(PacketHeader));
	assert(g_sent_len == offsetof(SessionListResponsePacket, entries) +
	       (size_t)pkt->count * sizeof(SessionPlayerEntry));

	return pkt;
}

static void test_empty(void) {
	const SessionListResponsePacket* pkt;

	reset();
	pkt = ask(0);
	assert(pkt->count == 0);
	assert(ntohl(pkt->total_players) == 0);
}

static void test_entries_and_status(void) {
	const SessionListResponsePacket* pkt;

	reset();
	add_player(101, "Alice", 12, 3, 45, g_now, 0);
	add_player(102, "Bob", 255, 7, 300, g_now - 600.0, 0);
	add_player(103, "Cara", 1, 1, 0, g_now, SESSION_STATUS_AFK);
	add_player(104, "Dan", 5, 2, 20, g_now - 600.0, SESSION_STATUS_DND);
	add_player(105, "Eve", 6, 4, 30, g_now, SESSION_STATUS_OFFLINE);
	pkt = ask(0);

	assert(pkt->count == 4);
	assert(ntohl(pkt->total_players) == 4);
	assert(ntohs(pkt->total_pages) == 1);
	assert(ntohs(pkt->current_page) == 0);

	assert(ntohl(pkt->entries[0].player_id) == 101);
	assert(strcmp(pkt->entries[0].name, "Alice") == 0);
	assert(pkt->entries[0].level == 12);
	assert(pkt->entries[0].player_race == 3);
	assert(pkt->entries[0].status == SESSION_STATUS_ONLINE);
	assert(ntohs(pkt->entries[0].ping_ms) == 45);

	assert(pkt->entries[1].level == 255);
	assert(pkt->entries[1].status == SESSION_STATUS_AFK);
	assert(ntohs(pkt->entries[1].ping_ms) == 300);

	assert(pkt->entries[2].status == SESSION_STATUS_AFK);
	assert(ntohs(pkt->entries[2].ping_ms) == 0);

	// a chosen status shows even for a player who has gone idle
	assert(pkt->entries[3].status == SESSION_STATUS_DND);

	// Eve is appearing Offline: nobody sees her, and she is not counted
	for(int i = 0; i < pkt->count; i++)
		assert(strcmp(pkt->entries[i].name, "Eve") != 0);
}

static void test_offline_is_skipped_across_pages(void) {
	const SessionListResponsePacket* pkt;

	reset();
	// Offline players sit between visible ones and must not take up page space
	for(int i = 0; i < SESSION_LIST_PAGE_SIZE + 1; i++) {
		add_player(2000u + (uint32_t)i, "V", 1, 1, 1, g_now, SESSION_STATUS_ONLINE);
		add_player(3000u + (uint32_t)i, "H", 1, 1, 1, g_now, SESSION_STATUS_OFFLINE);
	}

	pkt = ask(0);
	assert(pkt->count == SESSION_LIST_PAGE_SIZE);
	assert(ntohl(pkt->total_players) == SESSION_LIST_PAGE_SIZE + 1);
	assert(ntohs(pkt->total_pages) == 2);
	for(int i = 0; i < pkt->count; i++)
		assert(pkt->entries[i].status != SESSION_STATUS_OFFLINE);

	pkt = ask(1);
	assert(pkt->count == 1);
	assert(pkt->entries[0].status == SESSION_STATUS_ONLINE);
}

static void test_everyone_offline(void) {
	const SessionListResponsePacket* pkt;

	reset();
	add_player(1, "H", 1, 1, 1, g_now, SESSION_STATUS_OFFLINE);
	pkt = ask(0);
	assert(pkt->count == 0);
	assert(ntohl(pkt->total_players) == 0);
	assert(ntohs(pkt->total_pages) == 1);
}

static void test_pages(void) {
	const SessionListResponsePacket* pkt;

	reset();
	for(int i = 0; i < SESSION_LIST_PAGE_SIZE + 1; i++)
		add_player(1000u + (uint32_t)i, "P", 1, 1, 1, g_now, 0);

	pkt = ask(0);
	assert(pkt->count == SESSION_LIST_PAGE_SIZE);
	assert(ntohs(pkt->total_pages) == 2);

	pkt = ask(1);
	assert(pkt->count == 1);
	assert(ntohs(pkt->current_page) == 1);
	assert(ntohl(pkt->entries[0].player_id) == 1000u + SESSION_LIST_PAGE_SIZE);
}

int main(void) {
	test_empty();
	test_entries_and_status();
	test_pages();
	test_offline_is_skipped_across_pages();
	test_everyone_offline();
	printf("session_list_test: OK\n");

	return 0;
}
