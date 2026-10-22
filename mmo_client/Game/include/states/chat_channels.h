#ifndef CHAT_CHANNELS_H
#define CHAT_CHANNELS_H

// How each chat channel looks, and how a line of chat reads on screen.

#include "core/game_types.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
	uint8_t id; // a ChatChannel
	const char* tag; // "[W]" in front of a line
	const char* name; // "Whisper" on the input badge
	float r, g, b; // the colour its lines are drawn in
} chat_channel_info;

// The channel's entry; an id nobody knows gets Local's.
const chat_channel_info* chat_channel_lookup(uint8_t id);

// The channel Tab picks next; Whisper is skipped with nobody to whisper.
uint8_t chat_channel_next(uint8_t current, int can_whisper);

// The line as drawn, "[G] Ann: hi"; a client-made line is its text alone.
void chat_line_format(const ChatLine* line, char* out, size_t size);

// The colour a line is drawn in.
void chat_line_color(const ChatLine* line, float* r, float* g, float* b);

// The input badge: "[Local] ", or "[-> Ann] " for a whisper with a target.
void chat_badge_format(uint8_t channel, const char* whisper_target,
		char* out, size_t size);

// True for a line whose sender can be whispered by clicking it.
int chat_line_replyable(const ChatLine* line);

#endif /* CHAT_CHANNELS_H */
