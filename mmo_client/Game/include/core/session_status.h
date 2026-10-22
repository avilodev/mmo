#ifndef SESSION_STATUS_H
#define SESSION_STATUS_H

// The names of the statuses a player can show: Online, AFK, Do Not Disturb, Offline.

#include "protocol.h"

#include <stddef.h>
#include <stdint.h>

// The name shown in the player list and in chat; "Unknown" outside the list.
const char* session_status_label(uint8_t status);

// Reads a typed word (online, afk, dnd, offline; any case). Returns 1 and sets
// *status, or 0 when the word is not one of them.
int session_status_parse(const char* word, size_t length, uint8_t* status);

#endif /* SESSION_STATUS_H */
