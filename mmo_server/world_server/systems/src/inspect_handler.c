// Answer inspect requests with a copy of another nearby player's gear and stats.

#include "inspect_handler.h"
#include "outbox.h"
#include "log.h"
#include "player_data.h"
#include "str_fixed.h"

#include <arpa/inet.h>
#include <string.h>

// Returns 0 when the asking player is not in the world.
static int read_asker_position(uint32_t character_id, int player_slot, float* x,
		float* y) {
	ActivePlayer* p = player_acquire_hint(character_id, player_slot);

	if(!p)
		return 0;
	if(!p->is_loaded) {
		player_release(p);
		return 0;
	}

	*x = p->pos_x;
	*y = p->pos_y;
	player_release(p);

	return 1;
}

// Copy what an inspector may see. The caller holds the target's lock.
static void copy_inspectable(const ActivePlayer* p, InspectResponsePacket* out) {
	STR_COPY_FIELD(out->name, p->username);
	out->level = htonl((uint32_t)p->level);
	out->race_id = htonl(p->race_id);
	out->appearance = htonl(p->appearance);
	out->form = p->form;
	out->resource_type = p->resource_type;
	out->max_health = (int32_t)htonl((uint32_t)p->max_health);
	out->max_resource = (int32_t)htonl((uint32_t)p->max_resource);
	out->weapon_damage = (int32_t)htonl((uint32_t)p->weapon_damage);

	for(int i = 0; i < STAT_COUNT; i++)
		out->stats[i] = (int32_t)htonl((uint32_t)p->stats[i]);
	for(int i = 0; i < EQUIP_SLOTS; i++)
		out->equipment[i] = htonl(p->equipment[i].item_id);
}

// Look the target up and fill the reply, holding only the target's lock.
static InspectResult inspect_target(uint32_t asker_id, uint32_t target_id,
		float asker_x, float asker_y, InspectResponsePacket* out) {
	ActivePlayer* p;
	float dx;
	float dy;

	if(target_id == 0 || target_id == asker_id)
		return INSPECT_UNAVAILABLE;

	p = player_acquire(target_id);
	if(!p)
		return INSPECT_UNAVAILABLE;
	if(!p->is_loaded) {
		player_release(p);
		return INSPECT_UNAVAILABLE;
	}

	dx = p->pos_x - asker_x;
	dy = p->pos_y - asker_y;
	if(dx * dx + dy * dy > INSPECT_RANGE * INSPECT_RANGE) {
		player_release(p);
		return INSPECT_TOO_FAR;
	}

	copy_inspectable(p, out);
	player_release(p);

	return INSPECT_OK;
}

void handle_inspect_request(int client_fd, uint32_t character_id, int player_slot,
		const InspectRequestPacket* pkt) {
	InspectResponsePacket resp;
	uint32_t target_id = ntohl(pkt->target_id);
	float asker_x;
	float asker_y;

	if(!read_asker_position(character_id, player_slot, &asker_x, &asker_y))
		return;

	memset(&resp, 0, sizeof(resp));
	resp.header.type = PACKET_INSPECT_RESPONSE;
	resp.header.player_id = htonl(character_id);
	resp.header.payload_size = htons(sizeof(resp) - sizeof(PacketHeader));
	resp.target_id = pkt->target_id;
	resp.result = (uint8_t)inspect_target(character_id, target_id, asker_x, asker_y,
	                                      &resp);
	outbox_send(client_fd, &resp, sizeof(resp));
	LOG_DEBUG("[INSPECT] character %u inspected %u: result %u", character_id,
	          target_id, (unsigned)resp.result);
}
