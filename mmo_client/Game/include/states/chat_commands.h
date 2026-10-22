#ifndef CHAT_COMMANDS_H
#define CHAT_COMMANDS_H

// The slash commands typed into chat, as a table of entries and handlers.

// Emote commands (/sit, /dance) come from the emote table instead.

#include "core/game_types.h"

typedef enum {
	CHAT_ARGS_NONE, // the word alone: "/leave"; anything after it is a usage error
	CHAT_ARGS_REQUIRED // the word, a space, then text: "/invite name"
} chat_args;

typedef struct {
	const char* name; // without the slash; lowercase and unique in the table
	chat_args args;
	void (*run)(GameState* game, const char* args); // args: text after the space
	const char* help; // one line saying what it does
	const char* usage; // how to type it: "/invite name"
} chat_command;

// Runs the line if it is a slash command; a mistake is reported, not sent.

// Returns 1 when the line starts with '/', 0 for ordinary chat.
int chat_command_dispatch(GameState* game, const char* line);

// The entry for a command word of the given length, or NULL; case is ignored.
const chat_command* chat_command_find(const char* word, size_t length);

int chat_command_count(void);
const chat_command* chat_command_get(int index);

#endif /* CHAT_COMMANDS_H */
