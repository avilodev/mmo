/**
 * @file
 * Snapshot active players and build the gameplay tick's spatial-query index.
 */

#include "tick_snapshot.h"

#include "log.h"
#include "player_data.h"
#include "world_collision.h"

#include <pthread.h>
#include <string.h>

extern ActivePlayer active_players[];

/**
 * Initialize a tick snapshot and allocate its spatial grid.
 *
 * @return 1 on success, or 0 for NULL input or allocation failure.
 */
int tick_snapshot_init(TickSnapshot* snapshot) {
    if (!snapshot) return 0;

    memset(snapshot, 0, sizeof(*snapshot));
    for (int i = 0; i < MAX_PLAYERS; i++) snapshot->dense_of_slot[i] = -1;

    float world_w = 0.0f, world_h = 0.0f;
    world_collision_extent(&world_w, &world_h);

    snapshot->grid = spatial_grid_create(world_w, world_h,
                                         SPATIAL_GRID_DEFAULT_CELL, MAX_PLAYERS);
    if (!snapshot->grid) {
        LOG_ERROR("[TICK] could not allocate the gameplay interest grid");
        return 0;
    }

    int cols = 0, rows = 0;
    spatial_grid_dimensions(snapshot->grid, &cols, &rows);
    LOG_INFO("[TICK] gameplay interest grid %dx%d @ %.0fpx",
             cols, rows, SPATIAL_GRID_DEFAULT_CELL);
    return 1;
}

/**
 * Release a tick snapshot's spatial grid and clear its count.
 */
void tick_snapshot_free(TickSnapshot* snapshot) {
    if (!snapshot) return;
    spatial_grid_destroy(snapshot->grid);
    snapshot->grid = NULL;
    snapshot->count = 0;
}

/**
 * Resample loaded players and rebuild the tick-wide spatial grid.
 *
 * This function acquires the registry read lock and each sampled player's slot lock.
 */
void tick_snapshot_build(TickSnapshot* snapshot) {
    if (!snapshot) return;

    // clear mappings populated by the previous snapshot
    for (int d = 0; d < snapshot->count; d++)
        snapshot->dense_of_slot[snapshot->slot[d]] = -1;

    int count = 0;

    player_registry_rdlock();
    int online_count = 0;
    const int* online = player_active_list_locked(&online_count);

    for (int n = 0; n < online_count; n++) {
        int i = online[n];
        if (!active_players[i].is_loaded) continue;

        // sample mutable player fields under the slot lock
        pthread_mutex_lock(&active_players[i].lock);
        snapshot->slot[count]         = i;
        snapshot->character_id[count] = active_players[i].character_id;
        snapshot->pos_x[count]        = active_players[i].pos_x;
        snapshot->pos_y[count]        = active_players[i].pos_y;
        snapshot->is_dead[count]      = active_players[i].is_dead;
        snapshot->client_fd[count]    = active_players[i].client_fd;
        snapshot->form[count]         = active_players[i].form;
        snapshot->health[count]       = active_players[i].health;
        snapshot->max_health[count]   = active_players[i].max_health;
        pthread_mutex_unlock(&active_players[i].lock);

        snapshot->points[count].x = snapshot->pos_x[count];
        snapshot->points[count].y = snapshot->pos_y[count];
        snapshot->dense_of_slot[i] = count;
        count++;
    }
    player_registry_unlock();

    snapshot->count = count;
    spatial_grid_build(snapshot->grid, snapshot->points, count);
}

/**
 * Query snapshot players within a radius in nearest-first order.
 *
 * @return The number of dense indices written, limited by max_out.
 */
int tick_snapshot_query(TickSnapshot* snapshot, float x, float y, float radius,
                        int* out_indices, int max_out) {
    if (!snapshot || !snapshot->grid) return 0;
    return spatial_grid_query(snapshot->grid, x, y, radius, out_indices, max_out);
}

/**
 * Resolve a character identifier to its dense snapshot index.
 *
 * @return The dense index, or -1 when absent or the source slot was recycled.
 */
int tick_snapshot_find(const TickSnapshot* snapshot, uint32_t character_id) {
    if (!snapshot || character_id == 0) return -1;

    // map character to slot and slot to dense index
    int slot = player_slot_of(character_id);
    if (slot < 0 || slot >= MAX_PLAYERS) return -1;

    int dense = snapshot->dense_of_slot[slot];
    if (dense < 0 || dense >= snapshot->count) return -1;

    // reject slots recycled since the snapshot
    if (snapshot->character_id[dense] != character_id) return -1;
    return dense;
}
