#ifndef LIMIT_PROFILES_H
#define LIMIT_PROFILES_H

#include "packet_limiter.h"

/** @file Expose per-server packet-limiter budgets and opcode costs.
 * Unlisted opcodes use the profile's default cost.
 */

const PacketLimitProfile* limit_profile_world(void);
const PacketLimitProfile* limit_profile_realm(void);

#endif // LIMIT_PROFILES_H
