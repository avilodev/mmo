// Drive the status handler against a stub player table and check its answers.

#define _POSIX_C_SOURCE 200809L

#include "status_handler.h"
#include "status_rules.h"
#include "friends.h"
#include "player_data.h"

#include <assert.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SLOT 3
#define ID 77u
#define ACCOUNT 5500u
#define FD 9

// The player table, stubbed.
ActivePlayer active_players[MAX_PLAYERS];
static double g_now = 1000.0;

// What the server sent.
static StatusStatePacket g_sent[16];
static int g_sent_count;

double get_time_seconds(void) {
	return g_now;
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
	assert(fd == FD);
	assert(len == sizeof(StatusStatePacket));
	memcpy(&g_sent[g_sent_count++], buf, len);

	return (ssize_t)len;
}

// What the handler asked friends to do about being seen.
static int g_visible_calls;
static uint32_t g_visible_account;
static uint32_t g_visible_character;
static char g_visible_name[32];
static int g_visible_last;

void world_friends_set_visible(uint32_t account_id, uint32_t character_id,
		const char* character_name, int visible) {
	g_visible_calls++;
	g_visible_account = account_id;
	g_visible_character = character_id;
	snprintf(g_visible_name, sizeof(g_visible_name), "%s", character_name);
	g_visible_last = visible;
}

static void reset(void) {
	memset(active_players, 0, sizeof(active_players));
	for(int i = 0; i < MAX_PLAYERS; i++)
		pthread_mutex_init(&active_players[i].lock, NULL);
	active_players[SLOT].is_loaded = 1;
	active_players[SLOT].character_id = ID;
	active_players[SLOT].last_activity_at = g_now;
	active_players[SLOT].account_id = ACCOUNT;
	snprintf(active_players[SLOT].username, sizeof(active_players[SLOT].username), "Ayla");
	g_sent_count = 0;
	g_visible_calls = 0;
}

static uint8_t status_now(void) {
	const ActivePlayer* p = &active_players[SLOT];

	return status_effective(p->status_choice, p->last_activity_at, g_now);
}

static void choose(uint8_t status) {
	status_handle_request(FD, ID, SLOT, status);
}

static void test_activity_while_online_says_nothing(void) {
	reset();
	g_now += 10.0;
	status_note_activity(FD, ID, SLOT);
	assert(g_sent_count == 0);
	assert(active_players[SLOT].last_activity_at == g_now);
}

static void test_choose_each_status(void) {
	reset();
	choose(SESSION_STATUS_AFK);
	assert(status_now() == SESSION_STATUS_AFK);
	assert(g_sent_count == 1 && g_sent[0].status == SESSION_STATUS_AFK);
	assert(g_sent[0].header.type == PACKET_STATUS_STATE);
	assert(ntohl(g_sent[0].header.player_id) == ID);

	choose(SESSION_STATUS_DND);
	assert(status_now() == SESSION_STATUS_DND);
	assert(g_sent[1].status == SESSION_STATUS_DND);

	choose(SESSION_STATUS_OFFLINE);
	assert(status_now() == SESSION_STATUS_OFFLINE);
	assert(g_sent[2].status == SESSION_STATUS_OFFLINE);

	choose(SESSION_STATUS_ONLINE);
	assert(status_now() == SESSION_STATUS_ONLINE);
	assert(g_sent[3].status == SESSION_STATUS_ONLINE);
	assert(g_sent_count == 4);
}

static void test_acting_ends_chosen_afk(void) {
	reset();
	choose(SESSION_STATUS_AFK);
	g_sent_count = 0;
	g_now += 5.0;
	status_note_activity(FD, ID, SLOT);
	assert(status_now() == SESSION_STATUS_ONLINE);
	assert(g_sent_count == 1 && g_sent[0].status == SESSION_STATUS_ONLINE);
}

static void test_acting_ends_idle_afk(void) {
	reset();
	g_now += AFK_IDLE_SECONDS + 1.0;
	assert(status_now() == SESSION_STATUS_AFK);
	status_note_activity(FD, ID, SLOT);
	assert(status_now() == SESSION_STATUS_ONLINE);
	assert(g_sent_count == 1 && g_sent[0].status == SESSION_STATUS_ONLINE);
}

static void test_acting_keeps_dnd_and_offline(void) {
	reset();
	choose(SESSION_STATUS_DND);
	g_sent_count = 0;
	g_now += 5.0;
	status_note_activity(FD, ID, SLOT);
	assert(status_now() == SESSION_STATUS_DND);
	assert(g_sent_count == 0);

	choose(SESSION_STATUS_OFFLINE);
	g_sent_count = 0;
	status_note_activity(FD, ID, SLOT);
	assert(status_now() == SESSION_STATUS_OFFLINE);
	assert(g_sent_count == 0);
}

static void test_idle_keeps_dnd(void) {
	reset();
	choose(SESSION_STATUS_DND);
	g_now += AFK_IDLE_SECONDS * 3.0;
	assert(status_now() == SESSION_STATUS_DND);
}

static void test_online_restarts_the_idle_clock(void) {
	reset();
	choose(SESSION_STATUS_AFK);
	g_now += AFK_IDLE_SECONDS * 3.0;
	choose(SESSION_STATUS_ONLINE);
	assert(status_now() == SESSION_STATUS_ONLINE);
}

static void test_choosing_afk_while_idle(void) {
	reset();
	g_now += AFK_IDLE_SECONDS + 1.0;
	choose(SESSION_STATUS_AFK);
	assert(active_players[SLOT].status_choice == SESSION_STATUS_AFK);
	assert(g_sent_count == 1 && g_sent[0].status == SESSION_STATUS_AFK);
}

static void test_bad_status_is_ignored(void) {
	reset();
	choose(SESSION_STATUS_COUNT);
	choose(255);
	assert(g_sent_count == 0);
	assert(active_players[SLOT].status_choice == SESSION_STATUS_ONLINE);
}

static void test_unknown_player(void) {
	reset();
	status_note_activity(FD, ID + 1, SLOT);
	status_handle_request(FD, ID + 1, SLOT, SESSION_STATUS_DND);
	assert(g_sent_count == 0);
	assert(active_players[SLOT].status_choice == SESSION_STATUS_ONLINE);
}

static void test_offline_hides_from_friends(void) {
	reset();
	choose(SESSION_STATUS_OFFLINE);
	assert(g_visible_calls == 1);
	assert(g_visible_last == 0);
	assert(g_visible_account == ACCOUNT);
	assert(g_visible_character == ID);
	assert(strcmp(g_visible_name, "Ayla") == 0);

	// still Offline: nothing more to tell friends
	choose(SESSION_STATUS_OFFLINE);
	assert(g_visible_calls == 1);

	// acting does not bring them back
	status_note_activity(FD, ID, SLOT);
	assert(g_visible_calls == 1);

	// each way out shows them again, once
	choose(SESSION_STATUS_DND);
	assert(g_visible_calls == 2);
	assert(g_visible_last == 1);
	choose(SESSION_STATUS_ONLINE);
	assert(g_visible_calls == 2);
}

static void test_other_changes_leave_friends_alone(void) {
	reset();
	choose(SESSION_STATUS_AFK);
	choose(SESSION_STATUS_DND);
	choose(SESSION_STATUS_ONLINE);
	choose(SESSION_STATUS_COUNT);
	choose(255);
	assert(g_visible_calls == 0);
}

static void test_offline_to_afk_shows_again(void) {
	reset();
	choose(SESSION_STATUS_OFFLINE);
	choose(SESSION_STATUS_AFK);
	assert(g_visible_calls == 2 && g_visible_last == 1);
}

static void test_bad_status_does_not_hide(void) {
	reset();
	choose(SESSION_STATUS_OFFLINE);
	choose(200);
	assert(g_visible_calls == 1);
	assert(active_players[SLOT].status_choice == SESSION_STATUS_OFFLINE);
}

int main(void) {
	test_activity_while_online_says_nothing();
	test_choose_each_status();
	test_acting_ends_chosen_afk();
	test_acting_ends_idle_afk();
	test_acting_keeps_dnd_and_offline();
	test_idle_keeps_dnd();
	test_online_restarts_the_idle_clock();
	test_choosing_afk_while_idle();
	test_bad_status_is_ignored();
	test_unknown_player();
	test_offline_hides_from_friends();
	test_other_changes_leave_friends_alone();
	test_offline_to_afk_shows_again();
	test_bad_status_does_not_hide();
	printf("status_handler_test: OK\n");

	return 0;
}
