// Check how an inspect answer becomes a window, and what the player is told otherwise.

#include "states/inspect_client.h"
#include "ui/character_screen.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

// What the client sent, and what the server has said and not yet been read.
static uint32_t g_sent[8];
static int g_sent_count;
static InspectResponsePacket g_queue[8];
static int g_queued;
static int g_read;

// What the window was asked to do.
static int g_toggles;
static int g_places;
static float g_place_x;
static float g_place_y;

void network_send_inspect_request(uint32_t target_id) {
	g_sent[g_sent_count++] = target_id;
}

int network_get_inspect_response(InspectResponsePacket* out) {
	if(g_read >= g_queued)
		return 0;

	*out = g_queue[g_read++];

	return 1;
}

void character_screen_toggle(CharacterScreenState* cs) {
	cs->is_open = !cs->is_open;
	g_toggles++;
}

void character_screen_place(CharacterScreenState* cs, float x, float y) {
	cs->window_x = x;
	cs->window_y = y;
	g_places++;
	g_place_x = x;
	g_place_y = y;
}

static GameState* new_game(void) {
	GameState* game = calloc(1, sizeof(*game));

	g_sent_count = 0;
	g_queued = 0;
	g_read = 0;
	g_toggles = 0;
	g_places = 0;

	assert(game);
	game->playing = calloc(1, sizeof(*game->playing));
	game->character_screen = calloc(1, sizeof(*game->character_screen));
	game->inspect_screen = calloc(1, sizeof(*game->inspect_screen));
	assert(game->playing && game->character_screen && game->inspect_screen);
	game->character_screen->window_x = 100.0f;
	game->character_screen->window_y = 50.0f;
	game->character_screen->window_width = 400.0f;

	return game;
}

static void free_game(GameState* game) {
	free(game->inspect_screen);
	free(game->character_screen);
	free(game->playing);
	free(game);
}

// An answer as the server would write it.
static InspectResponsePacket answer(uint32_t target, uint8_t result) {
	InspectResponsePacket pkt;

	memset(&pkt, 0, sizeof(pkt));
	pkt.header.type = PACKET_INSPECT_RESPONSE;
	pkt.target_id = htonl(target);
	pkt.result = result;

	return pkt;
}

static InspectResponsePacket good_answer(uint32_t target) {
	InspectResponsePacket pkt = answer(target, INSPECT_OK);

	snprintf(pkt.name, sizeof(pkt.name), "Mirelle");
	pkt.level = htonl(12);
	pkt.race_id = htonl(4);
	pkt.form = FORM_ANIMAL;
	pkt.weapon_damage = (int32_t)htonl(17);
	pkt.equipment[EQUIP_HELMET] = htonl(1001);
	pkt.equipment[EQUIP_BLESSING] = htonl(1999);
	for(int i = 0; i < STAT_COUNT; i++)
		pkt.stats[i] = (int32_t)htonl((uint32_t)(10 + i));

	return pkt;
}

static void hear(InspectResponsePacket pkt) {
	g_queue[g_queued++] = pkt;
}

static const ChatLine* newest(GameState* game) {
	ChatState* chat = &game->playing->chat;

	assert(chat->line_count > 0);

	return &chat->lines[chat->line_count - 1];
}

static void test_decode(void) {
	InspectResponsePacket pkt = good_answer(7);
	CharacterView view;

	assert(character_view_from_inspect(&pkt, &view));
	assert(view.loaded);
	assert(strcmp(view.name, "Mirelle") == 0);
	assert(view.level == 12 && view.race_id == 4 && view.form == FORM_ANIMAL);
	assert(view.weapon_damage == 17);
	assert(view.equipment[EQUIP_HELMET] == 1001);
	assert(view.equipment[EQUIP_BLESSING] == 1999);
	assert(view.equipment[EQUIP_CHEST] == 0);
	for(int i = 0; i < STAT_COUNT; i++)
		assert(view.stats[i] == 10 + i);

	// a negative stat survives the round trip
	pkt.stats[0] = (int32_t)htonl((uint32_t)-5);
	assert(character_view_from_inspect(&pkt, &view));
	assert(view.stats[0] == -5);
}

static void test_decode_refuses(void) {
	CharacterView view;
	InspectResponsePacket pkt = good_answer(7);

	memset(&view, 0x5a, sizeof(view));
	pkt.result = INSPECT_TOO_FAR;
	assert(!character_view_from_inspect(&pkt, &view));
	pkt.result = INSPECT_UNAVAILABLE;
	assert(!character_view_from_inspect(&pkt, &view));

	// a form the client has never heard of
	pkt = good_answer(7);
	pkt.form = FORM_COUNT;
	assert(!character_view_from_inspect(&pkt, &view));

	// nothing was written
	assert(((uint8_t*)&view)[0] == 0x5a);
}

static void test_name_is_always_ended(void) {
	InspectResponsePacket pkt = good_answer(7);
	CharacterView view;

	memset(pkt.name, 'x', sizeof(pkt.name));
	assert(character_view_from_inspect(&pkt, &view));
	assert(strlen(view.name) == sizeof(view.name) - 1);
}

static void test_no_target(void) {
	GameState* game = new_game();

	inspect_client_request_target(game);
	assert(g_sent_count == 0);
	assert(!game->playing->inspect.pending);
	assert(newest(game)->channel == CHAT_CHANNEL_SYSTEM);
	assert(strstr(newest(game)->text, "Target a player") != NULL);
	free_game(game);
}

static void test_answer_opens_window(void) {
	GameState* game = new_game();

	game->playing->target_player_id = 7;
	inspect_client_request_target(game);
	assert(g_sent_count == 1 && g_sent[0] == 7);
	assert(game->playing->inspect.pending);

	// nothing yet: nothing happens
	inspect_client_poll(game);
	assert(!game->inspect_screen->is_open);

	hear(good_answer(7));
	inspect_client_poll(game);
	assert(!game->playing->inspect.pending);
	assert(game->inspect_screen->is_open);
	assert(game->playing->inspect.view.loaded);
	assert(game->playing->inspect.view.equipment[EQUIP_HELMET] == 1001);
	assert(g_places == 0);
	free_game(game);
}

static void test_opens_beside_the_character_window(void) {
	GameState* game = new_game();

	game->character_screen->is_open = 1;
	game->playing->target_player_id = 7;
	inspect_client_request_target(game);
	hear(good_answer(7));
	inspect_client_poll(game);
	assert(game->inspect_screen->is_open);
	assert(g_places == 1);
	assert(g_place_x > 100.0f + 400.0f && g_place_y == 50.0f);
	free_game(game);
}

static void test_second_answer_updates_an_open_window(void) {
	GameState* game = new_game();

	game->playing->target_player_id = 7;
	inspect_client_request_target(game);
	hear(good_answer(7));
	inspect_client_poll(game);
	assert(g_toggles == 1);

	// inspect another player: the same window, new contents, not toggled shut
	game->playing->target_player_id = 8;
	inspect_client_request_target(game);
	hear(good_answer(8));
	hear(good_answer(8));
	g_queue[g_queued - 1].level = htonl(30);
	inspect_client_poll(game);
	assert(g_toggles == 1 && game->inspect_screen->is_open);
	free_game(game);
}

static void test_refusals_are_told(void) {
	GameState* game = new_game();

	game->playing->target_player_id = 7;
	inspect_client_request_target(game);
	hear(answer(7, INSPECT_TOO_FAR));
	inspect_client_poll(game);
	assert(!game->inspect_screen->is_open);
	assert(!game->playing->inspect.pending);
	assert(strstr(newest(game)->text, "too far") != NULL);

	inspect_client_request_target(game);
	hear(answer(7, INSPECT_UNAVAILABLE));
	inspect_client_poll(game);
	assert(strstr(newest(game)->text, "can't inspect") != NULL);
	assert(!game->inspect_screen->is_open);
	free_game(game);
}

static void test_stale_answers_are_ignored(void) {
	GameState* game = new_game();

	// an answer nobody asked for
	hear(good_answer(7));
	inspect_client_poll(game);
	assert(!game->inspect_screen->is_open);

	// an answer about someone else than the latest question
	game->playing->target_player_id = 8;
	inspect_client_request_target(game);
	hear(good_answer(7));
	inspect_client_poll(game);
	assert(!game->inspect_screen->is_open);
	assert(game->playing->inspect.pending);
	free_game(game);
}

static void test_close_forgets_the_question(void) {
	GameState* game = new_game();

	game->playing->target_player_id = 7;
	inspect_client_request_target(game);
	hear(good_answer(7));
	inspect_client_poll(game);
	assert(game->inspect_screen->is_open);

	inspect_client_close(game);
	assert(!game->inspect_screen->is_open);

	// closing with a question in the air means its answer is not wanted
	inspect_client_request_target(game);
	inspect_client_close(game);
	hear(good_answer(7));
	inspect_client_poll(game);
	assert(!game->inspect_screen->is_open);
	free_game(game);
}

int main(void) {
	test_decode();
	test_decode_refuses();
	test_name_is_always_ended();
	test_no_target();
	test_answer_opens_window();
	test_opens_beside_the_character_window();
	test_second_answer_updates_an_open_window();
	test_refusals_are_told();
	test_stale_answers_are_ignored();
	test_close_forgets_the_question();
	printf("inspect_client_test: OK\n");

	return 0;
}
