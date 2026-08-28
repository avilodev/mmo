#ifndef PRESENCE_H
#define PRESENCE_H

/** @file Carry account presence, the friends cache, and the friend mutation bus over Redis.
 *
 * Three things live here, and they are together because they are one bus.
 *
 * 1. **Presence** -- `presence:<account>`, a hash holding the world, character
 *    and name an account is currently playing as, under a TTL the world
 *    refreshes. A world server that crashes takes its players offline on its
 *    own, with no cleanup pass and no stale "online" rows to reconcile.
 *
 * 2. **The friends cache** -- `friends:<account>`, a set of account ids mirroring
 *    the durable rows the realm owns. Opening a friends list is then one
 *    pipelined read of ~20 `presence:` keys: one round trip, no PostgreSQL, no
 *    fan-out across ten world databases.
 *
 * 3. **The mutation bus** -- how a world server asks the realm to change the
 *    graph, and how the answer comes back.
 *
 * ## Why the mutation bus is here and not on the world-to-realm link
 *
 * friends_design.md put mutations on "the world-to-realm link that already
 * exists (realm_net.c, the heartbeat path)". That link cannot carry them. It is
 * realm-initiated and strictly one request for one response: the realm sends
 * PACKET_WORLD_HEARTBEAT and the world answers PACKET_WORLD_STATUS, on a
 * WORLD_QUERY_TIMEOUT cycle of 15 seconds (realm_server/src/main.c). A world
 * server has no way to speak first on it. Riding mutations there would mean up
 * to 15 seconds to deliver a friend request and another 15 to return its
 * result, so clicking "Add Friend" would take half a minute to do anything.
 *
 * Redis is already the shared substrate for the other two layers, so mutations
 * use it too. They go on a durable LIST -- world LPUSHes, realm BRPOPs -- and
 * NOT on pub/sub, because the design's own rule is that pub/sub is an
 * optimization and never the record, and a mutation IS the record. A list
 * survives a realm restart; a publish into a channel nobody is subscribed to is
 * gone. Results and presence changes, which are both recoverable by re-reading,
 * do travel on pub/sub.
 */

#include <stdint.h>
#include <stddef.h>

/** Seconds an account's presence stands before the world must refresh it.
 *
 * Comfortably longer than the 15-second realm heartbeat that renews it. A TTL
 * near the refresh interval would let presence lapse in the moment before it
 * was renewed, and a lapsed key reads as "offline" to every friend watching.
 */
#define PRESENCE_TTL_SECONDS 120

/** Redis list the world pushes friend mutations onto and the realm drains. */
#define FRIEND_MUTATION_QUEUE "friend:mutations"

/** Redis channel carrying presence changes to every world server. */
#define PRESENCE_CHANNEL "presence"

/** Longest character name carried on this bus, including its terminator.
 *
 * Matches the 32-byte name fields in protocol.h. Kept as its own constant so
 * common/ does not have to include the wire protocol to encode a message.
 */
#define PRESENCE_NAME_LEN 32

/** What an account is doing right now. */
typedef struct {
    uint32_t account_id;
    uint32_t world_id;                       /**< 0 when offline. */
    uint32_t character_id;                   /**< 0 when offline. */
    char     character_name[PRESENCE_NAME_LEN];
    int64_t  since;                          /**< Unix seconds this presence began. */
    int      online;                         /**< 0 when this record says "left". */
} PresenceRecord;

/** One change a world asks the realm to make to the friend graph. */
typedef enum {
    FRIEND_OP_REQUEST = 0,
    FRIEND_OP_ACCEPT,
    FRIEND_OP_DECLINE,
    FRIEND_OP_CANCEL,
    FRIEND_OP_REMOVE,
    FRIEND_OP_BLOCK,
    FRIEND_OP_UNBLOCK,
    FRIEND_OP_LIST,          /**< Not a mutation: re-send the list through the realm. */
    FRIEND_OP_COUNT_         /**< Bound for validation. Never sent. */
} FriendOp;

/** A mutation in flight from a world server to the realm. */
typedef struct {
    FriendOp op;
    uint32_t actor_account;      /**< Who asked. Always the authenticated account. */
    uint32_t target_account;     /**< Who it is about; 0 when only a name is known. */
    uint32_t origin_world_id;    /**< Where the answer should be published. */
    uint32_t actor_character_id; /**< For addressing the reply to one session. */
    char     target_name[PRESENCE_NAME_LEN]; /**< Set when the player typed a name. */
} FriendMutation;

/* --- Presence ------------------------------------------------------------ */

/** Record that an account is playing, and publish the change.
 *
 * @return 1 when Redis accepted both the key and the publish, otherwise 0.
 */
int presence_set(uint32_t account_id, uint32_t world_id, uint32_t character_id,
                 const char* character_name);

/** Record that an account has left, and publish the change.
 *
 * Deletes the key rather than writing an "offline" one: absence already means
 * offline, and a stored offline record would have to be expired by something.
 */
int presence_clear(uint32_t account_id);

/** Clear presence only if it still names this character.
 *
 * A world transfer is a leave on the old world racing an enter on the new one,
 * in two processes with no ordering between them. When the leave lands second,
 * an unconditional clear deletes the presence the new world just wrote -- and
 * nothing puts it back, because the heartbeat only extends the TTL of a key
 * that exists. The player then reads as offline to every friend until they log
 * out and back in.
 *
 * Read-compare-delete, not atomic: the window shrinks from a whole session to
 * the gap between two commands. Closing it entirely would mean a Lua script,
 * which is a much larger dependency than this race is worth.
 *
 * @return 1 when the presence belonged to this character and was cleared, 0
 *         when it belonged to somebody else (or nobody) and was left alone.
 */
int presence_clear_if_character(uint32_t account_id, uint32_t character_id);

/** Extend the TTL on presence keys without rewriting them.
 *
 * Pipelined: one round trip for the whole batch, because this runs for every
 * player on the server every heartbeat.
 *
 * @return Keys refreshed, or -1 on failure.
 */
int presence_refresh_many(const uint32_t* account_ids, int count);

/** Read one account's presence.
 *
 * @return 1 when the account is online and `out` is filled, 0 when it is not.
 */
int presence_get(uint32_t account_id, PresenceRecord* out);

/** Read many accounts' presence in one pipelined round trip.
 *
 * `out[i]` corresponds to `account_ids[i]`; an offline account is written as a
 * zeroed record with `online` clear rather than omitted, so the caller does not
 * have to re-align two arrays.
 *
 * @return Accounts found online, or -1 on failure.
 */
int presence_get_many(const uint32_t* account_ids, int count, PresenceRecord* out);

/** Publish a presence change to every subscribed world. */
int presence_publish(const PresenceRecord* rec);

/* --- Friends cache ------------------------------------------------------- */

/** One cached friend: who they are, and what to call them.
 *
 * The name is carried here rather than looked up per friend because a friend
 * who is offline has no presence record to read one from, and a row that can
 * only say "account 41007" is a row nobody can act on. It is the character the
 * account was last seen playing, which is also the name the panel's own
 * commands take -- so what is displayed is what can be typed back.
 */
typedef struct {
    uint32_t account_id;
    char     name[PRESENCE_NAME_LEN];
} FriendCacheEntry;

/** Replace an account's cached friend set.
 *
 * Called by the realm after a mutation and by a world on a cache miss. A count
 * of zero stores an explicit empty marker, so "no friends" is distinguishable
 * from "not cached" -- without it, every panel open by a friendless player
 * would miss the cache and hit the realm.
 *
 * Replace-only: there is deliberately no incremental add or remove. The names
 * come from the realm's authoritative read, and a caller holding only two
 * account ids has nothing to write for the name -- which is exactly how a
 * cache full of unnamed rows would get created.
 */
int presence_friends_store(uint32_t account_id, const FriendCacheEntry* friends,
                           int count);

/** Read an account's cached friend set.
 *
 * @return Entries written, or -1 when the account is not cached at all.
 */
int presence_friends_load(uint32_t account_id, FriendCacheEntry* out, int max);

/** Drop an account's cached set, forcing the next read through to the realm. */
int presence_friends_drop(uint32_t account_id);

/* --- Pending-request cache ------------------------------------------------
 *
 * The friends panel shows two things, and only one of them is a friend. Pending
 * requests live in the realm's database like everything else, so they are
 * cached here beside `friends:` for the same reason: a world server opening a
 * panel must not have to wait on the realm to draw it.
 */

/** One pending incoming request, as the panel needs it. */
typedef struct {
    uint32_t from_account;
    int64_t  created_at;
    char     from_name[PRESENCE_NAME_LEN];  /**< "" when they have never played. */
} FriendRequestCache;

/** Replace an account's cached pending requests.
 *
 * A count of zero stores an explicit empty marker, so a player with no pending
 * requests is distinguishable from one whose cache has not been built.
 */
int presence_requests_store(uint32_t account_id, const FriendRequestCache* reqs, int count);

/** Read an account's cached pending requests.
 *
 * @return Entries written, or -1 when the account is not cached at all.
 */
int presence_requests_load(uint32_t account_id, FriendRequestCache* out, int max);

/** Drop an account's cached requests. */
int presence_requests_drop(uint32_t account_id);

/* --- Mutation bus -------------------------------------------------------- */

/** Hand one mutation to the realm. Returns 1 when Redis took it. */
int friend_mutation_push(const FriendMutation* m);

/** Take the next mutation off the queue, waiting up to `timeout_secs`.
 *
 * Blocking, and therefore on its own connection rather than a pooled one: a
 * BRPOP holds its connection for the whole wait, and a pooled connection parked
 * in one would starve every other Redis user in the process.
 *
 * @return 1 when `out` was filled, 0 on timeout, -1 when Redis is unreachable.
 */
int friend_mutation_pop(FriendMutation* out, int timeout_secs);

/** Close the dedicated connection the queue consumer and subscriber hold. */
void presence_consumer_close(void);

/* --- Encoding ------------------------------------------------------------
 *
 * Exposed for the tests, and because a wire format nobody can exercise
 * separately from Redis is one nobody exercises. Both are total: a decode of
 * anything, including a truncated or hostile string, either fills the struct or
 * reports failure, and never reads past the buffer.
 *
 * The name is always the LAST field and is taken verbatim to the end of the
 * string, so a separator inside a character name cannot split the message.
 */

/** Encode a mutation. @return Bytes written excluding the terminator, or 0. */
size_t friend_mutation_encode(const FriendMutation* m, char* out, size_t out_size);

/** Decode a mutation. @return 1 on success, 0 when the message is unusable. */
int friend_mutation_decode(const char* s, FriendMutation* out);

/** Encode a presence change. @return Bytes written, or 0. */
size_t presence_encode(const PresenceRecord* rec, char* out, size_t out_size);

/** Decode a presence change. @return 1 on success, 0 when unusable. */
int presence_decode(const char* s, PresenceRecord* out);

#endif // PRESENCE_H
