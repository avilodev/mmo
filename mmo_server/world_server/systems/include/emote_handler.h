#ifndef EMOTE_HANDLER_H
#define EMOTE_HANDLER_H

// Applies emote requests to the player table; rules are in emote_validator.h.

#include "server_types.h"

// Answer one EmoteRequestPacket from a connected player.
void handle_emote_request(int client_fd, uint32_t character_id, int player_slot,
		const EmoteRequestPacket* pkt);

// Tell a player what their emote is now and why it changed.
void emote_send_state(int client_fd, uint32_t character_id, uint8_t emote,
		uint8_t arg, uint8_t result, uint8_t requested);

// End and report emotes whose player died or fought. Run from the scheduler.
void emote_upkeep_tick(void);

#endif /* EMOTE_HANDLER_H */
