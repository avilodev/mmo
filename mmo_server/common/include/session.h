#ifndef SESSION_H
#define SESSION_H

/** @file Expose Redis-backed sessions, authentication tokens, and player-state caching. */

#include "types.h"

#include <stdint.h>
#include <time.h>
#include <stddef.h>
#include <hiredis/hiredis.h>

/** Configure the default Redis endpoint and session-cache lifetimes. */
#define REDIS_HOST "127.0.0.1"
#define REDIS_PORT 6379
#define SESSION_EXPIRY_SECONDS 300
#define PLAYER_STATE_EXPIRY_SECONDS 300    /**< Hot-cache lifetime in seconds. */
#define SESSION_KEY_LENGTH 32
#define MAX_SESSIONS 10000

/** Report whether the session system holds a usable Redis pool. */
int session_is_ready(void);

/** Lease one pooled Redis connection to the calling thread.
 *
 * There used to be one connection behind one process-wide mutex, so every
 * session validate, ticket mint and ticket consume in the service queued on
 * the same lock -- the busiest moment the system has, everybody logging in at
 * once, was the moment it serialized itself hardest.
 *
 * Re-entrant, so a helper may issue commands inside a caller's span. Every
 * acquire must be paired with redis_pool_release(), including a failed one.
 *
 * @return 1 when a connection is held, or 0 when none could be leased.
 */
int redis_pool_acquire(void);

/** Return this thread's leased Redis connection to the pool. */
void redis_pool_release(void);

/** Connections in the Redis pool. For tests and metrics. */
size_t redis_pool_size(void);

/** Represent the fixed fields stored for one account session. */
typedef struct {
    uint32_t account_id;
    char session_key[SESSION_KEY_LENGTH];
    time_t created_at;
    time_t expires_at;
    char ip_address[16];
} Session;

int session_store(uint32_t player_id, const char* session_key);
void session_remove(uint32_t player_id);
// return an allocated 32-character key that the caller frees
char* generate_session_key(void);


int session_init(void);
int session_create(uint32_t account_id, const char* ip_address, char* out_session_key);

/** Read the correlation id minted for an account's session.
 *
 * The login server mints one per session and stores it beside the session key;
 * the realm adopts it so its lines about this player carry the same id, and
 * passes it into the world ticket so the world does too. One `grep <id>`
 * across the three services' logs then gathers a player's whole login.
 *
 * @param out       Buffer of at least TRACE_ID_LEN bytes. Always terminated.
 * @param out_size  Its capacity.
 * @return          1 when a trace id was found, otherwise 0 and `out` empty.
 */
int session_trace_id(uint32_t account_id, char* out, size_t out_size);

/**
 * Validate a session key without checking where it is being presented from.
 *
 * Equivalent to session_validate_from(account_id, session_key, NULL).
 */
int session_validate(uint32_t account_id, const char* session_key);

/**
 * Validate a session key and require it to be presented from the address the
 * login server saw when it issued the session.
 *
 * The realm and world links are plaintext TCP and carry this key and the world
 * ticket in the clear, so anyone able to observe the traffic can replay both.
 * Binding them to the observed source address does not make the link private,
 * but it does mean a captured key is not by itself a usable credential from
 * somewhere else.
 *
 * @param peer_ip  Address the key is being presented from, or NULL to skip the
 *                 check. A session stored with an unknown address is accepted
 *                 from anywhere, because there is nothing to compare against.
 */
int session_validate_from(uint32_t account_id, const char* session_key,
                          const char* peer_ip);
void session_close(void);

/* Deliberately absent: session_mark_active(), session_refresh() and
 * session_invalidate(). All three were exported and never called; see the note
 * at their former site in session.c.
 *
 * Deliberately absent: session_cleanup_expired().
 *
 * It ran KEYS session:* plus one HGET per key, under the process-wide Redis
 * mutex, once per accepted login connection -- O(sessions) of blocking Redis
 * work on the one path that is busiest exactly when the session count is
 * highest. Every session already carries an EXPIRE set by session_create() and
 * refreshed by session_validate(), so Redis expires them without being asked.
 */

 
int session_cache_player_state(const ActivePlayer* player);
int session_load_cached_state(uint32_t player_id, ActivePlayer* player);
void session_clear_cached_state(uint32_t player_id);

/** Issue one Redis command with the sticky-context heal and one retry.
 *
 * The caller must already hold a lease from redis_pool_acquire().
 *
 * Exposed because realm_world_auth.c used redisCommand() directly and so
 * inherited none of this: after a single Redis blip its context stayed broken,
 * and every realm-to-world authentication failed permanently -- silently taking
 * the whole world list offline -- until the process was restarted.
 *
 * @return The reply, which the caller frees, or NULL when Redis is unreachable.
 */
redisReply* redis_command_locked(const char* format, ...);

/* --- Live world sessions -------------------------------------------------
 *
 * A world server records which characters it currently has in play, so the
 * realm can refuse to delete one out from under a live session. Without this a
 * character could be deleted while playing: the world's next save UPDATEd zero
 * rows and reported success, and its item writes re-INSERTed rows for a
 * character that no longer existed.
 *
 * Marks carry a TTL and are refreshed by the world, so a world that crashes
 * does not block deletion forever.
 */

/** Seconds a world-session mark stands before it must be refreshed.
 *
 * Comfortably longer than the world's periodic save interval, which is what
 * renews it (SAVE_INTERVAL_SECONDS in world_server/src/player_data.c, 120s).
 * A TTL equal to the refresh interval would let a mark lapse in the moment
 * before it was renewed, and a lapsed mark is a character that can be deleted
 * while it is being played.
 */
#define WORLD_SESSION_TTL_SECONDS 300

/** Record that a character is live in a world, or refresh an existing mark.
 *
 * @return 1 when Redis accepted the mark, otherwise 0.
 */
int world_session_mark(uint32_t character_id, uint32_t world_id);

/** Clear a character's world-session mark. */
void world_session_clear(uint32_t character_id);

/** Report which world a character is currently live in.
 *
 * @return The world identifier, or 0 when no live session is recorded.
 */
uint32_t world_session_world(uint32_t character_id);

int store_game_ticket_in_redis(const char* key, const char* value, int expiry_seconds);

/**
 * Consume a realm-issued world-entry ticket.
 *
 * @param peer_ip           Address presenting the ticket. The realm records the
 *                          address it issued the ticket to, and a mismatch is
 *                          refused: a ticket travels in cleartext and is worth
 *                          an account takeover to whoever captures it.
 * @param expected_world_id The world redeeming the ticket. A ticket names the
 *                          world it was minted for, and that field used to be
 *                          parsed and then discarded -- so a ticket for world A
 *                          was accepted by world B, and a character could be
 *                          entered into a world its realm never authorised,
 *                          against a different database. Pass 0 only where the
 *                          redeeming world genuinely is not known.
 */
int validate_game_ticket(const char* game_ticket, const char* peer_ip,
                         uint32_t expected_world_id,
                         uint32_t* out_account_id, uint32_t* out_character_id,
                         uint32_t* out_world_id);

// accept terminated 32-character tokens and consume them once
int     auth_token_store(const char* token, uint32_t player_id);
uint32_t auth_token_consume(const char* token);

#endif