#ifndef ATTACK_DEFS_H
#define ATTACK_DEFS_H

#include "attack_types.h"

// Get attack definition by attack type (for when class is unknown)
const AttackDef* attack_def_get_by_type(AttackType type);

#endif // ATTACK_DEFS_H