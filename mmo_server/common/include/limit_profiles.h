#ifndef LIMIT_PROFILES_H
#define LIMIT_PROFILES_H

#include "packet_limiter.h"

// ============================================================================
// limit_profiles.h — The budget each server hands to the packet limiter.
//
// One implementation, three profiles. World traffic is dominated by cheap
// high-frequency movement; realm traffic is a trickle of expensive database
// work. Sharing budgets between them would mean sizing for the wrong thing
// twice.
//
// When you add an opcode to protocol.h, add a row to the relevant profile in
// limit_profiles.c. Forgetting is not fatal — an unlisted opcode is charged the
// fail-safe default — but the default is sized for cheap queries, so anything
// expensive should be given an explicit cost.
// ============================================================================

const PacketLimitProfile* limit_profile_world(void);
const PacketLimitProfile* limit_profile_realm(void);

#endif // LIMIT_PROFILES_H
