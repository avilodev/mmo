#ifndef MOVEMENT_SYSTEM_H
#define MOVEMENT_SYSTEM_H

// Movement rules. Network code parses a request and calls this.

#include "protocol.h"

#include <stdint.h>

// accept or correct a position the client proposed
void movement_system_propose(int client_fd, uint32_t character_id, int player_slot,
                             const PlayerMovePacket* pkt);

#endif /* MOVEMENT_SYSTEM_H */
