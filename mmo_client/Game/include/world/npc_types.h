#ifndef NPC_TYPES_H
#define NPC_TYPES_H

#include <stdint.h>

// Load npc_types.json and build the type_id -> name table.
// Safe to call with a missing file (graceful degradation).
void        npc_types_init(const char* path);
void        npc_types_cleanup(void);

// Returns the display name for a given type_id, or NULL if unknown.
const char* npc_type_get_name(uint8_t type_id);

/** Look up how one NPC type is drawn.
 *
 * The table is generated from the server's own content, so the client never
 * decides what an enemy looks like: faction picks the hue, role picks the value
 * and the box size, and both arrive here already resolved. A type with no style
 * row falls back to the category colouring the renderer has always used, which
 * is what keeps an unpatched client usable against newer content.
 *
 * @param type_id  The npc_type_id from the wire.
 * @param out_rgb  Receives three floats in 0..1; untouched when 0 is returned.
 * @param out_size Receives the box multiplier; untouched when 0 is returned.
 * @return         1 when the type has a generated style, 0 otherwise.
 */
int npc_type_get_style(uint8_t type_id, float* out_rgb, float* out_size);

/** Report whether a type is drawn with an outline — elites and mini-bosses.
 *
 * @param out_rgb  Receives the outline colour; untouched when 0 is returned.
 * @return         1 when the type has an outline, 0 otherwise.
 */
int npc_type_get_outline(uint8_t type_id, float* out_rgb);

#endif // NPC_TYPES_H
