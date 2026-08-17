#ifndef BROADCAST_SNAPSHOT_H
#define BROADCAST_SNAPSHOT_H

/** @file Define one coherent player-state sample shared by outbound tasks.
 * The broadcast thread owns each read-only snapshot for one scheduler pass.
 */

#include "spatial_grid.h"

#include <stdint.h>

/** Flatten one active player's broadcast-visible state. */
typedef struct {
    int      client_fd;
    uint32_t character_id;
    float    pos_x, pos_y;
    int32_t  health, max_health;
    uint8_t  player_class;
    uint8_t  player_race;
    uint8_t  level;
    uint8_t  is_dead;
    uint16_t ping_ms;
    uint32_t party_id;   /**< Zero when the player has no party. */
} BroadcastPlayer;

/** Pair a compact player array with an interest grid using matching indices. */
typedef struct {
    const BroadcastPlayer* players;    /**< Compact range containing count entries. */
    int                    count;

    /** Retain a pass-local grid owned by the broadcast thread. */
    SpatialGrid*           grid;
} BroadcastSnapshot;

#endif // BROADCAST_SNAPSHOT_H
