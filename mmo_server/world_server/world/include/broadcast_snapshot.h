#ifndef BROADCAST_SNAPSHOT_H
#define BROADCAST_SNAPSHOT_H

/** @file Define one coherent player-state sample shared by outbound tasks.
 * The broadcast thread owns each read-only snapshot for one scheduler pass.
 */

#include "net_tuning.h"
#include "spatial_grid.h"

#include <stdint.h>

/** Flatten one active player's broadcast-visible state. */
typedef struct {
    int      client_fd;
    uint32_t character_id;
    float    pos_x, pos_y, pos_z;
    uint8_t  grounded;   /**< Zero when the player is airborne. */
    int32_t  health, max_health;
    /** Fused race/class identifier. Both wire fields carry it; see protocol.h. */
    uint8_t  race_id;
	uint32_t appearance; /**< Body and fur colour, host order. */
    uint8_t  level;
    uint8_t  is_dead;
    uint16_t ping_ms;
    uint32_t party_id;   /**< Zero when the player has no party. */
	uint8_t emote; /**< EmoteId; EMOTE_NONE when not emoting. */
	uint8_t emote_arg;
	float speed; /**< Ground speed in units per second; zero when standing. */
	uint8_t heading; /**< Facing, 0-255 for a full turn. */
	uint8_t idle_fidget; /**< 0 standing; else 1 + the idle variation the owner plays. */
} BroadcastPlayer;

/** Pair a compact player array with per-shard interest grids using matching indices. */
typedef struct {
    const BroadcastPlayer* players;    /**< Compact range containing count entries. */
    int                    count;

    /** Retain one pass-local grid per broadcast shard.
     *
     * Deliberately an array rather than a single grid. spatial_grid_query()
     * writes its candidate set into scratch storage the grid owns, so one grid
     * cannot serve two threads at once — see the ownership note in
     * spatial_grid.h. Fan-out is sharded across threads that all query at the
     * same instant, so each shard indexes the same points into its own grid.
     *
     * Only entries below shard_count are allocated.
     */
    SpatialGrid* grids[BROADCAST_POOL_MAX_SHARDS];
    int          shard_count;
} BroadcastSnapshot;

#endif // BROADCAST_SNAPSHOT_H
