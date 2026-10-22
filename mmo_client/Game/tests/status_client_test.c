// Check the status names and what the player is told when their status changes.

#include "states/status_client.h"
#include "core/session_status.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// What the server has said and not yet been read.
static uint8_t g_queue[8];
static int g_queued;
static int g_read;

int network_get_status_state(uint8_t* status) {
	if(g_read >= g_queued)
		return 0;

	*status = g_queue[g_read++];

	return 1;
}

static GameState* new_game(void) {
	GameState* game = calloc(1, sizeof(*game));

	g_queued = 0;
	g_read = 0;

	assert(game);
	game->playing = calloc(1, sizeof(*game->playing));
	assert(game->playing);

	return game;
}

static void say(uint8_t status) {
	g_queue[g_queued++] = status;
}

static const ChatLine* newest(GameState* game) {
	ChatState* chat = &game->playing->chat;

	assert(chat->line_count > 0);

	return &chat->lines[chat->line_count - 1];
}

static void test_labels(void) {
	assert(strcmp(session_status_label(SESSION_STATUS_ONLINE), "Online") == 0);
	assert(strcmp(session_status_label(SESSION_STATUS_AFK), "AFK") == 0);
	assert(strcmp(session_status_label(SESSION_STATUS_DND), "Do Not Disturb") == 0);
	assert(strcmp(session_status_label(SESSION_STATUS_OFFLINE), "Offline") == 0);
	assert(strcmp(session_status_label(SESSION_STATUS_COUNT), "Unknown") == 0);
	assert(strcmp(session_status_label(255), "Unknown") == 0);
}

static void test_parse(void) {
	uint8_t status = 99;

	assert(session_status_parse("online", 6, &status) && status == SESSION_STATUS_ONLINE);
	assert(session_status_parse("AFK", 3, &status) && status == SESSION_STATUS_AFK);
	assert(session_status_parse("Dnd", 3, &status) && status == SESSION_STATUS_DND);
	assert(session_status_parse("OFFLINE", 7, &status) && status == SESSION_STATUS_OFFLINE);

	status = 99;
	assert(!session_status_parse("away", 4, &status) && status == 99);
	assert(!session_status_parse("onlin", 5, &status));
	assert(!session_status_parse("onlinex", 7, &status));
	assert(!session_status_parse("", 0, &status));
	assert(!session_status_parse("dnd", 2, &status));
}

static void test_messages(void) {
	GameState* game = new_game();

	status_client_poll(game);
	assert(game->playing->chat.line_count == 0);

	say(SESSION_STATUS_DND);
	status_client_poll(game);
	assert(game->playing->own_status == SESSION_STATUS_DND);
	assert(strcmp(newest(game)->text, "You are now Do Not Disturb.") == 0);

	say(SESSION_STATUS_AFK);
	status_client_poll(game);
	assert(strcmp(newest(game)->text, "You are now AFK.") == 0);

	// back from AFK has its own wording
	say(SESSION_STATUS_ONLINE);
	status_client_poll(game);
	assert(game->playing->own_status == SESSION_STATUS_ONLINE);
	assert(strcmp(newest(game)->text, "You are no longer AFK.") == 0);

	say(SESSION_STATUS_OFFLINE);
	status_client_poll(game);
	assert(strcmp(newest(game)->text, "You are now Offline.") == 0);

	say(SESSION_STATUS_ONLINE);
	status_client_poll(game);
	assert(strcmp(newest(game)->text, "You are now Online.") == 0);

	free(game->playing);
	free(game);
}

static void test_repeats_and_batches(void) {
	GameState* game = new_game();
	int before;

	// the same status again says nothing
	say(SESSION_STATUS_DND);
	status_client_poll(game);
	before = game->playing->chat.line_count;
	say(SESSION_STATUS_DND);
	status_client_poll(game);
	assert(game->playing->chat.line_count == before);

	// two changes between frames are both reported, in order
	say(SESSION_STATUS_AFK);
	say(SESSION_STATUS_OFFLINE);
	status_client_poll(game);
	assert(game->playing->chat.line_count == before + 2);
	assert(game->playing->own_status == SESSION_STATUS_OFFLINE);

	free(game->playing);
	free(game);
}

static void test_no_playing_state(void) {
	GameState game;

	g_queued = 0;
	g_read = 0;
	memset(&game, 0, sizeof(game));
	say(SESSION_STATUS_DND);
	status_client_poll(&game);
}

int main(void) {
	test_labels();
	test_parse();
	test_messages();
	test_repeats_and_batches();
	test_no_playing_state();
	printf("status_client_test: OK\n");

	return 0;
}
