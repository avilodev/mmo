// Admission rules for the world server, between the network code and storage.

#include "world_entry_service.h"
#include "log.h"
#include "players_database.h"
#include "realm_world_auth.h"
#include "session.h"

world_entry_result world_entry_redeem(const char* ticket, const char* peer_ip,
                                      uint32_t world_id, uint32_t* out_account_id,
                                      uint32_t* out_character_id) {
	uint32_t account_id = 0;
	uint32_t character_id = 0;
	uint32_t ticket_world = 0;
	uint32_t owner;

	if(!validate_game_ticket(ticket, peer_ip, world_id, &account_id,
	                         &character_id, &ticket_world))
		return WORLD_ENTRY_BAD_TICKET;

	// a ticket for someone else's character must not claim a session slot
	owner = character_get_owner(character_id);

	if(owner != account_id) {
		LOG_WARN("[NET] character %u does not belong to account %u (owner=%u)",
		         character_id, account_id, owner);
		return WORLD_ENTRY_NOT_OWNER;
	}

	*out_account_id = account_id;
	*out_character_id = character_id;
	return WORLD_ENTRY_OK;
}

int world_entry_mark_live(uint32_t character_id, uint32_t world_id) {
	return world_session_mark(character_id, world_id);
}

void world_entry_clear_live(uint32_t character_id) {
	world_session_clear(character_id);
}

void world_entry_keep_login(uint32_t account_id) {
	session_touch(account_id);
}

int world_entry_start(void) {
	return session_init();
}

void world_entry_stop(void) {
	session_close();
}

int world_entry_realm_key_valid(const char* provided_key, const char* world_name) {
	return validate_server_auth_key(provided_key, world_name);
}

void world_entry_db_stats(int* in_use, int* size, int* waiters,
                          unsigned long* timeouts) {
	character_database_pool_stats(in_use, size, waiters);
	*timeouts = character_database_pool_timeouts();
}

int world_entry_db_pool_size(void) {
	return character_database_pool_configured_size();
}
