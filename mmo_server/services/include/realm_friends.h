#ifndef REALM_FRIENDS_H
#define REALM_FRIENDS_H

/* Named realm_friends.h rather than friends.h because the world server has one
 * too, and the test suite compiles both include directories at once: with two
 * files of the same basename, whichever -I came first won and the other's
 * declarations silently vanished. */

/** @file Apply friend mutations on the realm, and publish what they changed.
 *
 * The realm is the only writer of the friend graph, for the same reason it is
 * the only writer of character rows: account-scoped state belongs to the
 * service that owns account-scoped state, and a world server that could reach
 * the account database would be a world server that knows about the other nine.
 *
 * World servers therefore forward every mutation here over the Redis queue in
 * presence.h, and this drains it. The answer goes back on the event bus in
 * friend_bus.h.
 *
 * ## The ordering rule
 *
 * Commit, then update the caches, then publish. Never the reverse. If a cache
 * write fails the durable record is still correct and the cache rebuilds on the
 * next read-through; if a publish is dropped the panel re-reads on open. A
 * publish that outran its commit would announce a friendship that is not there.
 */

#include "presence.h"

#include <stdint.h>

/** Open the social database and start draining the mutation queue.
 *
 * @param db_path  The social SQLite file. Created if it does not exist.
 * @return         1 when the consumer thread is running, otherwise 0.
 */
int friends_start(const char* db_path);

/** Stop the consumer thread and close the social database. Safe to call twice. */
void friends_stop(void);

/** Rebuild an account's Redis caches from the durable rows and announce them.
 *
 * @param world_id  Where to publish the announcement, or 0 to refresh the
 *                  caches without telling anybody.
 */
void friends_refresh_caches(uint32_t account_id, uint32_t world_id);

/** Apply one mutation and publish its consequences.
 *
 * Exposed so the decision table can be tested without a queue and a thread in
 * the way. Ordinary callers never invoke this; the consumer thread does.
 */
void friends_apply(const FriendMutation* m);

#endif // REALM_FRIENDS_H
