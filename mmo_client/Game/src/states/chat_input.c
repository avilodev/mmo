// Editing the line being typed into chat, and turning it into what is sent.

#include "states/chat_input.h"
#include "states/chat_channels.h"
#include "states/chat_log.h"
#include "protocol.h"

#include <stdio.h>
#include <string.h>

// Seconds a held backspace waits before repeating, and between repeats.
#define BACKSPACE_DELAY 0.4f
#define BACKSPACE_REPEAT 0.05f

void chat_open(ChatState* chat) {
	chat->is_typing = 1;
	chat->history_pos = -1;
}

void chat_close(ChatState* chat) {
	chat->is_typing = 0;
	chat->input_len = 0;
	chat->input_buf[0] = '\0';
	chat->history_pos = -1;
	chat->scroll = 0;
}

// Replaces the typed line.
static void set_input(ChatState* chat, const char* text) {
	snprintf(chat->input_buf, sizeof(chat->input_buf), "%s", text);
	chat->input_len = (int)strlen(chat->input_buf);
}

void chat_begin_whisper(ChatState* chat, const char* name) {
	char line[MAX_CHAT_INPUT_LEN];

	snprintf(line, sizeof(line), "/w %s ", name);
	set_input(chat, line);
	chat_open(chat);
}

void chat_type(ChatState* chat, unsigned int codepoint) {
	if(codepoint < 32 || codepoint >= 127)
		return;
	if(chat->input_len >= MAX_CHAT_INPUT_LEN - 1)
		return;

	chat->input_buf[chat->input_len++] = (char)codepoint;
	chat->input_buf[chat->input_len] = '\0';
	chat->history_pos = -1;
}

int chat_backspace(ChatState* chat) {
	if(chat->input_len <= 0)
		return 0;

	chat->input_buf[--chat->input_len] = '\0';
	chat->history_pos = -1;

	return 1;
}

void chat_backspace_update(ChatState* chat, int just_pressed, int held,
		float delta_time) {
	float threshold;

	if(!held)
		return;

	if(just_pressed) {
		chat_backspace(chat);
		chat->backspace_timer = 0.0f;
		chat->backspace_first = 1;
		return;
	}

	chat->backspace_timer += delta_time;
	threshold = chat->backspace_first ? BACKSPACE_DELAY : BACKSPACE_REPEAT;
	if(chat->backspace_timer >= threshold) {
		chat->backspace_timer = 0.0f;
		chat->backspace_first = 0;
		chat_backspace(chat);
	}
}

void chat_cycle_channel(ChatState* chat) {
	chat->active_channel = chat_channel_next(chat->active_channel,
			chat->whisper_reply_target[0] != '\0');
}

void chat_history_add(ChatState* chat, const char* line) {
	if(line[0] == '\0')
		return;
	if(chat->history_count > 0 &&
	   strcmp(chat->history[chat->history_count - 1], line) == 0)
		return;

	if(chat->history_count >= CHAT_HISTORY_LINES) {
		memmove(chat->history[0], chat->history[1],
				sizeof(chat->history[0]) * (CHAT_HISTORY_LINES - 1));
		chat->history_count = CHAT_HISTORY_LINES - 1;
	}

	snprintf(chat->history[chat->history_count++], sizeof(chat->history[0]),
			"%s", line);
}

void chat_history_step(ChatState* chat, int direction) {
	int pos = chat->history_pos + direction;

	if(pos >= chat->history_count || pos < -1)
		return;

	// Leaving fresh typing: keep what was there for the way back
	if(chat->history_pos == -1)
		snprintf(chat->history_draft, sizeof(chat->history_draft), "%s",
				chat->input_buf);

	chat->history_pos = pos;
	if(pos == -1)
		set_input(chat, chat->history_draft);
	else
		set_input(chat, chat->history[chat->history_count - 1 - pos]);
}

// Copies the typed line into out->text, minus spaces at both ends.
static int trimmed_text(const ChatState* chat, chat_outgoing* out) {
	const char* start = chat->input_buf;
	size_t length;

	while(*start == ' ')
		start++;
	length = strlen(start);
	while(length > 0 && start[length - 1] == ' ')
		length--;

	snprintf(out->text, sizeof(out->text), "%.*s", (int)length, start);

	return length > 0;
}

int chat_input_submit(ChatState* chat, chat_outgoing* out) {
	memset(out, 0, sizeof(*out));
	if(!trimmed_text(chat, out))
		return 0;

	out->channel = chat->active_channel;
	if(out->channel >= CHAT_SPEAKABLE_CHANNELS)
		out->channel = CHAT_CHANNEL_LOCAL;
	if(out->channel != CHAT_CHANNEL_WHISPER)
		return 1;

	if(chat->whisper_reply_target[0] == '\0') {
		chat_log_add(chat, CHAT_CHANNEL_SYSTEM, CHAT_STYLE_ERROR, "",
				"Nobody to whisper. Use /w name message.");
		return 0;
	}

	snprintf(out->target, sizeof(out->target), "%s", chat->whisper_reply_target);

	return 1;
}
