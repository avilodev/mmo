#ifndef FRIEND_BUS_H
#define FRIEND_BUS_H

/** @file Carry friend events from the realm back out to the world servers.
 *
 * The other direction of the bus in presence.h. Mutations travel world -> realm
 * on a durable list, because a mutation is the record and must not be lost.
 * These travel realm -> world on pub/sub, because every one of them is
 * recoverable by re-reading: a dropped result leaves the durable rows correct
 * and the panel re-reads them when it next opens. That asymmetry is deliberate
 * and is the design's own rule -- pub/sub is an optimization, never the record.
 *
 * ## Addressing
 *
 * One channel per world, `friend:events:<world_id>`. The realm knows which
 * world a player is on because presence says so, so it publishes into exactly
 * one channel rather than fanning out to ten and having nine ignore it.
 *
 * A player who is offline gets nothing published at all. They are not missing
 * anything: the request is committed to the database, and their panel reads it
 * on their next world entry.
 *
 * ## Payloads
 *
 * Events are small on purpose. Nothing here carries a friends list -- the realm
 * refreshes the Redis caches first and then publishes a nudge, and the world
 * reads the list back out of Redis. That keeps one player's panel open off the
 * pub/sub bus that every world server is listening to.
 */

#include "presence.h"

#include <stdint.h>
#include <stddef.h>

/** What a friend event is telling a world server. */
typedef enum {
    /** One player's action finished. Send them a FRIEND_OP_RESULT. */
    FRIEND_EVENT_RESULT = 0,

    /** This account's `friends:` and `friendreq:` caches are fresh.
     *
     * Re-read them and send the panel packets. Used both to answer a panel open
     * and to push a change -- an accepted request, a removal -- to somebody who
     * did not ask for it.
     */
    FRIEND_EVENT_LIST_READY,

    /** Somebody just sent this account a friend request. */
    FRIEND_EVENT_REQUEST_NOTIFY,

    FRIEND_EVENT_COUNT_          /**< Bound for validation. Never sent. */
} FriendEventType;

/** One event addressed to one player on one world. */
typedef struct {
    FriendEventType type;
    uint32_t account_id;      /**< Whose client this is for. */
    uint32_t character_id;    /**< The exact session, or 0 for any of theirs. */
    uint8_t  action;          /**< FriendActionId, for a RESULT. */
    uint8_t  result;          /**< FriendWireResult, for a RESULT. */
    uint32_t peer_account;    /**< The other party, when there is one. */
    char     peer_name[PRESENCE_NAME_LEN];  /**< Echoed so a client can attribute it. */
} FriendEvent;

/** Publish one event into a world's channel.
 *
 * @param world_id  The world the recipient is on. Zero is refused: an event
 *                  with nowhere to go is a bug, not a broadcast.
 * @return          1 when Redis accepted the publish.
 */
int friend_event_publish(uint32_t world_id, const FriendEvent* e);

/* --- Subscriber -----------------------------------------------------------
 *
 * Run by each world server. One thread, one dedicated connection, subscribed to
 * this world's event channel and to the shared presence channel.
 */

/** Receive one friend event. Runs on the bus thread, never on a tick thread. */
typedef void (*FriendEventFn)(const FriendEvent* e, void* user);

/** Receive one presence change for any account, friend or not.
 *
 * Every world server sees every presence change: the channel is shared, and
 * filtering happens here rather than at the publisher, which has no idea who is
 * watching whom. The world's reverse index is what makes that cheap.
 */
typedef void (*PresenceEventFn)(const PresenceRecord* rec, void* user);

/** Start the subscriber thread.
 *
 * Idempotent: a second call while running is a no-op that reports success.
 *
 * @return 1 when the thread started, otherwise 0.
 */
int friend_bus_start(uint32_t world_id, FriendEventFn on_event,
                     PresenceEventFn on_presence, void* user);

/** Stop the subscriber thread and close its connection. Safe to call twice. */
void friend_bus_stop(void);

/** Report whether the subscriber is connected right now. For metrics and tests. */
int friend_bus_connected(void);

/* --- Encoding ------------------------------------------------------------ */

/** Encode an event. @return Bytes written excluding the terminator, or 0. */
size_t friend_event_encode(const FriendEvent* e, char* out, size_t out_size);

/** Decode an event. @return 1 on success, 0 when the message is unusable. */
int friend_event_decode(const char* s, FriendEvent* out);

#endif // FRIEND_BUS_H
