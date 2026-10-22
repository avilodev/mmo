// Decide when a nearby-entities list needs sending.

#include "list_gate.h"

// What was last sent on a descriptor
#define GATE_FRESH 0 // nothing yet: the first list goes whatever it holds
#define GATE_HAD_ENTRIES 1
#define GATE_EMPTY_SENT 2

list_gate g_player_list_gate;
list_gate g_npc_list_gate;

int list_gate_due(list_gate* gate, int fd, int count) {
	if(fd < 0 || fd >= LIST_GATE_FDS)
		return count > 0;

	if(count > 0) {
		gate->state[fd] = GATE_HAD_ENTRIES;

		return 1;
	}
	if(gate->state[fd] != GATE_EMPTY_SENT) {
		gate->state[fd] = GATE_EMPTY_SENT;

		return 1;
	}

	return 0;
}

void list_gate_reset(list_gate* gate, int fd) {
	if(fd < 0 || fd >= LIST_GATE_FDS)
		return;
	gate->state[fd] = GATE_FRESH;
}

void list_gates_reset_fd(int fd) {
	list_gate_reset(&g_player_list_gate, fd);
	list_gate_reset(&g_npc_list_gate, fd);
}
