/**
 * @file
 * Publish a shared, read-only spatial index of living NPCs once per tick.
 */

#define _POSIX_C_SOURCE 200809L

#include "npc_query.h"

#include "log.h"
#include "spatial_grid.h"
#include "world_collision.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/** One buffer of sampled NPCs and the grid over them. */
typedef struct {
    int capacity;
    int count;

    int*      slot;
    uint32_t* id;
    float*    pos_x;
    float*    pos_y;
    float*    hitbox_radius;
    uint16_t* npc_type_id;

    float max_hitbox_radius;

    SpatialGrid*  grid;
    SpatialPoint* points;
} NpcIndex;

/* Double buffered: the gameplay thread fills the back buffer with no lock
 * held, then swaps under the write lock. A publish therefore blocks readers
 * for a pointer exchange rather than for the length of a rebuild. */
static NpcIndex  g_buffers[2];
static NpcIndex* g_published = NULL;
static NpcIndex* g_building  = NULL;

static pthread_rwlock_t g_lock = PTHREAD_RWLOCK_INITIALIZER;
static int g_ready = 0;

/** Release one buffer's storage. */
static void index_free(NpcIndex* index) {
    free(index->slot);
    free(index->id);
    free(index->pos_x);
    free(index->pos_y);
    free(index->hitbox_radius);
    free(index->npc_type_id);
    free(index->points);
    spatial_grid_destroy(index->grid);
    memset(index, 0, sizeof(*index));
}

/** Allocate one buffer sized for a pool. @return 1 on success. */
static int index_alloc(NpcIndex* index, int capacity) {
    memset(index, 0, sizeof(*index));
    index->capacity = capacity;

    index->slot          = calloc((size_t)capacity, sizeof(*index->slot));
    index->id            = calloc((size_t)capacity, sizeof(*index->id));
    index->pos_x         = calloc((size_t)capacity, sizeof(*index->pos_x));
    index->pos_y         = calloc((size_t)capacity, sizeof(*index->pos_y));
    index->hitbox_radius = calloc((size_t)capacity, sizeof(*index->hitbox_radius));
    index->npc_type_id   = calloc((size_t)capacity, sizeof(*index->npc_type_id));
    index->points        = calloc((size_t)capacity, sizeof(*index->points));

    float world_w = 0.0f, world_h = 0.0f;
    world_collision_extent(&world_w, &world_h);
    if (world_w <= 0.0f) world_w = 1.0f;
    if (world_h <= 0.0f) world_h = 1.0f;

    index->grid = spatial_grid_create(world_w, world_h,
                                      SPATIAL_GRID_DEFAULT_CELL, capacity);

    if (!index->slot || !index->id || !index->pos_x || !index->pos_y ||
        !index->hitbox_radius || !index->npc_type_id || !index->points || !index->grid) {
        index_free(index);
        return 0;
    }
    return 1;
}

/**
 * Allocate the index for a pool.
 *
 * @return 1 on success, or 0 on allocation failure.
 */
int npc_query_init(const NPCWorld* world) {
    if (!world) return 0;

    int capacity = world->capacity > 0 ? world->capacity : 1;

    if (!index_alloc(&g_buffers[0], capacity)) return 0;
    if (!index_alloc(&g_buffers[1], capacity)) {
        index_free(&g_buffers[0]);
        return 0;
    }

    pthread_rwlock_wrlock(&g_lock);
    g_published = &g_buffers[0];
    g_building  = &g_buffers[1];
    g_ready     = 1;
    pthread_rwlock_unlock(&g_lock);

    LOG_INFO("[NPC] shared query index ready (%d slots)", capacity);
    return 1;
}

/** Release the index. */
void npc_query_shutdown(void) {
    pthread_rwlock_wrlock(&g_lock);
    g_ready     = 0;
    g_published = NULL;
    g_building  = NULL;
    pthread_rwlock_unlock(&g_lock);

    index_free(&g_buffers[0]);
    index_free(&g_buffers[1]);
}

/**
 * Resample living NPCs and publish a new index.
 */
void npc_query_publish(NPCWorld* world) {
    if (!g_ready || !world) return;

    NpcIndex* back = g_building;
    back->count = 0;
    back->max_hitbox_radius = 0.0f;

    npc_world_read_begin(world);

    /* Occupied slots only. Published once per gameplay tick, so the difference
     * between "the NPCs that exist" and "the size of the pool" is paid 20
     * times a second. */
    int live_count = 0;
    const int* live = npc_world_live_slots(world, &live_count);

    for (int n = 0; n < live_count && back->count < back->capacity; n++) {
        int i = live[n];
        NPCEntity* npc = npc_world_slot(world, i);
        if (!npc || npc->id == 0) continue;   // unlocked pre-filter only

        npc_world_slot_lock(world, i);
        if (npc->is_alive && npc->id != 0) {
            int d = back->count++;
            back->slot[d]          = i;
            back->id[d]            = npc->id;
            back->pos_x[d]         = npc->pos_x;
            back->pos_y[d]         = npc->pos_y;
            back->hitbox_radius[d] = npc->hitbox_radius;
            back->npc_type_id[d]   = npc->npc_type_id;
            back->points[d].x      = npc->pos_x;
            back->points[d].y      = npc->pos_y;
            if (npc->hitbox_radius > back->max_hitbox_radius)
                back->max_hitbox_radius = npc->hitbox_radius;
        }
        npc_world_slot_unlock(world, i);
    }
    npc_world_read_end(world);

    spatial_grid_build(back->grid, back->points, back->count);

    /* Only the swap is under the write lock. */
    pthread_rwlock_wrlock(&g_lock);
    NpcIndex* previous = g_published;
    g_published = back;
    g_building  = previous;
    pthread_rwlock_unlock(&g_lock);
}

/**
 * Collect living NPCs within a radius, nearest first.
 *
 * @return Number of hits written.
 */
int npc_query_lookup(uint32_t npc_id, NpcQueryHit* out) {
    if (!npc_id || !out) return 0;

    pthread_rwlock_rdlock(&g_lock);

    NpcIndex* index = g_published;
    int found = 0;
    if (g_ready && index) {
        /* A linear walk, deliberately. There is no identifier index over the
         * published copy, and this answers one question for one feared player on
         * one movement packet -- adding a hash to save a few hundred compares on
         * a path that already does a collision trace would be storage and
         * invalidation for nothing. */
        for (int i = 0; i < index->count; i++) {
            if (index->id[i] != npc_id) continue;
            out->slot          = index->slot[i];
            out->id            = index->id[i];
            out->npc_type_id   = index->npc_type_id[i];
            out->pos_x         = index->pos_x[i];
            out->pos_y         = index->pos_y[i];
            out->hitbox_radius = index->hitbox_radius[i];
            found = 1;
            break;
        }
    }

    pthread_rwlock_unlock(&g_lock);
    return found;
}

int npc_query_near(float x, float y, float radius, NpcQueryHit* out, int max_out) {
    if (!out || max_out <= 0) return 0;

    pthread_rwlock_rdlock(&g_lock);

    NpcIndex* index = g_published;
    if (!g_ready || !index || index->count == 0) {
        pthread_rwlock_unlock(&g_lock);
        return 0;
    }

    /* Scratch belongs to this call, not to the grid, which is what makes the
     * same published index queryable from several threads at once. */
    size_t scratch_bytes = spatial_grid_scratch_bytes(index->grid);
    void*  scratch       = malloc(scratch_bytes);
    int*   dense         = malloc(sizeof(int) * (size_t)max_out);

    int written = 0;
    if (scratch && dense) {
        int n = spatial_grid_query_into(index->grid, x, y, radius,
                                        dense, max_out, scratch, scratch_bytes);
        for (int i = 0; i < n; i++) {
            int d = dense[i];
            if (d < 0 || d >= index->count) continue;
            out[written].slot          = index->slot[d];
            out[written].id            = index->id[d];
            out[written].npc_type_id   = index->npc_type_id[d];
            out[written].pos_x         = index->pos_x[d];
            out[written].pos_y         = index->pos_y[d];
            out[written].hitbox_radius = index->hitbox_radius[d];
            written++;
        }
    } else {
        LOG_ERROR("[NPC] out of memory during a spatial query");
    }

    pthread_rwlock_unlock(&g_lock);

    free(scratch);
    free(dense);
    return written;
}

/** The largest hitbox radius in the published index. */
float npc_query_max_hitbox_radius(void) {
    pthread_rwlock_rdlock(&g_lock);
    float r = (g_ready && g_published) ? g_published->max_hitbox_radius : 0.0f;
    pthread_rwlock_unlock(&g_lock);
    return r;
}

/** Living NPCs in the published index. */
int npc_query_count(void) {
    pthread_rwlock_rdlock(&g_lock);
    int n = (g_ready && g_published) ? g_published->count : 0;
    pthread_rwlock_unlock(&g_lock);
    return n;
}
