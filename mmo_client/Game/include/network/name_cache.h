#ifndef NAME_CACHE_H
#define NAME_CACHE_H

/** @file Names for the character identifiers the client sees but is not told.
 *
 * Nearby players arrive in the position broadcast as identifiers and nothing
 * else, and the client used to label them "Player_1042" -- a number where a
 * name belongs, above every stranger in the world.
 *
 * The obvious fix is a name field in the broadcast, and it is the wrong one: a
 * name never changes, and putting one in NearbyPlayerData spends 32 bytes per
 * player per broadcast -- thirty-two players, twenty times a second, forever --
 * resending a constant.
 *
 * So the client asks instead. An identifier it has never seen is queued; the
 * queue is sent as one request per batch, at most a few times a minute; the
 * answers are kept for the session. A name is fetched once, when its owner
 * first comes into view, and never again.
 *
 * Everything here runs on the render thread except name_cache_store(), which
 * runs on the network thread when the response arrives. The table is small,
 * fixed and guarded, and a lookup that races a store returns the pending
 * placeholder rather than a torn string.
 */

#include <stdint.h>

/** Identifiers held at once. Beyond this the oldest entry is recycled.
 *
 * A player sees far fewer distinct characters in a session than this in
 * practice; the cap exists so a very long session in a very busy city cannot
 * grow without bound. */
#define NAME_CACHE_CAPACITY 256

/** Start the cache empty. Safe to call again; clears everything held. */
void name_cache_reset(void);

/**
 * The name for an identifier, asking the server when it is not known.
 *
 * Never returns NULL. An identifier whose name has not arrived yet reads back
 * as a placeholder, so a stranger is drawn immediately and simply gains their
 * name a moment later rather than not being drawn at all.
 *
 * Requesting is rate-limited internally: an identifier is asked about once and
 * then not again for NAME_CACHE_RETRY_SECONDS, so a character who never
 * resolves -- one who logged out between being seen and being asked about --
 * cannot turn into a request every frame.
 *
 * @return A NUL-terminated name, valid until the next call on this thread.
 */
const char* name_cache_lookup(uint32_t character_id);

/** Record a name the server sent. Called from the packet dispatcher. */
void name_cache_store(uint32_t character_id, const char* name);

/**
 * Send any queued identifiers as one request.
 *
 * Called once a frame. Batching is what keeps this cheap: walking into a
 * crowd of thirty strangers costs one packet, not thirty.
 */
void name_cache_flush_requests(void);

/** Identifiers whose names are known. For tests. */
int name_cache_known_count(void);

/** Identifiers waiting to be asked about. For tests. */
int name_cache_pending_count(void);

#endif // NAME_CACHE_H
