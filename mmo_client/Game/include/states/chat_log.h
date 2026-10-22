#ifndef CHAT_LOG_H
#define CHAT_LOG_H

// The chat lines on screen: the client's own, and the ones the server sends.

#include "core/game_types.h"

// Appends a line, dropping the oldest when full; every field is always set.
void chat_log_add(ChatState* chat, uint8_t channel, chat_style style,
		const char* sender, const char* text);

// Appends a line a ChatMessagePacket carried, and follows whisper partners.
void chat_log_receive(ChatState* chat, uint8_t channel, uint8_t flags,
		const char* sender, const char* text);

#endif /* CHAT_LOG_H */
