#ifndef SHOP_SESSION_H
#define SHOP_SESSION_H

#include <stdint.h>

/** @file Track which shop each character currently has open.
 *
 * Buying and selling used to take a shop_id straight off the wire and act on
 * it. Any shop, from anywhere in the world, with no requirement that the
 * player had ever opened it -- and because each kingdom's shops trade in that
 * kingdom's own coin, a client could buy in one currency and sell into another
 * without ever leaving the spot it was standing on. shop_validate() only ever
 * guarded resale *inside* one shop.
 *
 * So a shop is now something a character is *at*: opened by talking to a
 * merchant, remembered here with the NPC that offered it, re-checked for range
 * on every purchase and every sale, and dropped when the character walks away,
 * goes quiet, or leaves the world.
 *
 * The table is an open-addressed map keyed by character id that grows with the
 * population rather than being sized for MAX_PLAYERS: shops are open for
 * seconds at a time, so the live set is a small fraction of who is online.
 */

/** How far a character may stray from the merchant before the shop closes. */
#define SHOP_INTERACT_RANGE 100.0f

/** Seconds of no buying or selling before an open shop is dropped. */
#define SHOP_SESSION_TIMEOUT 120.0

/** One character's open shop. */
typedef struct {
    uint32_t character_id;
    uint32_t shop_id;
    uint32_t npc_id;        /**< The merchant; its live position gates range. */
    float    npc_x, npc_y;  /**< Where it stood when the shop opened. */
    double   last_activity;
} ShopSession;

/** Record that a character opened a shop, replacing any shop already open.
 *
 * @return 1 when stored, or 0 when the table could not grow.
 */
int shop_session_open(uint32_t character_id, uint32_t shop_id,
                      uint32_t npc_id, float npc_x, float npc_y);

/** Copy a character's open shop out under the lock.
 *
 * @param out  Receives the session; untouched when none is open.
 * @return     1 when a shop is open, otherwise 0.
 */
int shop_session_snapshot(uint32_t character_id, ShopSession* out);

/** Mark a session as still in use, so the timeout sweep leaves it alone. */
void shop_session_touch(uint32_t character_id);

/** Forget a character's open shop. Safe for a character that has none. */
void shop_session_close(uint32_t character_id);

/** Drop sessions idle for longer than SHOP_SESSION_TIMEOUT.
 *
 * Cheap enough for the gameplay tick: it walks only the map, which holds one
 * entry per character with a shop open, not one per player.
 */
void shop_session_tick(void);

/** Release the table. */
void shop_session_shutdown(void);

/** Sessions currently open. For tests and metrics. */
int shop_session_count(void);

#endif // SHOP_SESSION_H
