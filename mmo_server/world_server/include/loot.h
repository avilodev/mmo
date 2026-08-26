/** @file Define loot tables and the world server's ground-item lifecycle. */

#ifndef LOOT_H
#define LOOT_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "tick_snapshot.h"

/** Exclusive-owner duration in seconds. */
#define LOOT_OWNER_TIME    30.0
/** Ground-item lifetime in seconds. */
#define LOOT_DESPAWN_TIME  120.0
/** Maximum pickup distance in world units. */
#define LOOT_PICKUP_RANGE  80.0f

/** How long a pickup reservation may stand before the item returns to the ground.
 *
 * A reservation is held only across an inventory insert, which is microseconds.
 * This exists so a code path that ever fails to resolve one cannot strand an
 * item invisibly: the sweep in loot_tick() puts it back rather than leaving it
 * on the ground forever, claimed by nobody.
 */
#define LOOT_RESERVATION_TIMEOUT 10.0

/** Define one probabilistic item and quantity range in a loot table. */
typedef struct {
    uint32_t item_id;
    float    drop_chance;       /**< 0.0 - 1.0 */
    uint8_t  min_qty;
    uint8_t  max_qty;
} LootEntry;

/** Associate an NPC type with the drops authored for it.
 *
 * The entry list grows with the file. It used to be a fixed eight, with the
 * ninth drop reported as an error and discarded -- a content limit expressed as
 * a compile-time constant in a header, which is the wrong place for a decision
 * that belongs to whoever writes items.json.
 */
typedef struct {
    uint16_t   npc_type_id;
    LootEntry* entries;
    int        entry_count;
    int        entry_capacity;
} LootTable;

/** Track one dropped item through ownership, reservation, pickup, or despawn. */
typedef struct {
    uint32_t id;                /**< Unique ground item identifier. */
    uint32_t item_id;           /**< Item definition identifier. */
    uint8_t  quantity;
    float    pos_x, pos_y;
    uint32_t owner_id;          /**< Killer's character_id (exclusive pickup). */
    double   drop_time;         /**< When it was dropped (CLOCK_MONOTONIC). */
    uint8_t  active;            /**< 1 = on ground, 0 = picked up or despawned. */

    /** Character currently attempting to pick this up, or 0.
     *
     * A reserved item is still on the ground -- it has not been destroyed --
     * but no other player can claim it. Pickup used to deactivate the item
     * first and insert into the inventory second, so if the insert stored
     * nothing (a full bag, a player who disconnected in between) the item was
     * already gone and the client was told "Player state changed". The item was
     * destroyed by a failed pickup.
     */
    uint32_t reserved_by;
    double   reserved_at;       /**< When the reservation was taken. */
} GroundItem;

/** Parse loot tables from the item JSON registry.
 *
 * @return 1 on success, or 0 when the file cannot be read or parsed.
 */
int  loot_init(const char* json_path);

/** Release every ground item and loot table. */
void loot_cleanup(void);

/** Roll an NPC's loot table and broadcast the drops to nearby players.
 *
 * @return The number of ground items created.
 */
int  loot_roll(uint16_t npc_type_id, float x, float y, uint32_t killer_id);

/** Create one ground item and broadcast it to nearby players.
 *
 * @return The assigned ground-item identifier, or 0 on allocation failure.
 */
uint32_t loot_drop_item(uint32_t item_id, uint8_t quantity, float x, float y,
                        uint32_t owner_id);

/** Copy one ground item out under the pool lock.
 *
 * Returning a pointer into the pool, as this used to, handed the caller an
 * address whose contents another thread was free to overwrite -- and did:
 * loot_tick() and loot_drop() both mutate the same slot, so a reader could see
 * a position belonging to a different item entirely.
 *
 * @return 1 when `out` was filled, or 0 when no active item carries that id.
 */
int  loot_snapshot(uint32_t ground_item_id, GroundItem* out);

/** Claim a ground item for a player without destroying it.
 *
 * The item stays on the ground, marked as this player's, until the caller
 * resolves the reservation with loot_commit() or loot_restore().
 *
 * @return 1 when reserved, or 0 when the item is absent, already reserved, or
 *         still inside another player's exclusive window.
 */
int  loot_reserve(uint32_t ground_item_id, uint32_t player_id,
                  uint32_t* out_item_id, uint8_t* out_quantity);

/** Finish a reservation: the item leaves the world and nearby players are told. */
void loot_commit(uint32_t ground_item_id);

/** Abandon a reservation: the item returns to the ground, unclaimed. */
void loot_restore(uint32_t ground_item_id, uint32_t player_id);

/** Handle one LOOT_PICKUP_REQUEST end to end.
 *
 * Lives here rather than inline in the packet dispatcher, which is where it
 * used to be -- the only opcode implemented in the router rather than in the
 * module that owns the state.
 */
void loot_handle_pickup_request(uint32_t character_id, int client_fd,
                                const uint8_t* buffer, ssize_t bytes);

/** Despawn expired ground items and tell the players near each one.
 *
 * Also returns any reservation older than LOOT_RESERVATION_TIMEOUT to the
 * ground, so a pickup path that ever fails to resolve one cannot strand an item.
 *
 * @param players  The pass's player snapshot, used to find who can see each
 *                 despawn; may be NULL, in which case items expire silently.
 */
void loot_tick(TickSnapshot* players);

/** Ground items currently on the world floor. For tests and metrics. */
size_t loot_active_count(void);

/** Slots the ground-item pool has grown to hold. For tests and metrics. */
size_t loot_pool_capacity(void);

/** Loot tables loaded from JSON. For tests and metrics. */
int loot_table_count(void);

#endif // LOOT_H
