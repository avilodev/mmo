// Keeps each player's chosen status and idle clock.

#include "status_handler.h"
#include "status_rules.h"
#include "friends.h"
#include "outbox.h"
#include "player_data.h"
#include "utils.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

// Tell a player which status the server now shows for them.
static void send_state(int client_fd, uint32_t character_id, uint8_t status) {
	StatusStatePacket pkt;

	memset(&pkt, 0, sizeof(pkt));
	pkt.header.type = PACKET_STATUS_STATE;
	pkt.header.player_id = htonl(character_id);
	pkt.header.payload_size = htons(sizeof(pkt) - sizeof(PacketHeader));
	pkt.status = status;
	outbox_send(client_fd, &pkt, sizeof(pkt));
}

void status_note_activity(int client_fd, uint32_t character_id, int player_slot) {
	ActivePlayer* p = player_acquire_hint(character_id, player_slot);
	double now = get_time_seconds();
	uint8_t was;
	uint8_t shown;

	if(!p)
		return;

	was = status_effective(p->status_choice, p->last_activity_at, now);

	// Acting ends AFK; Do Not Disturb and Offline stay until the player changes them.
	if(p->status_choice == SESSION_STATUS_AFK)
		p->status_choice = SESSION_STATUS_ONLINE;
	p->last_activity_at = now;
	shown = status_effective(p->status_choice, p->last_activity_at, now);
	player_release(p);

	if(shown != was)
		send_state(client_fd, character_id, shown);
}

void status_handle_request(int client_fd, uint32_t character_id, int player_slot,
		uint8_t status) {
	ActivePlayer* p;
	double now = get_time_seconds();
	uint8_t shown;
	uint8_t was_choice;
	uint32_t account_id;
	char name[32];

	if(status >= SESSION_STATUS_COUNT)
		return;

	p = player_acquire_hint(character_id, player_slot);
	if(!p)
		return;

	was_choice = p->status_choice;
	account_id = p->account_id;
	snprintf(name, sizeof(name), "%s", p->username);
	p->status_choice = status;

	// Going back to Online also restarts the idle clock, so they do not flip to AFK.
	if(status == SESSION_STATUS_ONLINE)
		p->last_activity_at = now;
	shown = status_effective(p->status_choice, p->last_activity_at, now);
	player_release(p);

	send_state(client_fd, character_id, shown);

	// Offline hides them from friends and leaving it shows them again; Redis is
	// reached here, after the lock is released.
	if(status == SESSION_STATUS_OFFLINE && was_choice != SESSION_STATUS_OFFLINE)
		world_friends_set_visible(account_id, character_id, name, 0);
	else if(status != SESSION_STATUS_OFFLINE && was_choice == SESSION_STATUS_OFFLINE)
		world_friends_set_visible(account_id, character_id, name, 1);
}
