#ifndef STATUS_HANDLER_H
#define STATUS_HANDLER_H

// Keeps each player's chosen status and idle clock; rules are in status_rules.h.

#include "server_types.h"

// A player just did something. Ends AFK, and says so if their status changed.
void status_note_activity(int client_fd, uint32_t character_id, int player_slot);

// A player chose a status (SESSION_STATUS_*): set it, and tell them the result.
void status_handle_request(int client_fd, uint32_t character_id, int player_slot,
		uint8_t status);

#endif /* STATUS_HANDLER_H */
