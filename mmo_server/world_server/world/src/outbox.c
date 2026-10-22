// Where game logic puts packets for players.

#include "outbox.h"

#include <stdatomic.h>

static _Atomic outbox_sender g_sender;

void outbox_set_sender(outbox_sender sender) {
	atomic_store(&g_sender, sender);
}

ssize_t outbox_send(int client_fd, void* packet, size_t length) {
	outbox_sender sender = atomic_load(&g_sender);

	if(!sender)
		return -1;

	return sender(client_fd, packet, length);
}
