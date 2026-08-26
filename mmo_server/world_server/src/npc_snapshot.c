/**
 * @file
 * Sample living NPCs once per tick and build the gameplay spatial index over them.
 */

#include "npc_snapshot.h"

#include "log.h"
#include "world_collision.h"

#include <stdlib.h>
#include <string.h>

/**
 * Allocate a snapshot sized for a pool.
 *
 * @param snapshot  Snapshot to initialize.
 * @param world  Pool whose capacity sizes the dense arrays.
 * @return  1 on success, or 0 for NULL input or allocation failure.
 */
int npc_snapshot_init(NPCTickSnapshot* snapshot, const NPCWorld* world) {
    if (!snapshot || !world) return 0;

    int capacity = npc_world_capacity(world);
    if (capacity <= 0) return 0;

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->capacity = capacity;

    snapshot->slot          = calloc((size_t)capacity, sizeof(int));
    snapshot->id            = calloc((size_t)capacity, sizeof(uint32_t));
    snapshot->pos_x         = calloc((size_t)capacity, sizeof(float));
    snapshot->pos_y         = calloc((size_t)capacity, sizeof(float));
    snapshot->hitbox_radius = calloc((size_t)capacity, sizeof(float));
    snapshot->health        = calloc((size_t)capacity, sizeof(int32_t));
    snapshot->max_health    = calloc((size_t)capacity, sizeof(int32_t));
    snapshot->armor         = calloc((size_t)capacity, sizeof(int32_t));
    snapshot->xp_reward     = calloc((size_t)capacity, sizeof(uint32_t));
    snapshot->npc_type_id   = calloc((size_t)capacity, sizeof(uint16_t));
    snapshot->category      = calloc((size_t)capacity, sizeof(uint8_t));
    snapshot->dense_of_slot = calloc((size_t)capacity, sizeof(int));
    snapshot->points        = calloc((size_t)capacity, sizeof(SpatialPoint));

    if (!snapshot->slot || !snapshot->id || !snapshot->pos_x || !snapshot->pos_y ||
        !snapshot->hitbox_radius || !snapshot->health || !snapshot->max_health ||
        !snapshot->armor || !snapshot->xp_reward || !snapshot->npc_type_id ||
        !snapshot->category || !snapshot->dense_of_slot || !snapshot->points) {
        LOG_ERROR("[NPC] could not allocate a tick snapshot for %d NPCs", capacity);
        npc_snapshot_free(snapshot);
        return 0;
    }

    for (int i = 0; i < capacity; i++) snapshot->dense_of_slot[i] = -1;

    float world_w = 0.0f, world_h = 0.0f;
    world_collision_extent(&world_w, &world_h);

    snapshot->grid = spatial_grid_create(world_w, world_h,
                                         SPATIAL_GRID_DEFAULT_CELL, capacity);
    if (!snapshot->grid) {
        LOG_ERROR("[NPC] could not allocate the gameplay NPC grid");
        npc_snapshot_free(snapshot);
        return 0;
    }

    int cols = 0, rows = 0;
    spatial_grid_dimensions(snapshot->grid, &cols, &rows);
    LOG_INFO("[NPC] gameplay NPC grid %dx%d @ %.0fpx over %d slots",
             cols, rows, SPATIAL_GRID_DEFAULT_CELL, capacity);
    return 1;
}

/**
 * Release a snapshot's storage and grid.
 */
void npc_snapshot_free(NPCTickSnapshot* snapshot) {
    if (!snapshot) return;

    spatial_grid_destroy(snapshot->grid);
    free(snapshot->slot);
    free(snapshot->id);
    free(snapshot->pos_x);
    free(snapshot->pos_y);
    free(snapshot->hitbox_radius);
    free(snapshot->health);
    free(snapshot->max_health);
    free(snapshot->armor);
    free(snapshot->xp_reward);
    free(snapshot->npc_type_id);
    free(snapshot->category);
    free(snapshot->dense_of_slot);
    free(snapshot->points);

    memset(snapshot, 0, sizeof(*snapshot));
}

/**
 * Resample living NPCs and rebuild the tick-wide spatial grid.
 *
 * This function acquires the pool read lock and each sampled slot's lock in turn.
 */
void npc_snapshot_build(NPCTickSnapshot* snapshot, NPCWorld* world) {
    if (!snapshot || !world) return;

    // clear mappings populated by the previous snapshot
    for (int d = 0; d < snapshot->count; d++)
        snapshot->dense_of_slot[snapshot->slot[d]] = -1;

    snapshot->count = 0;
    snapshot->max_hitbox_radius = 0.0f;

    int count = 0;

    npc_world_read_begin(world);

    /* The occupied-slot list, not every slot in the pool. The snapshot is
     * rebuilt at the top of every gameplay tick, so this walk happens 20 times
     * a second and used to cost the pool's configured capacity each time --
     * a world sized for thousands paid for thousands whether or not they were
     * spawned. */
    int live_count = 0;
    const int* live = npc_world_live_slots(world, &live_count);

    for (int i = 0; i < live_count && count < snapshot->capacity; i++) {
        int s = live[i];
        NPCEntity* npc = npc_world_slot(world, s);
        if (!npc) continue;

        // Unlocked pre-filter. Reading id without the slot lock only decides
        // whether the slot is worth locking; every value that ends up in the
        // snapshot is read below, under the lock.
        if (npc->id == 0) continue;

        npc_world_slot_lock(world, s);
        int sampled = (npc->id != 0 && npc->is_alive);
        if (sampled) {
            snapshot->slot[count]          = s;
            snapshot->id[count]            = npc->id;
            snapshot->pos_x[count]         = npc->pos_x;
            snapshot->pos_y[count]         = npc->pos_y;
            snapshot->hitbox_radius[count] = npc->hitbox_radius;
            snapshot->health[count]        = npc->health;
            snapshot->max_health[count]    = npc->max_health;
            snapshot->armor[count]         = npc->armor;
            snapshot->xp_reward[count]     = npc->xp_reward;
            snapshot->npc_type_id[count]   = npc->npc_type_id;
            snapshot->category[count]      = npc->category;
        }
        npc_world_slot_unlock(world, s);

        if (!sampled) continue;

        snapshot->points[count].x  = snapshot->pos_x[count];
        snapshot->points[count].y  = snapshot->pos_y[count];
        snapshot->dense_of_slot[s] = count;

        if (snapshot->hitbox_radius[count] > snapshot->max_hitbox_radius)
            snapshot->max_hitbox_radius = snapshot->hitbox_radius[count];

        count++;
    }

    npc_world_read_end(world);

    snapshot->count = count;
    spatial_grid_build(snapshot->grid, snapshot->points, count);
}

/**
 * Query snapshot NPCs within a radius in nearest-first order.
 *
 * @return The number of dense indices written, limited by max_out.
 */
int npc_snapshot_query(NPCTickSnapshot* snapshot, float x, float y, float radius,
                       int* out_indices, int max_out) {
    if (!snapshot || !snapshot->grid) return 0;
    return spatial_grid_query(snapshot->grid, x, y, radius, out_indices, max_out);
}
