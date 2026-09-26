/**
 * @file
 * The world's single ground-zone pool: creation, lifetime, effects, broadcast.
 *
 * Extracted from ability_handler.c, which is where zones were born as a player
 * mechanic and where NPCs had no way to reach them. The extraction is what V6
 * needed, and it takes ability_handler.c back under the project's file-size
 * ceiling as a side effect.
 *
 * Two rules the old code did not have to state because it only had one kind of
 * caster:
 *
 *  - **Effects are directed by owner.** A player's zone reaches NPCs and heals
 *    the party; an NPC's zone reaches players and never its own allies.
 *  - **A caster has a budget.** A Rot Crawler lays one zone per tick. Without a
 *    per-caster cap it would own the pool inside twenty seconds and every other
 *    zone in the world would silently fail to spawn.
 */

#include "zone_owner.h"
#include "npc_effects.h"
#include "interest.h"
#include "player_data.h"
#include "player_effects.h"
#include "damage_model.h"
#include "log.h"
#include "utils.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

static ActiveZone*     g_zones;
static int             g_zone_capacity;
static int             g_zone_per_owner = ZONE_PER_OWNER_DEFAULT;
static pthread_mutex_t g_zones_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t        g_next_zone_id = 1;

/** Scratch for the zone tick's NPC query, sized to the NPC pool.
 *
 * Kept apart from the ability path's scratch: sharing one buffer between a zone
 * tick and a cast resolution meant a zone effect could resolve a cast's target
 * list out from under it.
 */
static int* g_zone_npc_candidates;
static int  g_zone_npc_capacity;

static float dist2d(float ax, float ay, float bx, float by) {
    float dx = bx - ax, dy = by - ay;
    return sqrtf(dx * dx + dy * dy);
}

/** Bound one zone broadcast's recipient list. A zone cannot interest more
 * players than can be online, and the buffer is 4 KB of stack for one call. */
#define ZONE_BROADCAST_RECIPIENTS MAX_PLAYERS

/** Widen an event radius so a zone stays visible from its own edge.
 *
 * Interest is measured from the zone's centre, but a large zone reaches players
 * standing well outside that centre's view radius -- and they are precisely the
 * players standing in it.
 */
static float zone_interest_radius(float zone_radius) {
    return INTEREST_EVENT_RADIUS + (zone_radius > 0.0f ? zone_radius : 0.0f);
}

/** Announce a zone spawn to every player who can see where it landed. */
static void broadcast_spawn_zone(const ActiveZone* z) {
    SpawnZonePacket pkt = {0};
    pkt.header.type         = PACKET_SPAWN_ZONE;
    pkt.header.player_id    = htonl(z->caster_id);
    pkt.header.payload_size = htons(sizeof(SpawnZonePacket) - sizeof(PacketHeader));
    pkt.zone_id       = htonl(z->zone_id);
    pkt.caster_id     = htonl(z->caster_id);
    pkt.ability_id    = htons(z->ability_id);
    pkt.pos_x         = z->pos_x;
    pkt.pos_y         = z->pos_y;
    pkt.duration      = z->duration_remaining;
    pkt.radius        = z->radius;
    pkt.has_collision = z->has_collision;

    int fds[ZONE_BROADCAST_RECIPIENTS];
    int count = interest_collect_fds(z->pos_x, z->pos_y,
                                     zone_interest_radius(z->radius),
                                     fds, ZONE_BROADCAST_RECIPIENTS);
    for (int i = 0; i < count; i++)
        server_send(fds[i], &pkt, sizeof(pkt));
}

/** Announce a zone removal to every player who could see it. */
static void broadcast_remove_zone(uint32_t zone_id, float px, float py, float radius) {
    RemoveZonePacket pkt = {0};
    pkt.header.type         = PACKET_REMOVE_ZONE;
    pkt.header.player_id    = 0;
    pkt.header.payload_size = htons(sizeof(RemoveZonePacket) - sizeof(PacketHeader));
    pkt.zone_id = htonl(zone_id);

    int fds[ZONE_BROADCAST_RECIPIENTS];
    int count = interest_collect_fds(px, py, zone_interest_radius(radius),
                                     fds, ZONE_BROADCAST_RECIPIENTS);
    for (int i = 0; i < count; i++)
        server_send(fds[i], &pkt, sizeof(pkt));
}

/* --- Pool ----------------------------------------------------------------- */

int zone_pool_init(int capacity, int per_owner) {
    if (capacity <= 0)                 capacity = ZONE_CAPACITY_DEFAULT;
    if (capacity > ZONE_CAPACITY_MAX)  capacity = ZONE_CAPACITY_MAX;
    if (per_owner <= 0)                per_owner = ZONE_PER_OWNER_DEFAULT;
    if (per_owner > ZONE_PER_OWNER_MAX) per_owner = ZONE_PER_OWNER_MAX;

    pthread_mutex_lock(&g_zones_lock);
    free(g_zones);
    g_zones = calloc((size_t)capacity, sizeof(*g_zones));
    if (!g_zones) {
        g_zone_capacity = 0;
        pthread_mutex_unlock(&g_zones_lock);
        LOG_ERROR("[ZONE] could not allocate a pool of %d zones", capacity);
        return 0;
    }
    g_zone_capacity  = capacity;
    g_zone_per_owner = per_owner;
    g_next_zone_id   = 1;
    pthread_mutex_unlock(&g_zones_lock);

    LOG_INFO("[ZONE] pool initialized: %d slots, %d per caster (%zu KB)",
             capacity, per_owner, ((size_t)capacity * sizeof(*g_zones)) / 1024);
    return 1;
}

int zone_pool_capacity(void)  { return g_zone_capacity; }
int zone_pool_per_owner(void) { return g_zone_per_owner; }

void zone_pool_shutdown(void) {
    pthread_mutex_lock(&g_zones_lock);
    free(g_zones);
    g_zones = NULL;
    g_zone_capacity = 0;
    pthread_mutex_unlock(&g_zones_lock);

    free(g_zone_npc_candidates);
    g_zone_npc_candidates = NULL;
    g_zone_npc_capacity = 0;
}

/** Grow the zone tick's NPC scratch to hold one entry per NPC slot. */
static int zone_npc_reserve(int capacity) {
    if (capacity <= g_zone_npc_capacity) return g_zone_npc_capacity;
    int* grown = realloc(g_zone_npc_candidates, (size_t)capacity * sizeof(int));
    if (!grown) {
        LOG_ERROR("[ZONE] could not size the zone NPC scratch to %d", capacity);
        return g_zone_npc_capacity;
    }
    g_zone_npc_candidates = grown;
    g_zone_npc_capacity = capacity;
    return g_zone_npc_capacity;
}

/* --- Creation -------------------------------------------------------------- */

/** Find a free slot, retiring this caster's oldest zone when it is at its cap.
 *
 * The caller must hold the zone lock.
 *
 * @return A slot index, or -1 when the pool is full and nothing could be retired.
 */
static int claim_slot(uint8_t owner_type, uint32_t caster_id) {
    int free_slot = -1;
    int owned = 0;
    int oldest = -1;
    float oldest_remaining = 1e30f;

    for (int i = 0; i < g_zone_capacity; i++) {
        if (!g_zones[i].is_active) {
            if (free_slot < 0) free_slot = i;
            continue;
        }
        if (g_zones[i].caster_id != caster_id ||
            g_zones[i].owner_type != owner_type) continue;
        owned++;
        if (g_zones[i].duration_remaining < oldest_remaining) {
            oldest_remaining = g_zones[i].duration_remaining;
            oldest = i;
        }
    }

    if (owned >= g_zone_per_owner && oldest >= 0) {
        /* Retire rather than refuse: a trail should move, not stop. */
        broadcast_remove_zone(g_zones[oldest].zone_id, g_zones[oldest].pos_x,
                              g_zones[oldest].pos_y, g_zones[oldest].radius);
        g_zones[oldest].is_active = 0;
        return oldest;
    }
    return free_slot;
}

uint32_t zone_create(const ZoneSpawnInfo* info) {
    if (!info || info->duration <= 0.0f) return 0;

    ActiveZone snapshot;

    pthread_mutex_lock(&g_zones_lock);
    int slot = claim_slot(info->owner_type, info->caster_id);
    if (slot < 0) {
        pthread_mutex_unlock(&g_zones_lock);
        LOG_WARN_RL(5, 60, "[ZONE] pool full at %d zones; a cast placed none",
                    g_zone_capacity);
        return 0;
    }

    ActiveZone* z = &g_zones[slot];
    memset(z, 0, sizeof(*z));
    z->is_active          = 1;
    z->zone_id            = g_next_zone_id++;
    z->caster_id          = info->caster_id;
    z->owner_type         = info->owner_type;
    z->ability_id         = info->ability_id;
    z->pos_x              = info->pos_x;
    z->pos_y              = info->pos_y;
    z->radius             = info->radius;
    z->has_collision      = info->has_collision;
    z->hp                 = info->hp;
    z->max_hp             = info->hp;
    z->duration_remaining = info->duration;
    z->healing_per_tick   = info->healing_per_tick;

    z->effect_count = 0;
    for (int i = 0; i < info->effect_count && i < MAX_ABILITY_EFFECTS; i++)
        z->effects[z->effect_count++] = info->effects[i];

    z->tick_rate = info->tick_rate > 0.0f ? info->tick_rate : 0.0f;
    if (z->tick_rate <= 0.0f) {
        for (int i = 0; i < z->effect_count; i++) {
            if (z->effects[i].tick_rate > 0.0f) { z->tick_rate = z->effects[i].tick_rate; break; }
        }
    }
    if (z->tick_rate <= 0.0f) z->tick_rate = 1.0f;
    z->tick_timer = z->tick_rate;

    snapshot = *z;
    uint32_t id = z->zone_id;
    pthread_mutex_unlock(&g_zones_lock);

    broadcast_spawn_zone(&snapshot);
    return id;
}

/* --- Tick ------------------------------------------------------------------ */

/** Apply one zone's periodic effects to the players standing in it. */
static void tick_players(const ActiveZone* z, TickSnapshot* players) {
    if (!players) return;

    /* Sized to the player table, which is what the snapshot draws from, so the
     * query cannot truncate. Everyone inside a zone is a real result, not a
     * distant one that may be dropped. */
    int inside[MAX_PLAYERS];
    int count = tick_snapshot_query(players, z->pos_x, z->pos_y, z->radius,
                                    inside, MAX_PLAYERS);

    for (int k = 0; k < count; k++) {
        int d = inside[k];
        if (players->is_dead[d]) continue;

        /* Revalidates the slot as it locks it: a player who logged out since the
         * snapshot was taken is skipped rather than applied to whoever took the
         * slot next. */
        ActivePlayer* target = player_acquire_slot(players->slot[d],
                                                   players->character_id[d]);
        if (!target) continue;

        if (z->owner_type == ZONE_OWNER_PLAYER) {
            if (z->healing_per_tick > 0) {
                target->health += z->healing_per_tick;
                if (target->health > target->max_health) target->health = target->max_health;
            }
            for (int e = 0; e < z->effect_count; e++)
                if (z->effects[e].reapply)
                    player_effect_apply(target, &z->effects[e], z->caster_id);
        } else {
            /* An NPC's zone is hostile ground. Its effects apply in full, and the
             * damage-over-time among them is what "standing in it hurts" means. */
            for (int e = 0; e < z->effect_count; e++)
                player_effect_apply(target, &z->effects[e], z->caster_id);
        }

        if (target->health > target->max_health) target->health = target->max_health;
        if (target->health < 0) target->health = 0;
        player_release(target);
    }
}

/** Apply one zone's periodic effects to the NPCs standing in it. */
static void tick_npcs(const ActiveZone* z, NPCWorld* world, NPCTickSnapshot* npcs,
                      int scratch_cap) {
    if (!npcs || !world || scratch_cap <= 0) return;
    /* An NPC's own zone never touches its allies. A Bio-Caster poisoning its own
     * pack is not a mechanic anyone asked for. */
    if (z->owner_type == ZONE_OWNER_NPC) return;

    int* inside = g_zone_npc_candidates;
    int count = npc_snapshot_query(npcs, z->pos_x, z->pos_y,
                                   z->radius + npcs->max_hitbox_radius,
                                   inside, scratch_cap);

    for (int k = 0; k < count; k++) {
        int c = inside[k];
        if (dist2d(z->pos_x, z->pos_y, npcs->pos_x[c], npcs->pos_y[c]) > z->radius)
            continue;

        NPCEntity* npc = npc_world_acquire_slot(world, npcs->slot[c], npcs->id[c]);
        if (!npc) continue;
        if (npc->is_alive)
            for (int e = 0; e < z->effect_count; e++)
                npc_effect_apply(world, npcs->slot[c], npc, &z->effects[e], z->caster_id);
        npc_world_release(world, npc);
    }
}

void zone_tick(NPCWorld* world, TickSnapshot* players, NPCTickSnapshot* npcs,
               double delta_time) {
    float dt = (float)delta_time;
    int scratch_cap = zone_npc_reserve(npc_world_capacity(world));

    /* Zones due to apply this tick, copied out from under the lock.
     *
     * Applying a zone's effects takes player and NPC locks. Holding the zone
     * lock across those would order zone-then-entity here, against the
     * entity-then-zone order any code that creates a zone while holding an
     * entity takes -- which is exactly what an NPC laying a trail does.
     * Retained across ticks and grown to the pool, so this costs no allocation
     * on a normal tick.
     */
    static ActiveZone* due;
    static int         due_capacity;
    int due_count = 0;

    if (due_capacity < g_zone_capacity) {
        ActiveZone* grown = realloc(due, (size_t)g_zone_capacity * sizeof(*grown));
        if (!grown) {
            LOG_ERROR("[ZONE] could not size the due-zone buffer to %d", g_zone_capacity);
            return;
        }
        due = grown;
        due_capacity = g_zone_capacity;
    }

    pthread_mutex_lock(&g_zones_lock);
    for (int z = 0; z < g_zone_capacity; z++) {
        ActiveZone* zone = &g_zones[z];
        if (!zone->is_active) continue;

        zone->duration_remaining -= dt;
        if (zone->duration_remaining <= 0.0f) {
            zone->is_active = 0;
            if (due_count < due_capacity) {
                due[due_count] = *zone;
                due[due_count].tick_timer = -1.0f;   /* marks "expired, not due" */
                due_count++;
            }
            continue;
        }

        zone->tick_timer -= dt;
        if (zone->tick_timer > 0.0f) continue;
        zone->tick_timer = zone->tick_rate;

        if (due_count < due_capacity) due[due_count++] = *zone;
    }
    pthread_mutex_unlock(&g_zones_lock);

    for (int i = 0; i < due_count; i++) {
        if (due[i].tick_timer < 0.0f) {
            broadcast_remove_zone(due[i].zone_id, due[i].pos_x, due[i].pos_y,
                                  due[i].radius);
            continue;
        }
        tick_players(&due[i], players);
        tick_npcs(&due[i], world, npcs, scratch_cap);
    }
}
