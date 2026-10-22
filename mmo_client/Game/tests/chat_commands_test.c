// Check the chat command table; the network and emote calls are stubbed.

#include "states/chat_commands.h"
#include "states/emote_client.h"
#include "world/world_clock.h"
#include "world/weather_now.h"
#include "world/weather_state.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// What the stubs last heard.
static char g_call[64];
static char g_arg[512];
static char g_target[64];
static int g_channel;
static int g_flag;
static int g_calls;
static const emote_def* g_requested;
static int g_stops;

static void heard(const char* call, const char* arg) {
	snprintf(g_call, sizeof(g_call), "%s", call);
	snprintf(g_arg, sizeof(g_arg), "%s", arg ? arg : "");
	g_calls++;
}

void network_send_party_invite(const char* name) {
	heard("invite", name);
}

void network_send_party_leave(void) {
	heard("leave", NULL);
}

void network_send_status_request(uint8_t status) {
	char digits[8];

	snprintf(digits, sizeof(digits), "%u", (unsigned)status);
	heard("status", digits);
}

void network_send_friend_request(const char* name) {
	heard("friend", name);
}

void network_send_friend_remove(const char* name) {
	heard("unfriend", name);
}

void network_send_friend_block(const char* name, int block) {
	heard("block", name);
	g_flag = block;
}

void network_send_chat(uint8_t channel, const char* target, const char* message) {
	heard("chat", message);
	snprintf(g_target, sizeof(g_target), "%s", target);
	g_channel = channel;
}

static int g_connected;
static int g_ping_requests;

int network_is_connected(void) {
	return g_connected;
}

void network_request_ping_report(void) {
	heard("ping", NULL);
	g_ping_requests++;
}

void emote_client_request(GameState* game, const emote_def* def) {
	(void)game;
	g_requested = def;
	g_calls++;
}

void emote_client_stop(GameState* game) {
	(void)game;
	g_stops++;
	g_calls++;
}

static GameState* new_game(void) {
	GameState* game = calloc(1, sizeof(*game));

	assert(game);
	game->playing = calloc(1, sizeof(*game->playing));
	assert(game->playing);

	return game;
}

// Types the line; returns the number of calls it made.
static int type(GameState* game, const char* line, int* handled) {
	g_calls = 0;
	g_call[0] = '\0';
	g_arg[0] = '\0';
	g_target[0] = '\0';
	*handled = chat_command_dispatch(game, line);

	return g_calls;
}

static void expect(GameState* game, const char* line, const char* call,
		const char* arg) {
	int handled;

	assert(type(game, line, &handled) == 1);
	assert(handled == 1);
	assert(strcmp(g_call, call) == 0);
	assert(strcmp(g_arg, arg) == 0);
}

// The newest line in the chat box.
static const ChatLine* newest(GameState* game) {
	ChatState* chat = &game->playing->chat;

	assert(chat->line_count > 0);

	return &chat->lines[chat->line_count - 1];
}

// A command line that makes no call, is never sent as chat, and says why.
static void expect_dropped(GameState* game, const char* line) {
	int before = game->playing->chat.line_count;
	int handled;

	assert(type(game, line, &handled) == 0);
	assert(handled == 1);
	assert(game->playing->chat.line_count == before + 1);
	assert(newest(game)->style == CHAT_STYLE_ERROR);
	assert(newest(game)->channel == CHAT_CHANNEL_SYSTEM);
}

// As expect_dropped, and the reported line contains this text.
static void expect_reported(GameState* game, const char* line, const char* text) {
	expect_dropped(game, line);
	assert(strstr(newest(game)->text, text) != NULL);
}

static void test_existing_commands(GameState* game) {
	expect(game, "/invite bob", "invite", "bob");
	expect(game, "/leave", "leave", "");
	// /afk flips between AFK and Online, going by what the server last said
	game->playing->own_status = SESSION_STATUS_ONLINE;
	expect(game, "/afk", "status", "1");
	game->playing->own_status = SESSION_STATUS_AFK;
	expect(game, "/afk", "status", "0");
	game->playing->own_status = SESSION_STATUS_DND;
	expect(game, "/afk", "status", "1");

	expect(game, "/status online", "status", "0");
	expect(game, "/status afk", "status", "1");
	expect(game, "/status dnd", "status", "2");
	expect(game, "/status offline", "status", "3");
	expect(game, "/status DND", "status", "2");
	expect(game, "/status  Offline", "status", "3");
	expect(game, "/friend ann", "friend", "ann");
	expect(game, "/unfriend ann", "unfriend", "ann");
	expect(game, "/block eve", "block", "eve");
	assert(g_flag == 1);
	expect(game, "/unblock eve", "block", "eve");
	assert(g_flag == 0);

	expect(game, "/g hello all", "chat", "hello all");
	assert(g_channel == 1 && g_target[0] == '\0');
	expect(game, "/p hi party", "chat", "hi party");
	assert(g_channel == 3 && g_target[0] == '\0');

	// a whisper's first word is its target, sent apart from its text
	expect(game, "/w bob psst", "chat", "psst");
	assert(g_channel == 2 && strcmp(g_target, "bob") == 0);
	expect(game, "/w  bob   two  words", "chat", "two  words");
	assert(strcmp(g_target, "bob") == 0);

	// the argument is everything after the first space, verbatim
	expect(game, "/invite  bob", "invite", " bob");
}

static void test_reply(GameState* game) {
	char* target = game->playing->chat.whisper_reply_target;

	snprintf(target, sizeof(game->playing->chat.whisper_reply_target), "%s", "ann");
	expect(game, "/r thanks", "chat", "thanks");
	assert(g_channel == 2 && strcmp(g_target, "ann") == 0);

	// nobody has whispered us
	target[0] = '\0';
	expect_reported(game, "/r thanks", "Nobody has whispered");
}

static void test_dropped_lines(GameState* game) {
	expect_reported(game, "/invite", "Usage: /invite name");
	expect_reported(game, "/invite ", "Usage: /invite name");
	expect_reported(game, "/leave now", "Usage: /leave");
	expect_reported(game, "/afk now", "Usage: /afk");
	expect_reported(game, "/status", "Usage: /status online|afk|dnd|offline");
	expect_reported(game, "/status away", "Usage: /status online|afk|dnd|offline");
	expect_reported(game, "/status dnd now", "Usage: /status online|afk|dnd|offline");
	expect_reported(game, "/g", "Usage: /g message");
	expect_reported(game, "/gx hi", "Unknown command: /gx");
	expect_reported(game, "/nosuchthing", "Unknown command: /nosuchthing");
	expect_reported(game, "/", "Unknown command");
	expect_reported(game, "/ g hi", "Unknown command");
	expect_reported(game, "/averyveryverylongcommandword", "Unknown command");

	// a whisper needs a target and some text
	expect_reported(game, "/w", "Usage: /w name message");
	expect_reported(game, "/w bob", "Usage: /w name message");
	expect_reported(game, "/w   ", "Usage: /w name message");
}

static void test_ordinary_chat(GameState* game) {
	int handled;

	assert(type(game, "hello there", &handled) == 0 && handled == 0);
	assert(type(game, "", &handled) == 0 && handled == 0);
	assert(type(game, NULL, &handled) == 0 && handled == 0);
	assert(type(game, " /g hi", &handled) == 0 && handled == 0);
}

static void test_emotes(GameState* game) {
	int handled;

	// every emote in the table has a command, and nothing else was needed
	for(int id = EMOTE_NONE + 1; id < EMOTE_COUNT; id++) {
		const emote_def* def = emote_lookup((uint8_t)id);
		char line[64];

		assert(def);
		snprintf(line, sizeof(line), "/%s", def->command);
		g_requested = NULL;
		assert(type(game, line, &handled) == 1 && handled == 1);
		assert(g_requested == def);

		// they take no arguments
		snprintf(line, sizeof(line), "/%s now", def->command);
		g_requested = NULL;
		expect_reported(game, line, "Usage: ");
		assert(g_requested == NULL);
	}

	g_requested = NULL;
	g_stops = 0;
	assert(type(game, "/stand", &handled) == 1 && handled == 1 && g_stops == 1);
	expect_reported(game, "/stand up", "Usage: /stand");
	assert(g_stops == 1);

	// a word that is neither a command nor an emote is reported
	expect_reported(game, "/sitt", "Unknown command: /sitt");
	assert(g_requested == NULL);

	// case is ignored, for commands and for emotes
	expect(game, "/INVITE bob", "invite", "bob");
	assert(type(game, "/Sit", &handled) == 1 && handled == 1);
	assert(g_requested != NULL && g_requested == emote_find_command("sit", 3));
}

static void test_ping(GameState* game) {
	int handled;

	g_connected = 1;
	g_ping_requests = 0;
	expect(game, "/ping", "ping", "");
	assert(g_ping_requests == 1);
	expect(game, "/PING", "ping", "");
	assert(g_ping_requests == 2);

	// no link, nothing to measure: say so rather than wait for an answer
	g_connected = 0;
	g_ping_requests = 0;
	expect_reported(game, "/ping", "Not connected");
	assert(g_ping_requests == 0);

	g_connected = 1;
	expect_reported(game, "/ping now", "Usage: /ping");
	assert(type(game, "/ping now", &handled) == 0);
	assert(g_ping_requests == 0);
}

static void test_time(GameState* game) {
	ChatState* chat = &game->playing->chat;
	int before;
	int handled;

	// before the server has given the time, say so
	world_clock_reset();
	expect_reported(game, "/time", "not known");

	// 2:30 in the afternoon, by the player's choice of clock
	world_clock_sync(1631250, 2700000, 10.0);
	world_clock_tick(10.0);
	game->settings.clock_24h = 0;
	before = chat->line_count;
	assert(type(game, "/time", &handled) == 0 && handled == 1);
	assert(chat->line_count == before + 1);
	assert(newest(game)->style == CHAT_STYLE_SYSTEM);
	assert(newest(game)->channel == CHAT_CHANNEL_SYSTEM);
	assert(strstr(newest(game)->text, "2:30 PM") != NULL);

	game->settings.clock_24h = 1;
	assert(type(game, "/TIME", &handled) == 0 && handled == 1);
	assert(strstr(newest(game)->text, "14:30") != NULL);

	expect_reported(game, "/time now", "Usage: /time");
	world_clock_reset();
}

// Says a /weather line and returns the day it forced, with 0 when none is
static int weather_forced(GameState* game, const char* line, WeatherDayData* day) {
	int handled = 0;

	weather_now_force(-1);
	type(game, line, &handled);
	assert(handled);

	return weather_now_forced_day(day);
}

static void test_weather(GameState* game) {
	WeatherDayData day;

	assert(weather_forced(game, "/weather rain", &day));
	assert(day.condition == WEATHER_RAIN && day.intensity == 2 && day.wind == 1);
	assert(weather_forced(game, "/weather rain heavy gale", &day));
	assert(day.condition == WEATHER_RAIN && day.intensity == 3 && day.wind == 3);
	// The order of the words does not matter, nor their case
	assert(weather_forced(game, "/weather GALE Light snow", &day));
	assert(day.condition == WEATHER_SNOW && day.intensity == 1 && day.wind == 3);
	// A thunderstorm is at least moderate, and heavy unless said otherwise
	assert(weather_forced(game, "/weather thunder", &day));
	assert(day.condition == WEATHER_THUNDER && day.intensity == 3 && day.wind == 2);
	assert(weather_forced(game, "/weather thunder light", &day));
	assert(day.intensity == 2);
	// A dry day has nothing to be light or heavy
	assert(weather_forced(game, "/weather clear heavy windy", &day));
	assert(day.condition == WEATHER_CLEAR && day.intensity == 0 && day.wind == 2);
	assert(weather_forced(game, "/weather cloudy calm", &day));
	assert(day.wind == 0);
	// Rolling in is not a different day
	assert(weather_forced(game, "/weather roll rain heavy", &day));
	assert(day.condition == WEATHER_RAIN && day.intensity == 3);
	// Off gives the forecast back
	weather_now_force(WEATHER_RAIN);
	{
		int handled = 0;

		type(game, "/weather off", &handled);
		assert(handled && strstr(newest(game)->text, "forecast's again") != NULL);
	}
	assert(weather_now_forced() == -1);
	// Mistakes are reported and change nothing
	weather_now_force(-1);
	expect_reported(game, "/weather", "Usage: /weather");
	expect_reported(game, "/weather sunny", "Usage: /weather");
	expect_reported(game, "/weather rain hevy", "Usage: /weather");
	assert(weather_now_forced() == -1);
	// Start the chat log over, so the tests after this one have the room they expect
	game->playing->chat.line_count = 0;
}

// Says a /weather storm line, lets the weather see it, and returns how long it will last
static double storm_length(GameState* game, const char* line) {
	double elapsed;
	double length = 0.0;
	int handled = 0;

	weather_now_force(-1);
	weather_state_reset();
	weather_now_reset();
	type(game, line, &handled);
	assert(handled);
	weather_state_tick(1000.0);
	weather_now_update(0.0f, 0, 0.0f, 0.0f);
	if(!weather_now_storm_progress(&elapsed, &length))
		return 0.0;

	return length;
}

static void test_weather_storm(GameState* game) {
	assert(storm_length(game, "/weather storm") == 8.0 * 60.0);
	assert(strstr(newest(game)->text, "rainy storm") != NULL);
	assert(weather_now_forced() == WEATHER_RAIN);
	assert(storm_length(game, "/weather storm 5 thunder") == 300.0);
	assert(strstr(newest(game)->text, "thundery storm") != NULL);
	assert(storm_length(game, "/weather STORM thunder 2") == 120.0);
	// Off ends it, and any other weather takes its place
	{
		int handled = 0;
		double elapsed, length;

		type(game, "/weather off", &handled);
		assert(handled);
		assert(!weather_now_storm_progress(&elapsed, &length));
		assert(storm_length(game, "/weather storm 3") == 180.0);
		weather_forced(game, "/weather rain", &(WeatherDayData){ 0 });
		assert(!weather_now_storm_progress(&elapsed, &length));
	}
	// A storm that is made already, and comes to you
	{
		int handled = 0;

		weather_now_force(-1);
		type(game, "/weather storm sharp thunder", &handled);
		assert(handled && strstr(newest(game)->text, "sharp thundery storm") != NULL);
		assert(weather_now_cell_running() && weather_now_forced() == WEATHER_CLEAR);
		type(game, "/weather storm BROAD", &handled);
		assert(strstr(newest(game)->text, "broad storm") != NULL && weather_now_cell_running());
		type(game, "/weather off", &handled);
		assert(!weather_now_cell_running());
	}
	// Mistakes are reported and start nothing
	weather_now_force(-1);
	expect_reported(game, "/weather storm broad 5", "Usage: /weather");
	expect_reported(game, "/weather storm broad sharp 5", "Usage: /weather");
	assert(!weather_now_cell_running());
	expect_reported(game, "/weather storm 0", "Usage: /weather");
	expect_reported(game, "/weather storm 99", "Usage: /weather");
	expect_reported(game, "/weather storm soon", "Usage: /weather");
	assert(weather_now_forced() == -1);
	game->playing->chat.line_count = 0;
}

static void test_help(GameState* game) {
	ChatState* chat = &game->playing->chat;
	int before = chat->line_count;
	int handled;

	assert(type(game, "/help", &handled) == 0 && handled == 1);

	// one line per command, then the emotes
	assert(chat->line_count == before + chat_command_count() + 1);
	for(int i = before; i < chat->line_count; i++)
		assert(chat->lines[i].style == CHAT_STYLE_SYSTEM);
	assert(strstr(chat->lines[before].text, "/invite name") != NULL);
	assert(strstr(chat->lines[chat->line_count - 1].text, "Emotes:") != NULL);
	assert(strstr(chat->lines[chat->line_count - 1].text, "/sit") != NULL);

	expect_reported(game, "/help me", "Usage: /help");
}

static void test_table_is_sound(void) {
	int n = chat_command_count();

	assert(n >= 11);
	for(int i = 0; i < n; i++) {
		const chat_command* c = chat_command_get(i);

		assert(c && c->name && c->name[0] && c->run && c->help && c->help[0]);
		assert(strchr(c->name, ' ') == NULL && c->name[0] != '/');

		// the usage line starts with the command itself
		assert(c->usage && c->usage[0] == '/');
		assert(strncmp(c->usage + 1, c->name, strlen(c->name)) == 0);

		// names are unique
		assert(chat_command_find(c->name, strlen(c->name)) == c);
	}
	assert(chat_command_get(-1) == NULL && chat_command_get(n) == NULL);
	assert(chat_command_find("stand", 5) != NULL);
	assert(chat_command_find("stan", 4) == NULL);
	assert(chat_command_find("STAND", 5) != NULL);

	// emote commands are not in this table: they come from the emote table
	assert(chat_command_find("sit", 3) == NULL);
}

int main(void) {
	GameState* game = new_game();

	test_weather(game);
	test_weather_storm(game);
	test_existing_commands(game);
	test_reply(game);
	test_dropped_lines(game);
	test_ordinary_chat(game);
	test_emotes(game);
	test_ping(game);
	test_time(game);
	test_help(game);
	test_table_is_sound();
	free(game->playing);
	free(game);
	printf("chat_commands_test: OK\n");

	return 0;
}
