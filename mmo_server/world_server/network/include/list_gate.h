#ifndef LIST_GATE_H
#define LIST_GATE_H

/** @file Decide when a "who is near you" list is worth sending.
 *
 * A client keeps the last list it was given until the next one arrives. The
 * broadcasts used to send nothing when the list was empty, so the last player
 * to leave (a disconnect, a walk out of range, a stealthed NPC) stayed on the
 * client's screen for good. An empty list has to be sent once to clear it, but
 * not twenty times a second for as long as the player stands alone.
 *
 * A connection's first list goes whatever it holds. A client entering the world
 * waits for it, and with nobody around, silence could not be told from a list
 * still on its way.
 *
 * One byte of memory per descriptor. Each shard touches only its own
 * descriptors, so no lock is needed.
 */

#include <stdint.h>

/** Descriptors tracked; the connection table is capped at the same size. */
#define LIST_GATE_FDS 65536

typedef struct {
	uint8_t state[LIST_GATE_FDS];
} list_gate;

/** Whether a list of `count` entries should be sent to `fd` now.
 *
 * A list with entries always goes. An empty one goes once after a list that had
 * entries, and as the first list a connection is sent. A descriptor outside the
 * table is never gated.
 */
int list_gate_due(list_gate* gate, int fd, int count);

/** Make the next list for `fd` its first: call when a connection opens. */
void list_gate_reset(list_gate* gate, int fd);

/** The two gates the broadcasts use. */
extern list_gate g_player_list_gate;
extern list_gate g_npc_list_gate;

/** Reset both gates for a descriptor that has just been given to a player. */
void list_gates_reset_fd(int fd);

#endif // LIST_GATE_H
