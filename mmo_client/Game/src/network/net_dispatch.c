// Offer each packet to the domain dispatchers in turn.

#include "net_internal.h"

// By traffic: the per-tick streams are asked first, rarer opcodes after
int net_dispatch_packet(uint8_t type, const char* data, int length) {
	if(net_dispatch_world(type, data, length))
		return 1;
	if(net_dispatch_combat(type, data, length))
		return 1;
	if(net_dispatch_session(type, data, length))
		return 1;
	if(net_dispatch_inventory(type, data, length))
		return 1;
	return net_dispatch_friends(type, data, length);
}
