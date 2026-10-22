#ifndef INSPECT_HANDLER_H
#define INSPECT_HANDLER_H

// Answers inspect requests: another nearby player's gear and stats.

#include "server_types.h"

// How close, in world units, the target must be. A little under the interest
// radius, so anyone the inspector can see is anyone they may inspect.
#define INSPECT_RANGE 600.0f

// Answer one InspectRequestPacket from a connected player.
void handle_inspect_request(int client_fd, uint32_t character_id, int player_slot,
		const InspectRequestPacket* pkt);

#endif /* INSPECT_HANDLER_H */
