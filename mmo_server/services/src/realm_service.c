// Character and world-entry rules for the realm server, between the network
// code and storage.

#include "realm_service.h"
#include "log.h"
#include "players_database.h"
#include "realm_friends.h"
#include "realm_world_auth.h"
#include "session.h"
#include "world_database_manager.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/random.h>

// bytes of entropy behind one world-entry ticket
#define TICKET_ENTROPY_BYTES 24

// seconds a world-entry ticket stays redeemable
#define TICKET_LIFETIME_SECONDS 60

_Static_assert(REALM_TICKET_SIZE >= TICKET_ENTROPY_BYTES * 2 + 1,
               "the ticket text must hold the entropy as hex");

// fill a buffer from the kernel random source, retrying only interruptions
static int fill_random(unsigned char* out, size_t len) {
	size_t filled = 0;

	while(filled < len) {
		ssize_t got = getrandom(out + filled, len - filled, 0);

		if(got < 0) {
			if(errno == EINTR)
				continue;
			return 0;
		}
		if(got == 0)
			return 0;
		filled += (size_t)got;
	}

	return 1;
}

// build a hex ticket, failing closed when there is no entropy
static int generate_ticket(char* ticket_out) {
	unsigned char random_bytes[TICKET_ENTROPY_BYTES];

	if(!fill_random(random_bytes, sizeof(random_bytes))) {
		LOG_ERROR("generate_ticket: getrandom failed: %s", strerror(errno));
		return 0;
	}

	for(size_t i = 0; i < sizeof(random_bytes); i++)
		snprintf(ticket_out + i * 2, REALM_TICKET_SIZE - i * 2, "%02x", random_bytes[i]);

	ticket_out[sizeof(random_bytes) * 2] = '\0';
	return 1;
}

int realm_service_start(void) {
	return session_init();
}

int realm_service_ready(void) {
	return session_is_ready();
}

void realm_service_stop(void) {
	character_database_close();
	session_close();
}

void realm_service_shutdown(void) {
	world_databases_cleanup();

	// the friends consumer must stop before session_close takes Redis away
	friends_stop();
	session_close();
}

char* realm_link_key(const char* name) {
	return get_server_auth_key(name);
}

int realm_db_worker_count(void) {
	return world_database_manager_conn_per_world();
}

int realm_session_valid(uint32_t account_id, const char* session_key,
                        const char* peer_ip) {
	return session_validate_from(account_id, session_key, peer_ip);
}

int realm_character_list(uint32_t account_id, uint32_t world_id,
                         character_record* characters, int max_count) {
	return world_character_get_list(account_id, world_id, characters, max_count);
}

int realm_character_count(uint32_t account_id, uint32_t world_id) {
	return world_character_count(account_id, world_id);
}

int realm_character_create(uint32_t account_id, uint32_t world_id,
                           const char* name, int class_id, int race_id,
                           uint32_t appearance, uint32_t* out_character_id) {
	return world_character_create(account_id, world_id, name, class_id, race_id,
	                              appearance, out_character_id);
}

int realm_character_owned(uint32_t character_id, uint32_t account_id,
                          uint32_t world_id) {
	return world_character_belongs_to_account(character_id, account_id, world_id);
}

realm_delete_result realm_character_delete(uint32_t account_id,
                                           uint32_t character_id,
                                           uint32_t world_id,
                                           uint32_t* out_live_world) {
	// a character that is being played must not lose its row underneath it
	uint32_t live_in = world_session_world(character_id);

	if(out_live_world)
		*out_live_world = live_in;

	if(live_in != 0)
		return REALM_DELETE_LIVE_IN_WORLD;

	if(!world_character_delete(account_id, character_id, world_id))
		return REALM_DELETE_FAILED;

	return REALM_DELETE_OK;
}

realm_ticket_result realm_issue_world_ticket(uint32_t account_id,
                                             uint32_t character_id,
                                             uint32_t world_id,
                                             const char* peer_ip,
                                             char* out_ticket) {
	char ticket[REALM_TICKET_SIZE];
	char ticket_key[128];
	char ticket_value[224];
	char trace_id[TRACE_ID_LEN];

	if(!generate_ticket(ticket))
		return REALM_TICKET_NO_ENTROPY;

	// an unbound ticket is a replayable credential, so no address means no ticket
	if(!peer_ip || !peer_ip[0])
		return REALM_TICKET_NO_PEER;

	// the trace id follows the player from login into the world
	if(!session_trace_id(account_id, trace_id, sizeof(trace_id)) || !trace_id[0])
		log_new_trace(trace_id);
	log_set_trace(trace_id);

	snprintf(ticket_key, sizeof(ticket_key), "ticket:%s", ticket);
	snprintf(ticket_value, sizeof(ticket_value), "%u:%u:%u:%s:%s",
	         character_id, world_id, account_id, peer_ip, trace_id);

	if(!store_game_ticket(ticket_key, ticket_value, TICKET_LIFETIME_SECONDS))
		return REALM_TICKET_STORE_FAILED;

	memcpy(out_ticket, ticket, REALM_TICKET_SIZE);
	return REALM_TICKET_OK;
}
