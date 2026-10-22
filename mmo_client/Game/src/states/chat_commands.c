// The slash commands typed into chat.

// To add a command: write its handler, then add one line to COMMANDS.

#include "states/chat_commands.h"
#include "states/chat_log.h"
#include "network/network.h"
#include "states/emote_client.h"
#include "core/session_status.h"
#include "protocol.h"
#include "world/world_clock.h"
#include "world/weather_now.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The longest command word; anything longer cannot be a command.
#define COMMAND_WORD_MAX 16

// Says something to the player, in the chat box only.
static void say(GameState* game, chat_style style, const char* text) {
	chat_log_add(&game->playing->chat, CHAT_CHANNEL_SYSTEM, style, "", text);
}

static void fail(GameState* game, const char* text) {
	say(game, CHAT_STYLE_ERROR, text);
}

static void cmd_invite(GameState* game, const char* args) {
	(void)game;
	network_send_party_invite(args);
}

static void cmd_leave(GameState* game, const char* args) {
	(void)game;
	(void)args;
	network_send_party_leave();
}

// Flips between AFK and Online, going by what the server last said.
static void cmd_afk(GameState* game, const char* args) {
	(void)args;

	if(game->playing->own_status == SESSION_STATUS_AFK)
		network_send_status_request(SESSION_STATUS_ONLINE);
	else
		network_send_status_request(SESSION_STATUS_AFK);
}

// "/status dnd": choose how you show in the player list.
static void cmd_status(GameState* game, const char* args) {
	uint8_t status;
	size_t length;

	while(*args == ' ')
		args++;
	length = strcspn(args, " ");

	if(length == 0 || args[length] != '\0' || !session_status_parse(args, length, &status)) {
		fail(game, "Usage: /status online|afk|dnd|offline");
		return;
	}

	network_send_status_request(status);
}

static void cmd_friend(GameState* game, const char* args) {
	(void)game;
	network_send_friend_request(args);
}

static void cmd_unfriend(GameState* game, const char* args) {
	(void)game;
	network_send_friend_remove(args);
}

static void cmd_block(GameState* game, const char* args) {
	(void)game;
	network_send_friend_block(args, 1);
}

static void cmd_unblock(GameState* game, const char* args) {
	(void)game;
	network_send_friend_block(args, 0);
}

static void cmd_global(GameState* game, const char* args) {
	(void)game;
	network_send_chat(CHAT_CHANNEL_GLOBAL, "", args);
}

static void cmd_party(GameState* game, const char* args) {
	(void)game;
	network_send_chat(CHAT_CHANNEL_PARTY, "", args);
}

// "/w name message": the first word is who, the rest is what.
static void cmd_whisper(GameState* game, const char* args) {
	char target[MAX_CHAT_TARGET];
	size_t length;
	const char* text;

	while(*args == ' ')
		args++;
	length = strcspn(args, " ");
	text = args + length;
	while(*text == ' ')
		text++;

	if(length == 0 || length >= sizeof(target) || text[0] == '\0') {
		fail(game, "Usage: /w name message");
		return;
	}

	snprintf(target, sizeof(target), "%.*s", (int)length, args);
	network_send_chat(CHAT_CHANNEL_WHISPER, target, text);
}

// Replies to whoever the last whisper was with.
static void cmd_reply(GameState* game, const char* args) {
	const char* target = game->playing->chat.whisper_reply_target;

	if(target[0] == '\0') {
		fail(game, "Nobody has whispered you yet.");
		return;
	}

	network_send_chat(CHAT_CHANNEL_WHISPER, target, args);
}

// Emotes (/sit, /dance) are in the emote table, not here.
static void cmd_stand(GameState* game, const char* args) {
	(void)args;
	emote_client_stop(game);
}

// Measures the ping now; the figure is written to chat when it comes in.
static void cmd_ping(GameState* game, const char* args) {
	(void)args;

	if(!network_is_connected()) {
		fail(game, "Not connected to a server.");
		return;
	}

	network_request_ping_report();
}

// Says the time of day in the world, in the clock format the player chose.
static void cmd_time(GameState* game, const char* args) {
	char clock[16];
	char line[48];

	(void)args;

	if(!world_clock_synced()) {
		fail(game, "The time is not known yet.");
		return;
	}

	world_clock_text_now(game->settings.clock_24h, clock, sizeof(clock));
	snprintf(line, sizeof(line), "The time is %s.", clock);
	say(game, CHAT_STYLE_SYSTEM, line);
}

// The words /weather takes. A condition, how hard it falls, how hard it blows.
typedef struct {
	const char* word;
	int value;
} weather_word;

static const weather_word WEATHER_CONDITIONS[] = {
	{ "clear", WEATHER_CLEAR }, { "partly", WEATHER_PARTLY_CLOUDY }, { "cloudy", WEATHER_CLOUDY },
	{ "rain", WEATHER_RAIN }, { "thunder", WEATHER_THUNDER }, { "snow", WEATHER_SNOW }
};

static const weather_word WEATHER_FALLS[] = {
	{ "light", 1 }, { "moderate", 2 }, { "heavy", 3 }
};

static const weather_word WEATHER_WINDS[] = {
	{ "calm", 0 }, { "breezy", 1 }, { "windy", 2 }, { "gale", 3 }
};

#define WEATHER_USAGE "Usage: /weather rain|snow|thunder|cloudy|partly|clear [light|heavy] " \
                      "[calm|breezy|windy|gale] [roll], /weather storm [minutes] [thunder], " \
                      "/weather storm broad|sharp [thunder], or /weather off"

// A storm lasts this many minutes unless the line says, and at most this many
#define STORM_MINUTES 8
#define STORM_MINUTES_MAX 60

// The value of a word in a table, or -1 when it is not there
static int weather_lookup(const weather_word* table, int count, const char* word) {
	for(int i = 0; i < count; i++)
		if(strcmp(word, table[i].word) == 0)
			return table[i].value;

	return -1;
}

// Reads the words of a /weather line into a day. Returns 0 for a word it does
// not know. off is set for "off", roll for "roll".
static int parse_weather(const char* args, WeatherDayData* day, int* off, int* roll) {
	char word[16];
	int has_condition = 0;
	int has_fall = 0;
	int has_wind = 0;

	memset(day, 0, sizeof(*day));
	*off = 0;
	*roll = 0;
	while(*args) {
		size_t length;
		int value;

		while(*args == ' ')
			args++;
		length = strcspn(args, " ");
		if(length == 0)
			break;
		if(length >= sizeof(word))
			return 0;
		for(size_t i = 0; i < length; i++)
			word[i] = (char)tolower((unsigned char)args[i]);
		word[length] = '\0';
		args += length;

		if(strcmp(word, "off") == 0) {
			*off = 1;
		} else if(strcmp(word, "roll") == 0) {
			*roll = 1;
		} else if((value = weather_lookup(WEATHER_CONDITIONS, 6, word)) >= 0) {
			day->condition = (uint8_t)value;
			has_condition = 1;
		} else if((value = weather_lookup(WEATHER_FALLS, 3, word)) >= 0) {
			day->intensity = (uint8_t)value;
			has_fall = 1;
		} else if((value = weather_lookup(WEATHER_WINDS, 4, word)) >= 0) {
			day->wind = (uint8_t)value;
			has_wind = 1;
		} else {
			return 0;
		}
	}
	if(*off)
		return 1;
	// A wet day with no say in how hard is a moderate one; one in the wind, breezy
	if(!has_fall && (day->condition == WEATHER_RAIN || day->condition == WEATHER_SNOW))
		day->intensity = 2;
	if(!has_fall && day->condition == WEATHER_THUNDER)
		day->intensity = 3;
	if(has_fall && day->condition == WEATHER_THUNDER && day->intensity < 2)
		day->intensity = 2;
	if(has_fall && day->condition != WEATHER_RAIN && day->condition != WEATHER_SNOW &&
	   day->condition != WEATHER_THUNDER)
		day->intensity = 0;
	if(!has_wind)
		day->wind = (uint8_t)(day->condition == WEATHER_THUNDER ? 2 : 1);
	(void)has_condition;

	return 1;
}

// Reads "storm [minutes] [thunder]" or "storm broad|sharp [thunder]". Returns 0
// when the line is not one that starts with "storm", 1 when it is (minutes and
// thunder are set, and cell is -1, or 0 for broad and 1 for sharp), -1 when it
// starts with "storm" but says something else.
static int parse_storm(const char* args, int* minutes, int* thunder, int* cell) {
	char word[16];
	int first = 1;
	int has_minutes = 0;

	*minutes = STORM_MINUTES;
	*thunder = 0;
	*cell = -1;
	while(*args) {
		size_t length;

		while(*args == ' ')
			args++;
		length = strcspn(args, " ");
		if(length == 0)
			break;
		if(length >= sizeof(word))
			return first ? 0 : -1;
		for(size_t i = 0; i < length; i++)
			word[i] = (char)tolower((unsigned char)args[i]);
		word[length] = '\0';
		args += length;

		if(first) {
			if(strcmp(word, "storm") != 0)
				return 0;
			first = 0;
		} else if(strcmp(word, "thunder") == 0) {
			*thunder = 1;
		} else if(strcmp(word, "broad") == 0 || strcmp(word, "sharp") == 0) {
			*cell = word[0] == 's' ? 1 : 0;
		} else if(word[0] >= '0' && word[0] <= '9') {
			has_minutes = 1;
			*minutes = atoi(word);
			if(*minutes < 1 || *minutes > STORM_MINUTES_MAX)
				return -1;
		} else {
			return -1;
		}
	}

	if(*cell >= 0 && has_minutes)
		return -1;

	return first ? 0 : 1;
}

// Makes the weather you see a chosen one, on this computer only. "off" ends it.
static void cmd_weather(GameState* game, const char* args) {
	WeatherDayData day;
	char line[96];
	int minutes;
	int thunder;
	int cell;
	int storm = parse_storm(args, &minutes, &thunder, &cell);
	int off;
	int roll;

	if(storm < 0) {
		fail(game, WEATHER_USAGE);
		return;
	}
	if(storm > 0 && cell >= 0) {
		weather_now_start_cell(cell, thunder);
		snprintf(line, sizeof(line), "A %s%s storm is coming your way, here only. /weather off ends it.",
		         cell ? "sharp" : "broad", thunder ? " thundery" : "");
		say(game, CHAT_STYLE_SYSTEM, line);
		return;
	}
	if(storm > 0) {
		weather_now_start_storm((double)minutes * 60.0, thunder);
		snprintf(line, sizeof(line), "A %s storm rolls in over %d minutes, here only. /weather off ends it.",
		         thunder ? "thundery" : "rainy", minutes);
		say(game, CHAT_STYLE_SYSTEM, line);
		return;
	}
	if(!parse_weather(args, &day, &off, &roll)) {
		fail(game, WEATHER_USAGE);
		return;
	}
	if(off) {
		weather_now_force_day(NULL, 0);
		say(game, CHAT_STYLE_SYSTEM, "The weather is the forecast's again.");
		return;
	}
	weather_now_force_day(&day, roll);
	say(game, CHAT_STYLE_SYSTEM, roll ? "That weather is rolling in, here only. /weather off ends it."
	                                  : "You see that weather, here only. /weather off ends it.");
}

static void cmd_help(GameState* game, const char* args);

static const chat_command COMMANDS[] = {
	{ "invite", CHAT_ARGS_REQUIRED, cmd_invite, "Invite a player to your party", "/invite name" },
	{ "leave", CHAT_ARGS_NONE, cmd_leave, "Leave your party", "/leave" },
	{ "afk", CHAT_ARGS_NONE, cmd_afk, "Mark yourself away, or back", "/afk" },
	{ "status", CHAT_ARGS_REQUIRED, cmd_status, "Choose how you show in the player list", "/status online|afk|dnd|offline" },
	{ "friend", CHAT_ARGS_REQUIRED, cmd_friend, "Send a friend request", "/friend name" },
	{ "unfriend", CHAT_ARGS_REQUIRED, cmd_unfriend, "Remove a friend", "/unfriend name" },
	{ "block", CHAT_ARGS_REQUIRED, cmd_block, "Block a player", "/block name" },
	{ "unblock", CHAT_ARGS_REQUIRED, cmd_unblock, "Unblock a player", "/unblock name" },
	{ "g", CHAT_ARGS_REQUIRED, cmd_global, "Say something in global chat", "/g message" },
	{ "p", CHAT_ARGS_REQUIRED, cmd_party, "Say something to your party", "/p message" },
	{ "w", CHAT_ARGS_REQUIRED, cmd_whisper, "Whisper to a player", "/w name message" },
	{ "r", CHAT_ARGS_REQUIRED, cmd_reply, "Reply to the last whisper", "/r message" },
	{ "stand", CHAT_ARGS_NONE, cmd_stand, "Stop emoting and get up", "/stand" },
	{ "ping", CHAT_ARGS_NONE, cmd_ping, "Show your ping in milliseconds", "/ping" },
	{ "time", CHAT_ARGS_NONE, cmd_time, "Show the time of day in the world", "/time" },
	{ "weather", CHAT_ARGS_REQUIRED, cmd_weather, "See a chosen weather, on your screen only", "/weather rain heavy gale" },
	{ "help", CHAT_ARGS_NONE, cmd_help, "List the commands", "/help" },
};

#define COMMAND_COUNT ((int)(sizeof(COMMANDS) / sizeof(COMMANDS[0])))

// Lists every command and emote in the chat box.
static void cmd_help(GameState* game, const char* args) {
	char line[160];
	size_t used;

	(void)args;

	for(int i = 0; i < COMMAND_COUNT; i++) {
		snprintf(line, sizeof(line), "%s - %s", COMMANDS[i].usage, COMMANDS[i].help);
		say(game, CHAT_STYLE_SYSTEM, line);
	}

	used = (size_t)snprintf(line, sizeof(line), "Emotes:");
	for(int id = EMOTE_NONE + 1; id < EMOTE_COUNT; id++) {
		const emote_def* emote = emote_lookup((uint8_t)id);

		if(emote && used < sizeof(line))
			used += (size_t)snprintf(line + used, sizeof(line) - used, " /%s",
					emote->command);
	}
	say(game, CHAT_STYLE_SYSTEM, line);
}

int chat_command_count(void) {
	return COMMAND_COUNT;
}

const chat_command* chat_command_get(int index) {
	if(index < 0 || index >= COMMAND_COUNT)
		return NULL;

	return &COMMANDS[index];
}

// Whether the typed word is the name, ignoring case.
static int same_word(const char* name, const char* word, size_t length) {
	if(strlen(name) != length)
		return 0;

	for(size_t i = 0; i < length; i++)
		if(tolower((unsigned char)word[i]) != name[i])
			return 0;

	return 1;
}

const chat_command* chat_command_find(const char* word, size_t length) {
	for(int i = 0; i < COMMAND_COUNT; i++)
		if(same_word(COMMANDS[i].name, word, length))
			return &COMMANDS[i];

	return NULL;
}

// Tells the player how the command is typed.
static void report_usage(GameState* game, const char* usage) {
	char text[96];

	snprintf(text, sizeof(text), "Usage: %s", usage);
	fail(game, text);
}

// Runs the command or emote once its word and arguments are checked.
static void run_command(GameState* game, const chat_command* command,
		const emote_def* emote, const char* args) {
	if(emote)
		emote_client_request(game, emote);
	else
		command->run(game, args);
}

int chat_command_dispatch(GameState* game, const char* line) {
	const char* word;
	size_t length;
	char lowered[COMMAND_WORD_MAX];
	const chat_command* command = NULL;
	const emote_def* emote = NULL;
	int has_space;
	const char* args;
	char usage[COMMAND_WORD_MAX + 2];

	if(!line || line[0] != '/')
		return 0;

	word = line + 1;
	length = strcspn(word, " ");
	if(length < sizeof(lowered)) {
		for(size_t i = 0; i < length; i++)
			lowered[i] = (char)tolower((unsigned char)word[i]);
		lowered[length] = '\0';
		command = chat_command_find(lowered, length);
		emote = command ? NULL : emote_find_command(lowered, length);
	}

	if(!command && !emote) {
		char text[96];

		snprintf(text, sizeof(text), "Unknown command: /%.*s. Type /help for a list.",
				(int)length, word);
		fail(game, text);
		return 1;
	}

	has_space = word[length] == ' ';
	args = has_space ? word + length + 1 : "";

	// an emote takes no arguments, like a command declared CHAT_ARGS_NONE
	if(emote || command->args == CHAT_ARGS_NONE) {
		if(has_space) {
			snprintf(usage, sizeof(usage), "/%s", lowered);
			report_usage(game, usage);
			return 1;
		}
	} else if(args[0] == '\0') {
		report_usage(game, command->usage);
		return 1;
	}

	run_command(game, command, emote, args);

	return 1;
}
