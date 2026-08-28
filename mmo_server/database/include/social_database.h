#ifndef SOCIAL_DATABASE_H
#define SOCIAL_DATABASE_H

/** @file Store the account-scoped friend graph, its pending requests, and blocks.
 *
 * The graph is a graph of ACCOUNTS, never of characters. Characters are
 * world-exclusive -- `character_id` is a SERIAL in each world's own PostgreSQL
 * database, so character 5 exists in all ten worlds and names are unique only
 * per world -- which means nothing outside a world server can identify a
 * character from an id alone. `account_id` is already the Redis session key and
 * already a column on `characters`, so it is the only global identity there is.
 * You friend a person and are shown whichever character of theirs is online.
 *
 * This database is owned by the realm server. World servers never open it; they
 * forward mutations and read the Redis caches instead. That is the same rule
 * that keeps a world ignorant of every other world.
 *
 * SQLite today, alongside the account table in users_database.c. Moving to
 * PostgreSQL later changes nothing above this header.
 */

#include "presence.h"

#include <stdint.h>
#include <stddef.h>

/** Friends one account may hold.
 *
 * A cap and not a guideline: the friends list is read in one pipelined Redis
 * round trip whose size is this number, and every entry is a presence key
 * fetched on every panel open.
 */
#define MAX_FRIENDS 100

/** Outgoing requests one account may have pending at once.
 *
 * This is the harassment budget. Past it the request is refused with a reason
 * rather than silently dropped, because a silent drop teaches the sender to
 * click again.
 */
#define MAX_PENDING_REQUESTS 20

/* MAX_BLOCKS -- the cap this file enforces in social_block_add() -- is defined
 * in presence.h, not here. It bounds three arrays in three subsystems: the
 * realm's read below, the Redis set presence_blocks_store() writes, and the
 * world's block index. The cache contract is the one header all three include,
 * so that is where it lives.
 */

/** Days a pending request stands before it is swept.
 *
 * Swept lazily, on read, rather than by a cron job: there is no scheduler in
 * this service and a request nobody ever looks at costs one row.
 */
#define FRIEND_REQUEST_TTL_DAYS 30

/** Report what a mutation actually did.
 *
 * Every value here is reported to the requesting player except
 * FRIEND_RESULT_BLOCKED, which is deliberately indistinguishable from success
 * on the wire -- see social_friend_request().
 */
typedef enum {
    FRIEND_RESULT_OK = 0,            /**< Written; the target should be notified. */
    FRIEND_RESULT_ALREADY_FRIENDS,   /**< Nothing written. */
    FRIEND_RESULT_ALREADY_PENDING,   /**< Nothing written. */
    FRIEND_RESULT_MUTUAL,            /**< Crossing requests collapsed into a friendship. */
    FRIEND_RESULT_BLOCKED,           /**< Accepted, nothing written, nothing reported. */
    FRIEND_RESULT_SELF,              /**< Rejected. */
    FRIEND_RESULT_NOT_FOUND,         /**< No such account, or no such request/friendship. */
    FRIEND_RESULT_FRIEND_CAP,        /**< Either side is at MAX_FRIENDS. */
    FRIEND_RESULT_PENDING_CAP,       /**< Sender is at MAX_PENDING_REQUESTS. */
    FRIEND_RESULT_ERROR              /**< The database refused the write. */
} FriendResult;

/** One friend, as the panel needs them.
 *
 * Presence is not stored here -- it is read from Redis at panel-open time. The
 * last-seen fields are the durable fallback that renders an offline friend
 * without opening a world database.
 */
typedef struct {
    uint32_t account_id;
    uint32_t last_world_id;       /**< 0 when this account has never entered a world. */
    char     last_character_name[32];
    int64_t  last_seen_at;        /**< Unix seconds; 0 when never seen. */
} FriendEntry;

/** One pending incoming request. */
typedef struct {
    uint32_t from_account;
    char     from_name[32];       /**< Their most recent character name; "" when unknown. */
    int64_t  created_at;
} FriendRequestEntry;

/** Open the social database and create its tables.
 *
 * Safe to point at the same file users_database.c opens; the table names do
 * not collide and SQLite handles the two handles.
 *
 * @return Nonzero on success, otherwise zero.
 */
int social_db_init(const char* db_path);

/** Close the social database when open. */
void social_db_close(void);

/* --- Mutations -----------------------------------------------------------
 *
 * Each of these is one transaction. A half-landed friendship is invisible
 * afterwards -- each side's read is a single-direction scan that looks
 * perfectly healthy -- so there is no "repair on read" path and there must
 * never need to be one.
 */

/**
 * Record that `from_account` wants to be friends with `to_account`.
 *
 * The decision table, in the order it is evaluated:
 *
 *   | State found                     | Result                                  |
 *   | ------------------------------- | --------------------------------------- |
 *   | to == from                      | FRIEND_RESULT_SELF                      |
 *   | from is blocked by to           | FRIEND_RESULT_BLOCKED, nothing written  |
 *   | already friends                 | FRIEND_RESULT_ALREADY_FRIENDS           |
 *   | a pending from->to request      | FRIEND_RESULT_ALREADY_PENDING           |
 *   | either side at MAX_FRIENDS      | FRIEND_RESULT_FRIEND_CAP                |
 *   | a pending to->from request      | FRIEND_RESULT_MUTUAL, both rows written |
 *   | from at MAX_PENDING_REQUESTS    | FRIEND_RESULT_PENDING_CAP               |
 *   | nothing                         | FRIEND_RESULT_OK, request stored        |
 *
 * The friend cap is evaluated BEFORE the mutual collapse, not after. The
 * collapse creates a friendship, so a cap checked after it is not a cap; and
 * refusing at request time rather than at accept time tells the sender now,
 * instead of letting a request sit in someone's list that could never have been
 * accepted.
 *
 * The mutual case collapses to an accept because sending a request IS consent:
 * two crossing requests are two consents, and the pair is already a friendship.
 * Leaving either request row behind would show both players an invitation from
 * somebody who is already on their list.
 *
 * FRIEND_RESULT_BLOCKED is returned so the caller can log it. It must be
 * reported to the sender as FRIEND_RESULT_OK: telling someone they are blocked
 * is how you get a second account.
 */
FriendResult social_friend_request(uint32_t from_account, uint32_t to_account);

/**
 * Accept a pending `from_account` -> `to_account` request, as `to_account`.
 *
 * Inserts both directed friendship rows and deletes the request in one
 * transaction.
 */
FriendResult social_friend_accept(uint32_t to_account, uint32_t from_account);

/**
 * Decline a pending request, as its target.
 *
 * The sender is never told. Telling someone they were declined is how you get
 * a second request, and then a whisper.
 */
FriendResult social_friend_decline(uint32_t to_account, uint32_t from_account);

/** Withdraw an outgoing request, as its sender. */
FriendResult social_friend_cancel(uint32_t from_account, uint32_t to_account);

/**
 * Remove a friendship, as either side.
 *
 * Unilateral and needs no confirmation: deletes both directed rows.
 */
FriendResult social_friend_remove(uint32_t account, uint32_t friend_account);

/**
 * Block an account.
 *
 * Removes any friendship and any pending request in BOTH directions, then
 * records the block so future requests from them are refused. One transaction:
 * a block that removed the friendship but failed to record itself would let
 * the next request straight back through.
 *
 * Refused with FRIEND_RESULT_FRIEND_CAP once the blocker holds MAX_BLOCKS.
 */
FriendResult social_block_add(uint32_t account, uint32_t blocked_account);

/** Lift a block. Does not restore anything it removed. */
FriendResult social_block_remove(uint32_t account, uint32_t blocked_account);

/* --- Reads --------------------------------------------------------------- */

/**
 * List an account's friends, newest friendship first.
 *
 * @param out       Receives up to `max` entries.
 * @param max       Capacity of `out`.
 * @return          Entries written, or -1 on failure.
 */
int social_friend_list(uint32_t account, FriendEntry* out, int max);

/**
 * List the requests waiting for `account` to answer.
 *
 * Sweeps requests older than FRIEND_REQUEST_TTL_DAYS before reading, which is
 * the only thing that ever expires them.
 *
 * @return Entries written, or -1 on failure.
 */
int social_friend_requests_incoming(uint32_t account, FriendRequestEntry* out, int max);

/**
 * List the accounts `account` has outstanding requests to.
 *
 * @param out  Receives up to `max` account ids.
 * @return     Entries written, or -1 on failure.
 */
int social_friend_requests_outgoing(uint32_t account, uint32_t* out, int max);

/** Report whether two accounts are friends. */
int social_is_friend(uint32_t account, uint32_t other);

/** Report whether `account` has blocked `other`. */
int social_is_blocked(uint32_t account, uint32_t other);

/**
 * List the accounts `account` has blocked.
 *
 * The realm reads this to publish the block cache every world consults when it
 * decides whether a line of chat may be delivered; social_is_blocked() answers
 * one pair and is not usable on a per-message path.
 *
 * @param out  Receives up to `max` account ids, newest block first.
 * @param max  Capacity of `out`.
 * @return     Entries written, or -1 on failure.
 */
int social_block_list(uint32_t account, uint32_t* out, int max);

/** Count an account's friends, or -1 on failure. */
int social_friend_count(uint32_t account);

/* --- Presence cache -------------------------------------------------------
 *
 * One row per account, written when a player leaves a world. It is what renders
 * an offline friend -- "Kaelen, 3h ago" -- without opening a world database. A
 * cache of something a world knows, written once at the boundary and read many
 * times.
 */

/** Record where an account was last seen.
 *
 * Written on world ENTRY as well as on exit. Entry is what makes the name index
 * below usable: a player who has logged in once is findable by name from then
 * on, rather than only after their first clean logout.
 */
int social_presence_update(uint32_t account_id, uint32_t world_id,
                           uint32_t character_id, const char* character_name);

/**
 * Resolve a character name to the account that owns it.
 *
 * A player types a character name, because that is the only name they ever see.
 * Nothing else in this system can turn one into an account: character rows live
 * in ten separate PostgreSQL databases and are unique only per world, so
 * `UNIQUE(name, world_id)` means the same name can legitimately name two
 * different people on two different worlds.
 *
 * The presence cache is what closes that gap. It is the one table that has seen
 * every world's characters, and it is written on every world entry, so it holds
 * a name for anybody who has ever played.
 *
 * Ambiguity is resolved and then refused, in that order:
 *
 *   1. A match on `prefer_world_id` wins outright. Somebody typing a name is
 *      almost always looking at the person standing next to them.
 *   2. Otherwise a single match across all worlds is taken.
 *   3. Otherwise the name is ambiguous and this reports not-found, because
 *      guessing which of two strangers was meant is worse than saying no.
 *
 * @param prefer_world_id  The asking player's world, or 0 for no preference.
 * @param out_account      Receives the account id on success.
 * @return                 1 when exactly one account was resolved, otherwise 0.
 */
int social_account_by_character_name(const char* character_name,
                                     uint32_t prefer_world_id,
                                     uint32_t* out_account);

#endif // SOCIAL_DATABASE_H
