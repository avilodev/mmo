#ifndef WORLD_ENTRY_SERVICE_H
#define WORLD_ENTRY_SERVICE_H

// Admission rules for the world server. Network code calls this, never storage.

#include <stdint.h>

typedef enum {
	WORLD_ENTRY_OK = 0,
	WORLD_ENTRY_BAD_TICKET,
	WORLD_ENTRY_NOT_OWNER
} world_entry_result;

// redeem a ticket for this world and check the account owns the character
world_entry_result world_entry_redeem(const char* ticket, const char* peer_ip,
                                      uint32_t world_id, uint32_t* out_account_id,
                                      uint32_t* out_character_id);

// record that the character is being played in the world
int world_entry_mark_live(uint32_t character_id, uint32_t world_id);

// clear the record that the character is being played
void world_entry_clear_live(uint32_t character_id);

// extend the account's login session so a dropped player can reconnect without logging in
void world_entry_keep_login(uint32_t account_id);

// open the session store the world depends on, return 1 on success
int world_entry_start(void);

// close what world_entry_start opened
void world_entry_stop(void);

// return 1 when the realm's key for this world is valid
int world_entry_realm_key_valid(const char* provided_key, const char* world_name);

// read the database pool's load and how often callers gave up waiting
void world_entry_db_stats(int* in_use, int* size, int* waiters,
                          unsigned long* timeouts);

// report how many database connections this process will open
int world_entry_db_pool_size(void);

#endif /* WORLD_ENTRY_SERVICE_H */
