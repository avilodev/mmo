// How each chat channel looks, and how a line of chat reads on screen.

#include "states/chat_channels.h"

#include <stdio.h>

// Indexed by channel id.
static const chat_channel_info CHANNELS[] = {
	{ CHAT_CHANNEL_LOCAL, "[L]", "Local", 0.92f, 0.92f, 0.92f },
	{ CHAT_CHANNEL_GLOBAL, "[G]", "Global", 1.00f, 0.72f, 0.35f },
	{ CHAT_CHANNEL_WHISPER, "[W]", "Whisper", 0.95f, 0.50f, 0.95f },
	{ CHAT_CHANNEL_PARTY, "[P]", "Party", 0.45f, 0.70f, 1.00f },
	{ CHAT_CHANNEL_SYSTEM, "[S]", "System", 0.90f, 0.90f, 0.55f },
};

#define CHANNEL_COUNT ((int)(sizeof(CHANNELS) / sizeof(CHANNELS[0])))

const chat_channel_info* chat_channel_lookup(uint8_t id) {
	if(id >= CHANNEL_COUNT)
		return &CHANNELS[CHAT_CHANNEL_LOCAL];

	return &CHANNELS[id];
}

uint8_t chat_channel_next(uint8_t current, int can_whisper) {
	uint8_t next = (uint8_t)((current + 1) % CHAT_SPEAKABLE_CHANNELS);

	if(next == CHAT_CHANNEL_WHISPER && !can_whisper)
		next = (uint8_t)((next + 1) % CHAT_SPEAKABLE_CHANNELS);

	return next;
}

void chat_line_format(const ChatLine* line, char* out, size_t size) {
	const chat_channel_info* info = chat_channel_lookup(line->channel);

	if(line->style != CHAT_STYLE_NORMAL)
		snprintf(out, size, "%s", line->text);
	else if(line->flags & CHAT_FLAG_ECHO)
		snprintf(out, size, "%s To %s: %s", info->tag, line->sender, line->text);
	else
		snprintf(out, size, "%s %s: %s", info->tag, line->sender, line->text);
}

void chat_line_color(const ChatLine* line, float* r, float* g, float* b) {
	const chat_channel_info* info = chat_channel_lookup(line->channel);

	*r = info->r;
	*g = info->g;
	*b = info->b;

	if(line->style == CHAT_STYLE_ERROR) {
		*r = 1.0f;
		*g = 0.25f;
		*b = 0.25f;
	} else if(line->style == CHAT_STYLE_EMOTE) {
		*r = 1.0f;
		*g = 0.85f;
		*b = 0.45f;
	} else if(line->style == CHAT_STYLE_SYSTEM) {
		info = chat_channel_lookup(CHAT_CHANNEL_SYSTEM);
		*r = info->r;
		*g = info->g;
		*b = info->b;
	}
}

void chat_badge_format(uint8_t channel, const char* whisper_target,
		char* out, size_t size) {
	if(channel == CHAT_CHANNEL_WHISPER && whisper_target[0] != '\0')
		snprintf(out, size, "[-> %s] ", whisper_target);
	else
		snprintf(out, size, "[%s] ", chat_channel_lookup(channel)->name);
}

int chat_line_replyable(const ChatLine* line) {
	return line->style == CHAT_STYLE_NORMAL && line->sender[0] != '\0' &&
		!(line->flags & CHAT_FLAG_ECHO);
}
