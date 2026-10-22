/**
 * @file
 * Handle authenticated world packets for movement, inventory, chat, parties, and sessions.
 */

#include "packet_handler.h"
#include "net_send.h"
#include "inventory_system.h"
#include "movement_system.h"
#include "party_commands.h"
#include "str_fixed.h"
#include "player_directory.h"
#include "log.h"
#include "config.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <arpa/inet.h>

/**
 * Record client latency and echo the ping packet back.
 *
 * The echo is the framed packet, byte for byte -- not a fixed
 * header-plus-two-bytes. The two differ whenever a client declares a
 * payload_size larger than the latency field, and the old fixed reply left the
 * surplus bytes unsent while the header the client reads still announced them:
 * the client's next read then took the following packet's header as this
 * packet's tail, and every packet after it on that connection was misframed.
 * The realm's ping route (realm_server/src/routes.c) already echoed what it
 * was given; this is the same rule on the world side.
 *
 * @param bytes  Length of the framed packet, as computed by the read loop from
 *               the declared payload_size. Never larger than what was received.
 */
void handle_ping(int client_fd, uint8_t* buffer, ssize_t bytes, uint32_t character_id, int player_slot) {
    PacketHeader* hdr = (PacketHeader*)buffer;
    if (bytes >= (ssize_t)(sizeof(PacketHeader) + sizeof(uint16_t)) &&
        ntohs(hdr->payload_size) >= sizeof(uint16_t)) {
        /* memcpy, not *(uint16_t*)(buffer + sizeof(PacketHeader)).
         *
         * PacketHeader is 7 bytes, so the latency field sits at an odd offset.
         * Casting the buffer to a uint16_t* asserts a 2-byte alignment that is
         * never there: undefined behaviour on any target, and on the AArch64
         * targets protocol.h names, a trap or a kernel fixup on the hot path.
         * memcpy of two bytes compiles to the same unaligned load x86 was
         * already doing, without the claim. The realm router carried the same
         * defect in its character-list case; see realm_server/src/routes.c. */
        uint16_t raw_ping = 0;
        memcpy(&raw_ping, buffer + sizeof(PacketHeader), sizeof(raw_ping));
        player_directory_record_ping(character_id, player_slot, ntohs(raw_ping));
    }
    if (bytes < (ssize_t)sizeof(PacketHeader)) return;
    server_send(client_fd, buffer, (size_t)bytes);
}

/** Send the requested character data to a client. */
void handle_request_player_data(int client_fd, uint32_t character_id) {
    player_directory_send_data(client_fd, character_id);
}


/** Pass a client movement proposal to the movement rules. */
void handle_player_move(int client_fd, uint32_t character_id, int player_slot, PlayerMovePacket* pkt) {
	movement_system_propose(client_fd, character_id, player_slot, pkt);
}

/** Parse an equip request and pass it to the inventory rules. */
void handle_equip_item(int client_fd, uint32_t character_id, uint8_t* buffer, ssize_t bytes) {
	EquipItemPacket* equip;

	if(bytes < (ssize_t)sizeof(EquipItemPacket)) {
		LOG_WARN_RL(5, 60, "Invalid equip packet size");
		return;
	}

	equip = (EquipItemPacket*)buffer;
	inventory_system_equip(client_fd, character_id, ntohl(equip->item_id),
	                       equip->inventory_slot, equip->equip_slot);
}

/** Parse an unequip request and pass it to the inventory rules. */
void handle_unequip_item(int client_fd, uint32_t character_id, uint8_t* buffer, ssize_t bytes) {
	UnequipItemPacket* unequip;

	if(bytes < (ssize_t)sizeof(UnequipItemPacket)) {
		LOG_WARN_RL(5, 60, "Invalid unequip packet size");
		return;
	}

	unequip = (UnequipItemPacket*)buffer;
	inventory_system_unequip(client_fd, character_id, unequip->equip_slot);
}

/** Parse a use-item request and pass it to the inventory rules. */
void handle_use_item(int client_fd, uint32_t character_id, uint8_t* buffer, ssize_t bytes) {
	UseItemPacket* use;

	if(bytes < (ssize_t)sizeof(UseItemPacket)) {
		LOG_WARN_RL(5, 60, "Invalid use item packet size");
		return;
	}

	use = (UseItemPacket*)buffer;
	inventory_system_use(client_fd, character_id, use->inventory_slot);
}

/** Parse a drop request and pass it to the inventory rules. */
void handle_drop_item(int client_fd, uint32_t character_id, uint8_t* buffer, ssize_t bytes) {
	DropItemPacket* drop;

	if(bytes < (ssize_t)sizeof(DropItemPacket)) {
		LOG_WARN_RL(5, 60, "Invalid drop packet size");
		return;
	}

	drop = (DropItemPacket*)buffer;
	inventory_system_drop(client_fd, character_id, drop->inventory_slot);
}

/** Parse a move request and pass it to the inventory rules. */
void handle_move_item(int client_fd, uint32_t character_id, uint8_t* buffer, ssize_t bytes) {
	MoveItemPacket* move;

	if(bytes < (ssize_t)sizeof(MoveItemPacket)) {
		LOG_WARN_RL(5, 60, "Invalid move item packet size");
		return;
	}

	move = (MoveItemPacket*)buffer;
	inventory_system_move(client_fd, character_id, move->from_slot, move->to_slot);
}

/* Chat lives in chat.c now.
 *
 * Recipient selection, whisper parsing and fan-out were written out here, in
 * the generic packet handler -- and the global-channel fan-out ran inline on
 * the network loop thread, taking the registry read lock and every online
 * player's slot mutex in turn for one player's keystroke. See chat.h.
 */

/** Parse a party invitation and pass it to the party commands. */
void handle_party_invite(int client_fd, uint32_t character_id, uint8_t* buffer, ssize_t bytes) {
	PartyInvitePacket* pkt;
	char target_name[32];

	if(bytes < (ssize_t)sizeof(PartyInvitePacket))
		return;

	pkt = (PartyInvitePacket*)buffer;
	memset(target_name, 0, sizeof(target_name));
	STR_COPY_FIELD(target_name, pkt->target_name);

	party_command_invite(client_fd, character_id, target_name);
}

/** Pass a party accept to the party commands. */
void handle_party_accept(int client_fd, uint32_t character_id) {
	party_command_accept(client_fd, character_id);
}

/** Pass a party decline to the party commands. */
void handle_party_decline(int client_fd, uint32_t character_id) {
	party_command_decline(client_fd, character_id);
}

/** Pass a party leave to the party commands. */
void handle_party_leave(int client_fd, uint32_t character_id) {
	party_command_leave(client_fd, character_id);
}

/** Parse a party kick and pass it to the party commands. */
void handle_party_kick(int client_fd, uint32_t character_id, uint8_t* buffer, ssize_t bytes) {
	PartyKickPacket* pkt;

	if(bytes < (ssize_t)sizeof(PartyKickPacket))
		return;

	pkt = (PartyKickPacket*)buffer;
	party_command_kick(client_fd, character_id, ntohl(pkt->target_id));
}
/** Parse a name query and pass it to the player directory. */
void handle_name_query_request(int client_fd, uint8_t* buffer, ssize_t bytes) {
	const ssize_t base = (ssize_t)offsetof(NameQueryRequestPacket, character_ids);
	NameQueryRequestPacket* req;
	uint8_t asked;

	if(bytes < base)
		return;

	req = (NameQueryRequestPacket*)buffer;
	asked = req->count;

	if(asked > MAX_NAME_QUERY)
		asked = MAX_NAME_QUERY;

	// believe the count only as far as the bytes that arrived
	while(asked > 0 && bytes < base + (ssize_t)asked * (ssize_t)sizeof(uint32_t))
		asked--;

	if(asked == 0)
		return;

	player_directory_names(client_fd, req->character_ids, asked);
}

/** Parse a roster request and pass it to the player directory. */
void handle_session_list_request(int client_fd, uint8_t* buffer, ssize_t bytes) {
	SessionListRequestPacket* req;

	if(!g_server.session_list_enabled) {
		LOG_DEBUG("[SESSION] roster request refused: session_list is off");
		return;
	}

	if(bytes < (ssize_t)sizeof(SessionListRequestPacket))
		return;

	req = (SessionListRequestPacket*)buffer;
	player_directory_session_page(client_fd, ntohs(req->page));
}
