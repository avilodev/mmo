#ifndef REALM_SERVICE_H
#define REALM_SERVICE_H

// Character and world-entry rules for the realm server. Network code calls
// this, never storage.

#include "character_record.h"

#include <stddef.h>
#include <stdint.h>

// size of a world-entry ticket as text, with its terminator
#define REALM_TICKET_SIZE 65

typedef enum {
	REALM_DELETE_OK = 0,
	REALM_DELETE_LIVE_IN_WORLD,
	REALM_DELETE_FAILED
} realm_delete_result;

typedef enum {
	REALM_TICKET_OK = 0,
	REALM_TICKET_NO_ENTROPY,
	REALM_TICKET_NO_PEER,
	REALM_TICKET_STORE_FAILED
} realm_ticket_result;

// open the session store the realm depends on, return 1 on success
int realm_service_start(void);

// report whether sessions can currently be validated
int realm_service_ready(void);

// close storage after a failed start
void realm_service_stop(void);

// close storage in the order a running realm needs
void realm_service_shutdown(void);

// return a caller-owned auth key for a world or "global", or NULL
char* realm_link_key(const char* name);

// report how many realm requests can reach the database at once
int realm_db_worker_count(void);

// return 1 when the session key is valid for the account at that address
int realm_session_valid(uint32_t account_id, const char* session_key,
                        const char* peer_ip);

// return how many characters were written, or a negative value on failure
int realm_character_list(uint32_t account_id, uint32_t world_id,
                         character_record* characters, int max_count);

// return how many characters the account has in the world
int realm_character_count(uint32_t account_id, uint32_t world_id);

// create a character and return 1 on success
int realm_character_create(uint32_t account_id, uint32_t world_id,
                           const char* name, int class_id, int race_id,
                           uint32_t appearance, uint32_t* out_character_id);

// return 1 when the character belongs to the account in that world
int realm_character_owned(uint32_t character_id, uint32_t account_id,
                          uint32_t world_id);

// delete a character, refusing while it is being played in a world
realm_delete_result realm_character_delete(uint32_t account_id,
                                           uint32_t character_id,
                                           uint32_t world_id,
                                           uint32_t* out_live_world);

// issue a single-use ticket that admits the character to the world
realm_ticket_result realm_issue_world_ticket(uint32_t account_id,
                                             uint32_t character_id,
                                             uint32_t world_id,
                                             const char* peer_ip,
                                             char* out_ticket);

#endif /* REALM_SERVICE_H */
