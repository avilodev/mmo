#ifndef WORLD_FRIENDS_H
#define WORLD_FRIENDS_H

/** @file Serve the friends panel from this world, and watch friends elsewhere.
 *
 * The world server owns none of the friend graph. It reads the Redis caches the
 * realm maintains, forwards every change to the realm over the mutation queue,
 * and pushes what comes back to the right local session.
 *
 * ## The reverse index
 *
 * Presence changes are published on ONE channel that every world server hears,
 * because the publisher has no idea who is watching whom. Filtering therefore
 * happens here, and it must not be a scan: ten thousand players logging in and
 * out is a stream of changes, and answering each one by walking a thousand
 * local player slots and a hundred friend ids apiece is a per-event cost that
 * grows with how busy the game is.
 *
 * So each local player's friend set is inverted on entry into
 * `watched_account -> [local characters watching them]`, and a presence change
 * is one hash lookup. Ten thousand players at twenty friends each is 200k
 * entries spread over ten processes -- 20k apiece, which is nothing.
 *
 * ## Threading
 *
 * Everything here is called from one of three places: a network loop thread
 * (client packets), the event bus thread (pushes from the realm), or the
 * heartbeat (TTL refresh). The index has its own lock and nothing below it ever
 * takes a player slot lock while holding it.
 */

#include "types.h"

#include <stdint.h>
#include <sys/types.h>

/** Start the event bus and prepare the index. Call once, after session_init().
 *
 * @param world_id  This world's id, which is the channel events arrive on.
 * @return          1 when the bus is running, otherwise 0. A world that cannot
 *                  reach the bus still runs; its players simply see an empty
 *                  friends panel.
 */
int world_friends_init(uint32_t world_id);

/** Stop the event bus and drop the index. Safe to call twice. */
void world_friends_shutdown(void);

/** Publish presence and build this player's watch set.
 *
 * Called once the character is loaded and addressable, not at connect: the
 * name published here is what every one of their friends sees.
 */
void world_friends_player_entered(uint32_t account_id, uint32_t character_id,
                                  const char* character_name);

/** Clear presence and tear down this player's watch set. */
void world_friends_player_left(uint32_t account_id, uint32_t character_id);

/** Refresh the TTL on every local player's presence key.
 *
 * Driven by the realm heartbeat, which is the cadence PRESENCE_TTL_SECONDS is
 * sized against. A world that stops calling this has its whole population read
 * as offline by everybody else two minutes later.
 */
void world_friends_heartbeat(void);

/** Handle one friend packet from a client.
 *
 * The account is resolved here from the character rather than passed in. The
 * router does not carry one, and every mutation this sends is attributed to an
 * account the realm then trusts -- so there is exactly one place that decides
 * who a packet came from, and it is the player slot.
 *
 * @param bytes  Length of `buffer`; every handler checks its own packet's size
 *               before reading past the header.
 */
void world_friends_handle_packet(int client_fd, uint32_t character_id,
                                 const uint8_t* buffer, ssize_t bytes);

/** Count the entries in the reverse index. For metrics and tests. */
int world_friends_watch_count(void);

#endif // WORLD_FRIENDS_H
