// The chat lines on screen: the client's own, and the ones the server sends.

#include "states/chat_log.h"
#include "protocol.h"

#include <stdio.h>
#include <string.h>

// The slot for a new line, making room by dropping the oldest when full.
static ChatLine* next_slot(ChatState* chat) {
	int idx = chat->line_count;

	if(idx >= MAX_CHAT_LINES) {
		memmove(&chat->lines[0], &chat->lines[1],
		        sizeof(ChatLine) * (MAX_CHAT_LINES - 1));
		idx = MAX_CHAT_LINES - 1;
	} else {
		chat->line_count++;
	}

	// A reader scrolled back keeps the same lines in view
	if(chat->scroll > 0 && chat->scroll < chat->line_count - 1)
		chat->scroll++;

	return &chat->lines[idx];
}

void chat_log_add(ChatState* chat, uint8_t channel, chat_style style,
		const char* sender, const char* text) {
	ChatLine* line = next_slot(chat);

	snprintf(line->sender, sizeof(line->sender), "%s", sender ? sender : "");
	snprintf(line->text, sizeof(line->text), "%s", text ? text : "");
	line->channel = channel;
	line->style = (uint8_t)style;
	line->flags = 0;
}

void chat_log_receive(ChatState* chat, uint8_t channel, uint8_t flags,
		const char* sender, const char* text) {
	chat_style style = channel == CHAT_CHANNEL_SYSTEM ?
		CHAT_STYLE_SYSTEM : CHAT_STYLE_NORMAL;

	chat_log_add(chat, channel, style, sender, text);
	chat->lines[chat->line_count - 1].flags = flags;

	// Whoever whispered us, or whom we whispered, is who /r and Tab answer
	if(channel == CHAT_CHANNEL_WHISPER && sender && sender[0] != '\0')
		snprintf(chat->whisper_reply_target, sizeof(chat->whisper_reply_target),
				"%s", sender);
}
