#ifndef TICK_SNAPSHOT_H
#define TICK_SNAPSHOT_H

/** @file Snapshot player positions once for all phases of a gameplay tick.
 * The gameplay thread owns the unlocked copy; player mutations still require slot locks.
 */

#include "types.h"
#include "spatial_grid.h"

#include <stdint.h>

/** Pair dense player arrays with a pass-local spatial index. */
typedef struct {
    /** Cover the compact half-open range from zero through count. */
    int      slot[MAX_PLAYERS];          // index into active_players[]
    uint32_t character_id[MAX_PLAYERS];
    float    pos_x[MAX_PLAYERS];
    float    pos_y[MAX_PLAYERS];
    uint8_t  is_dead[MAX_PLAYERS];
    int      client_fd[MAX_PLAYERS];
    int      count;

    /** Map active-player slots to dense indices, using -1 when absent. */
    int dense_of_slot[MAX_PLAYERS];

    SpatialGrid*  grid;                  /**< Index built over the copied positions. */
    SpatialPoint  points[MAX_PLAYERS];   /**< Scratch storage used during grid rebuilds. */
} TickSnapshot;

// allocate on the gameplay thread and return zero on failure
int  tick_snapshot_init(TickSnapshot* snapshot);
void tick_snapshot_free(TickSnapshot* snapshot);

// rebuild once per tick while briefly holding the registry read lock
void tick_snapshot_build(TickSnapshot* snapshot);

// write nearest-first dense indices and return at most max_out
int tick_snapshot_query(TickSnapshot* snapshot, float x, float y, float radius,
                        int* out_indices, int max_out);

// return the dense index or -1 when absent from this tick
int tick_snapshot_find(const TickSnapshot* snapshot, uint32_t character_id);

#endif // TICK_SNAPSHOT_H
