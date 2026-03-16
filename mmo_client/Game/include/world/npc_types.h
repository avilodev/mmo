#ifndef NPC_TYPES_H
#define NPC_TYPES_H

#include <stdint.h>

// Load npc_types.json and build the type_id -> name table.
// Safe to call with a missing file (graceful degradation).
void        npc_types_init(const char* path);
void        npc_types_cleanup(void);

// Returns the display name for a given type_id, or NULL if unknown.
const char* npc_type_get_name(uint8_t type_id);

#endif // NPC_TYPES_H
