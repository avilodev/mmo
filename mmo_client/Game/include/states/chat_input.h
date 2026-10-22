#ifndef CHAT_INPUT_H
#define CHAT_INPUT_H

// Editing the line being typed into chat, and turning it into what is sent.

#include "core/game_types.h"

// What one typed line becomes on the wire.
typedef struct {
	uint8_t channel;
	char target[32]; // the whisper's recipient; empty on other channels
	char text[MAX_CHAT_INPUT_LEN];
} chat_outgoing;

void chat_open(ChatState* chat);

// Stops typing and clears the line, and returns the log to the newest.
void chat_close(ChatState* chat);

// Opens chat with "/w name " already typed.
void chat_begin_whisper(ChatState* chat, const char* name);

// Adds a typed character; anything but printable ASCII is ignored.
void chat_type(ChatState* chat, unsigned int codepoint);

// Deletes the last character; returns 0 when there was none.
int chat_backspace(ChatState* chat);

// Deletes on the first press, then repeats while the key is held.
void chat_backspace_update(ChatState* chat, int just_pressed, int held,
		float delta_time);

// Tab: the next channel, skipping Whisper while nobody is there to whisper.
void chat_cycle_channel(ChatState* chat);

// Keeps a sent line for the up arrow; blanks and repeats are not kept.
void chat_history_add(ChatState* chat, const char* line);

// Up (+1) recalls an older line, down (-1) a newer one, then what was typed.
void chat_history_step(ChatState* chat, int direction);

// The typed line as a message for the active channel.
// Returns 0, saying why in the log when it can, if there is nothing to send.
int chat_input_submit(ChatState* chat, chat_outgoing* out);

#endif /* CHAT_INPUT_H */
