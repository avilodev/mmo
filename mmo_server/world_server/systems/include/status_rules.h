#ifndef STATUS_RULES_H
#define STATUS_RULES_H

// Which status the server shows for a player: the one they chose, or idle.

#include "protocol.h"

#include <stdint.h>

// Seconds without any action before an Online player shows as AFK.
#define AFK_IDLE_SECONDS 300.0

// Returns the SESSION_STATUS_* to show. Only Online goes AFK on its own.
uint8_t status_effective(uint8_t choice, double last_activity_at, double now);

#endif /* STATUS_RULES_H */
