/**
 * @file
 * Manage movement, collision, damage, and broadcasts for world projectiles.
 * Serialize projectile mutations while deferring network sends until locks are released.
 */

#include "projectile.h"
#include "npc_mitigation.h"
#include "npc_triggers.h"

#include "interest.h"
#include "log.h"
#include "npc_world.h"
#include "player_effects.h"
#include "log.h"
#include "combat_stats.h"
#include "player_data.h"
#include "player_level.h"
#include "party.h"
#include "loot.h"
#include "quest_system.h"

#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <time.h>
#include "utils.h"

extern ActivePlayer active_players[];

static Projectile*     g_projectiles;
static int             g_projectile_capacity;
static pthread_mutex_t g_projectiles_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t        g_next_projectile_id = 1;

/** Deferred sends per projectile.
 *
 * Derived from the projectile pool rather than picked as a round number: a
 * projectile can contribute at most one destroy, one effect, one XP award, and
 * one loot roll in a single tick, so four entries per projectile cannot overflow.
 *
 * The fixed 256 this replaced was below that ceiling and the overflow was silent —
 * the queue simply stopped accepting, and the packets it dropped were despawns,
 * so clients kept rendering projectiles that no longer existed. Exactly the
 * failure that only appears once the server is busy enough to matter.
 */
#define DEFERRED_SENDS_PER_PROJECTILE 4

typedef enum {
    DSEND_PROJECTILE_DESTROY,
    DSEND_ABILITY_EFFECT,
    DSEND_XP_AWARD,
    DSEND_LOOT_ROLL
} DeferredSendType;

typedef struct {
    DeferredSendType type;
    int     client_fd;
    union {
        struct {
            uint32_t projectile_id;
            uint8_t  reason;
            /** Despawn position; recipients are resolved at flush time. */
            float    pos_x, pos_y;
        } destroy;
        struct {
            uint32_t caster_id;
            uint32_t target_id;
            uint16_t ability_id;
            int      damage;
            int      healing;
            int      target_new_hp;
            uint8_t  is_kill;
        } effect;
        struct {
            uint32_t killer_id;
            uint64_t xp;
            int      client_fd;
        } xp;
        struct {
            uint16_t npc_type_id;
            float    npc_x, npc_y;
            uint32_t killer_id;
        } loot;
    };
} DeferredSend;

/** One projectile's position, copied out under the pool lock for broadcasting. */
typedef struct {
    uint32_t id;
    float px, py;
} ProjSnapshot;

static _Thread_local ProjSnapshot* t_proj_snaps;
static _Thread_local int           t_snap_capacity;

typedef struct {
    DeferredSend* items;
    int count;
    int capacity;
} DeferredQueue;

/** Retain one deferred-send buffer per thread.
 *
 * The queue is used from more than one thread: projectile_tick() runs on the
 * gameplay thread, but projectile_remove() is reachable from a packet thread.
 * A shared buffer would race, and allocating per call would malloc twenty times
 * a second for the life of the server, so each thread keeps its own and grows it
 * once. Same arrangement as npc_ai.c's queue, for the same reason.
 */
static _Thread_local DeferredSend* t_dq_items;
static _Thread_local int           t_dq_capacity;

static void dq_init(DeferredQueue* q) {
    int want = g_projectile_capacity * DEFERRED_SENDS_PER_PROJECTILE;
    if (want > t_dq_capacity) {
        DeferredSend* grown = realloc(t_dq_items, (size_t)want * sizeof(*grown));
        if (grown) {
            t_dq_items    = grown;
            t_dq_capacity = want;
        } else {
            LOG_ERROR("[PROJECTILE] could not size the deferred queue to %d sends; "
                      "holding at %d", want, t_dq_capacity);
        }
    }
    q->items    = t_dq_items;
    q->capacity = t_dq_capacity;
    q->count    = 0;
}

static void dq_push(DeferredQueue* q, const DeferredSend* item) {
    if (q->count >= q->capacity) {
        /* Only reachable when the allocation above failed; never silently. */
        LOG_WARN_RL(5, 60, "[PROJECTILE] deferred queue full at %d sends — "
                           "a projectile packet was dropped this tick",
                    q->capacity);
        return;
    }
    q->items[q->count++] = *item;
}

static float dist2d(float ax, float ay, float bx, float by) {
    float dx = bx - ax;
    float dy = by - ay;
    return sqrtf(dx * dx + dy * dy);
}

static double get_monotonic_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/**
 * Send a projectile-spawn packet to one client.
 */
static void send_projectile_spawn_pkt(int client_fd, uint32_t projectile_id,
                                       uint16_t ability_id, uint32_t owner_id,
                                       uint8_t owner_type,
                                       float px, float py,
                                       float dx, float dy, float speed) {
    ProjectileSpawnPacket pkt = {0};
    pkt.header.type         = PACKET_PROJECTILE_SPAWN;
    pkt.header.player_id    = htonl(owner_id);
    pkt.header.payload_size = htons(sizeof(ProjectileSpawnPacket) - sizeof(PacketHeader));
    pkt.projectile_id    = htonl(projectile_id);
    pkt.ability_id       = htons(ability_id);
    pkt.owner_id         = htonl(owner_id);
    pkt.owner_type       = owner_type;
    pkt.pos_x            = px;
    pkt.pos_y            = py;
    pkt.dir_x            = dx;
    pkt.dir_y            = dy;
    pkt.speed            = speed;
    server_send(client_fd, &pkt, sizeof(pkt));
}

static void send_projectile_destroy(int client_fd, uint32_t projectile_id,
                                     uint8_t reason) {
    ProjectileDestroyPacket pkt = {0};
    pkt.header.type         = PACKET_PROJECTILE_DESTROY;
    pkt.header.player_id    = 0;
    pkt.header.payload_size = htons(sizeof(ProjectileDestroyPacket) - sizeof(PacketHeader));
    pkt.projectile_id    = htonl(projectile_id);
    pkt.reason           = reason;
    server_send(client_fd, &pkt, sizeof(pkt));
}

/**
 * Send projectile damage or healing results to one client.
 */
static void send_ability_effect(int client_fd, uint32_t caster_id, uint32_t target_id,
                                uint16_t ability_id, int damage, int healing,
                                int target_new_hp, uint8_t is_kill) {
    AbilityEffectPacket pkt = {0};
    pkt.header.type         = PACKET_ABILITY_EFFECT;
    pkt.header.player_id    = htonl(caster_id);
    pkt.header.payload_size = htons(sizeof(AbilityEffectPacket) - sizeof(PacketHeader));
    pkt.caster_id           = htonl(caster_id);
    pkt.target_id           = htonl(target_id);
    pkt.ability_id          = htons(ability_id);
    pkt.damage              = htonl((uint32_t)damage);
    pkt.healing             = htonl((uint32_t)healing);
    pkt.target_new_health   = htonl((uint32_t)target_new_hp);
    pkt.is_kill             = is_kill;
    server_send(client_fd, &pkt, sizeof(pkt));
}

/**
 * Execute queued sends, rewards, loot rolls, and quest notifications.
 *
 * The caller must hold no projectile, player, or NPC-world locks.
 */
static void dq_flush(DeferredQueue* q) {
    for (int i = 0; i < q->count; i++) {
        DeferredSend* ds = &q->items[i];
        switch (ds->type) {
            case DSEND_PROJECTILE_DESTROY: {
                /* Recipients resolved here, not at queue time. Queueing one
                 * entry per nearby player made a single despawn in a crowd
                 * consume the whole queue; it is one entry now regardless of
                 * how many people can see it. */
                int fds[MAX_PLAYERS];
                int n = interest_collect_fds(ds->destroy.pos_x, ds->destroy.pos_y,
                                             PROJECTILE_VIEW_RANGE, fds, MAX_PLAYERS);
                for (int k = 0; k < n; k++)
                    send_projectile_destroy(fds[k], ds->destroy.projectile_id,
                                            ds->destroy.reason);
                break;
            }
            case DSEND_ABILITY_EFFECT:
                send_ability_effect(ds->client_fd,
                                    ds->effect.caster_id,
                                    ds->effect.target_id,
                                    ds->effect.ability_id,
                                    ds->effect.damage,
                                    ds->effect.healing,
                                    ds->effect.target_new_hp,
                                    ds->effect.is_kill);
                break;
            case DSEND_XP_AWARD: {
                if (ds->xp.xp > 0) {
                    party_award_xp(ds->xp.killer_id, ds->xp.xp);
                }
                if (ds->xp.xp > 0) {
                    ActivePlayer* killer = player_acquire(ds->xp.killer_id);
                    if (killer) {
                        player_send_kill_reward_locked(ds->xp.client_fd, killer,
                                                       (uint32_t)ds->xp.xp);
                        player_release(killer);
                    }
                }
                break;
            }
            case DSEND_LOOT_ROLL:
                loot_roll(ds->loot.npc_type_id, ds->loot.npc_x,
                          ds->loot.npc_y, ds->loot.killer_id);
                quest_on_npc_kill(ds->loot.killer_id, ds->client_fd, ds->loot.npc_type_id);
                break;
        }
    }
}

/**
 * Queue one projectile despawn for broadcast to whoever can see it.
 *
 * Takes no locks and needs none held: the recipients are worked out by
 * dq_flush(), after the projectile and NPC locks have been released. That is
 * also why the callers below no longer reach for the player registry while
 * holding g_projectiles_lock.
 */
static void queue_destroy_broadcast(DeferredQueue* q, float px, float py,
                                     uint32_t projectile_id, uint8_t reason) {
    DeferredSend ds = {0};
    ds.type = DSEND_PROJECTILE_DESTROY;
    ds.client_fd = -1;                  // unused; recipients resolved at flush
    ds.destroy.projectile_id = projectile_id;
    ds.destroy.reason = reason;
    ds.destroy.pos_x = px;
    ds.destroy.pos_y = py;
    dq_push(q, &ds);
}

/**
 * Calculate projectile damage after scaling, bonuses, variance, and mitigation.
 *
 * The caster's contribution was snapshotted at launch; only the target's mitigation
 * is read now, at impact.
 *
 * @param target_mods  The target's collected modifiers, or NULL for an unmodified target.
 * @return             Final damage, never below one.
 */
static int calc_projectile_damage(const Projectile* proj,
                                   int target_health, int target_max_health,
                                   const DamageModifiers* target_mods) {
    float mult = combat_ability_damage_mult(proj->damage_stat, proj->caster_stats);
    int damage = (int)((float)proj->damage * mult);

    // Bonus damage condition (e.g. execute)
    if (proj->bonus_damage.condition == 1 && target_max_health > 0) {
        float hp_pct = (float)target_health / (float)target_max_health * 100.0f;
        if (hp_pct <= proj->bonus_damage.threshold) {
            damage = (int)((float)damage * proj->bonus_damage.multiplier);
        }
    }

    if (combat_check_crit(proj->caster_stats[STAT_PRECISION])) {
        damage = (int)((float)damage * combat_crit_multiplier(proj->caster_stats[STAT_FEROCITY]));
    }

    // Random variance (+-10%)
    int variance = (rand() % 21) - 10;
    damage += (damage * variance) / 100;

    return damage_resolve(damage, &proj->caster_mods, target_mods);
}

/**
 * Apply one projectile-borne effect to an NPC.
 *
 * The caller must hold the NPC world's lock.
 */
static void apply_effect_to_npc(NPCEntity* npc, const AbilityEffectDef* effect,
                                uint32_t source_id) {
    if (effect->type == EFFECT_TAUNT) {
        npc->taunt_source_id  = source_id;
        npc->taunt_expires_at = get_monotonic_time() + effect->duration;
        npc->ai_target_id     = source_id;
        LOG_DEBUG("[PROJ] NPC %u taunted by %u for %.1fs", npc->id, source_id, effect->duration);
        return;
    }

    LOG_DEBUG("[PROJ] Applied effect %d to NPC %u (val=%d, dur=%.1fs)", effect->type, npc->id, effect->value, effect->duration);
}

/**
 * Initialize the projectile pool and identifier sequence.
 */
int projectile_init(int capacity) {
    if (capacity <= 0)                        capacity = PROJECTILE_CAPACITY_DEFAULT;
    if (capacity > PROJECTILE_CAPACITY_MAX)   capacity = PROJECTILE_CAPACITY_MAX;

    free(g_projectiles);
    g_projectiles = calloc((size_t)capacity, sizeof(*g_projectiles));
    if (!g_projectiles) {
        g_projectile_capacity = 0;
        LOG_ERROR("[PROJECTILE] could not allocate a pool of %d projectiles", capacity);
        return 0;
    }

    g_projectile_capacity = capacity;
    g_next_projectile_id = 1;
    LOG_INFO("[PROJECTILE] pool initialized: %d slots (%zu KB)",
             capacity, ((size_t)capacity * sizeof(*g_projectiles)) / 1024);
    return 1;
}

int projectile_capacity(void) { return g_projectile_capacity; }

/**
 * Clear the projectile pool while holding its state lock.
 */
void projectile_cleanup(void) {
    pthread_mutex_lock(&g_projectiles_lock);
    free(g_projectiles);
    g_projectiles = NULL;
    g_projectile_capacity = 0;
    pthread_mutex_unlock(&g_projectiles_lock);
    LOG_DEBUG("[PROJECTILE] System cleaned up");
}

/**
 * Spawn a projectile and notify nearby players.
 *
 * @param info  Complete immutable projectile creation parameters.
 * @return      The assigned projectile identifier, or 0 when the pool is full.
 */
uint32_t projectile_spawn(const ProjectileSpawnInfo* info) {
    // Calculate direction from origin to aim
    float dx = info->aim_x - info->origin_x;
    float dy = info->aim_y - info->origin_y;
    float len = sqrtf(dx * dx + dy * dy);
    float dir_x = (len > 0.001f) ? dx / len : 1.0f;
    float dir_y = (len > 0.001f) ? dy / len : 0.0f;

    // Snapshot data for spawn broadcast (before lock)
    uint32_t id;
    float spawn_x, spawn_y;

    pthread_mutex_lock(&g_projectiles_lock);

    int slot = -1;
    for (int i = 0; i < g_projectile_capacity; i++) {
        if (!g_projectiles[i].is_active) { slot = i; break; }
    }

    if (slot == -1) {
        pthread_mutex_unlock(&g_projectiles_lock);
        LOG_WARN_RL(5, 60, "[PROJ] No free projectile slots");
        return 0;
    }

    Projectile* proj = &g_projectiles[slot];
    memset(proj, 0, sizeof(Projectile));

    proj->is_active       = 1;
    proj->projectile_id   = g_next_projectile_id++;
    proj->owner_type      = info->owner_type;
    proj->owner_id        = info->owner_id;
    proj->ability_id      = info->ability_id;
    proj->owner_fd        = info->owner_fd;

    proj->pos_x           = info->origin_x;
    proj->pos_y           = info->origin_y;
    proj->dir_x           = dir_x;
    proj->dir_y           = dir_y;
    proj->speed           = info->speed;
    proj->width           = info->width;
    proj->max_range       = info->max_range;
    proj->distance_traveled = 0.0f;

    proj->damage          = info->damage;
    proj->damage_type     = info->damage_type;
    proj->bonus_damage    = info->bonus_damage;

    memcpy(proj->caster_stats, info->caster_stats, sizeof(proj->caster_stats));
    proj->damage_stat = info->damage_stat;
    proj->caster_mods = info->caster_mods;

    proj->effect_count = info->effect_count;
    for (int i = 0; i < info->effect_count && i < MAX_ABILITY_EFFECTS; i++) {
        proj->effects[i] = info->effects[i];
    }

    id = proj->projectile_id;
    spawn_x = proj->pos_x;
    spawn_y = proj->pos_y;

    pthread_mutex_unlock(&g_projectiles_lock);

    // Broadcast spawn to nearby players (outside projectile lock)
    int spawn_targets[MAX_PLAYERS];
    int spawn_count = interest_collect_fds(spawn_x, spawn_y, PROJECTILE_VIEW_RANGE,
                                           spawn_targets, MAX_PLAYERS);

    // Send outside all locks
    for (int i = 0; i < spawn_count; i++) {
        send_projectile_spawn_pkt(spawn_targets[i], id,
                                   info->ability_id, info->owner_id,
                                   info->owner_type,
                                   spawn_x, spawn_y, dir_x, dir_y,
                                   info->speed);
    }

    LOG_DEBUG("[PROJ] Spawned projectile %u (ability=%u, owner=%s %u) at (%.1f, %.1f) dir=(%.2f, %.2f)", id, info->ability_id, info->owner_type == PROJECTILE_OWNER_PLAYER ? "player" : "npc", info->owner_id, info->origin_x, info->origin_y, dir_x, dir_y);

    return id;
}

/**
 * Remove a projectile and notify nearby players.
 */
void projectile_remove(uint32_t projectile_id) {
    float px = 0, py = 0;
    int found = 0;

    pthread_mutex_lock(&g_projectiles_lock);
    for (int i = 0; i < g_projectile_capacity; i++) {
        if (g_projectiles[i].is_active &&
            g_projectiles[i].projectile_id == projectile_id) {
            px = g_projectiles[i].pos_x;
            py = g_projectiles[i].pos_y;
            g_projectiles[i].is_active = 0;
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&g_projectiles_lock);

    if (!found) return;

    // Notify nearby players (outside projectile lock)
    DeferredQueue q;
    dq_init(&q);
    queue_destroy_broadcast(&q, px, py, projectile_id, PROJECTILE_DESTROY_CANCELLED);
    dq_flush(&q);
}

/**
 * Advance projectiles and resolve range expiry and entity collisions.
 *
 * Network sends and reward processing are deferred until state locks are released.
 *
 * @param world       NPC world used for player-owned projectile collisions.
 * @param snap        Player snapshot used to select NPC-projectile candidates; may be NULL.
 * @param delta_time  Elapsed tick time in seconds.
 */
void projectile_tick(NPCWorld* world, TickSnapshot* snap, NPCTickSnapshot* npcs,
                     double delta_time) {
    float dt = (float)delta_time;
    DeferredQueue q;
    dq_init(&q);

    pthread_mutex_lock(&g_projectiles_lock);

    for (int p = 0; p < g_projectile_capacity; p++) {
        if (!g_projectiles[p].is_active) continue;

        Projectile* proj = &g_projectiles[p];

        // Move
        float move_dist = proj->speed * dt;
        proj->pos_x += proj->dir_x * move_dist;
        proj->pos_y += proj->dir_y * move_dist;
        proj->distance_traveled += move_dist;

        float proj_px = proj->pos_x;
        float proj_py = proj->pos_y;
        uint32_t proj_id = proj->projectile_id;

        // Check max range
        if (proj->distance_traveled >= proj->max_range) {
            proj->is_active = 0;
            // Queue destroy broadcast
            queue_destroy_broadcast(&q, proj_px, proj_py, proj_id,
                                     PROJECTILE_DESTROY_EXPIRED);
            continue;
        }

        // --- Player projectile: collide with NPCs ---
        if (proj->owner_type == PROJECTILE_OWNER_PLAYER) {
            /* This was the worst offender in the old design: a full scan of the
             * NPC pool per projectile, re-taking the one global NPC lock each
             * time. At 128 projectiles and 256 NPCs that was 32,768 distance
             * checks and 128 lock round-trips every tick, and it grew with the
             * pool. Now each projectile asks the tick grid for the handful of
             * NPCs near its own position. */
            const float proj_half = proj->width / 2.0f;
            int nearby_npcs[PROJECTILE_HIT_CANDIDATES];
            int npc_near = npcs
                ? npc_snapshot_query(npcs, proj_px, proj_py,
                                     proj_half + npcs->max_hitbox_radius,
                                     nearby_npcs, PROJECTILE_HIT_CANDIDATES)
                : 0;

            for (int k = 0; k < npc_near; k++) {
                int c = nearby_npcs[k];

                float d = dist2d(proj_px, proj_py, npcs->pos_x[c], npcs->pos_y[c]);
                float hit_range = proj_half + npcs->hitbox_radius[c];
                if (d > hit_range) continue;

                NPCEntity* npc = npc_world_acquire_slot(world, npcs->slot[c], npcs->id[c]);
                if (!npc) continue;               // died or was recycled this tick
                if (!npc->is_alive) { npc_world_release(world, npc); continue; }

                {
                    int owner_fd = proj->owner_fd;

                    /* There is no dodge roll: a projectile that reaches its target
                     * connects, and mitigation is armor and percentage reducers only. */
                    DamageModifiers target_mods;
                    damage_mods_reset(&target_mods);
                    damage_mods_add_armor(&target_mods, npc->armor);

                    int damage = calc_projectile_damage(proj,
                                                         npc->health, npc->max_health,
                                                         &target_mods);

                    /* Shields, facings and weak points, consulted here as at
                     * every other path that writes NPC health. Measured from the
                     * projectile's position, which is where the hit came from --
                     * not from the caster, who may be behind the NPC by now. */
                    damage = npc_mitigation_apply(world, npcs->slot[c], npc, damage,
                                                  proj->pos_x, proj->pos_y,
                                                  get_monotonic_time(), NULL);

                    npc->health -= damage;
                    if (npc->health < 0) npc->health = 0;
                    npc_trigger_note_damage(npc, proj->pos_x, proj->pos_y);

                    uint8_t is_kill = (npc->health == 0) ? 1 : 0;
                    if (is_kill) {
                        npc->is_alive = 0;
                        npc->death_time = get_monotonic_time();
                    }

                    if (owner_fd >= 0) {
                        DeferredSend ds = {0};
                        ds.type = DSEND_ABILITY_EFFECT;
                        ds.client_fd = owner_fd;
                        ds.effect.caster_id = proj->owner_id;
                        ds.effect.target_id = npc->id;
                        ds.effect.ability_id = proj->ability_id;
                        ds.effect.damage = damage;
                        ds.effect.target_new_hp = npc->health;
                        ds.effect.is_kill = is_kill;
                        dq_push(&q, &ds);
                    }

                    for (int e = 0; e < proj->effect_count; e++) {
                        apply_effect_to_npc(npc, &proj->effects[e], proj->owner_id);
                    }

                    if (is_kill) {
                        if (npc->xp_reward > 0) {
                            DeferredSend ds = {0};
                            ds.type = DSEND_XP_AWARD;
                            ds.xp.killer_id = proj->owner_id;
                            ds.xp.xp = npc->xp_reward;
                            ds.xp.client_fd = owner_fd;
                            dq_push(&q, &ds);
                        }
                        {
                            DeferredSend ds = {0};
                            ds.type = DSEND_LOOT_ROLL;
                            ds.client_fd = owner_fd;  // used for quest_on_npc_kill
                            ds.loot.npc_type_id = npc->npc_type_id;
                            ds.loot.npc_x = npc->pos_x;
                            ds.loot.npc_y = npc->pos_y;
                            ds.loot.killer_id = proj->owner_id;
                            dq_push(&q, &ds);
                        }
                    }

                    proj->is_active = 0;
                }

                npc_world_release(world, npc);

                // Queue destroy broadcast outside the NPC locks
                queue_destroy_broadcast(&q, proj_px, proj_py, proj_id,
                                         PROJECTILE_DESTROY_HIT);
                break;
            }

        // --- NPC projectile: collide with players ---
        } else if (proj->owner_type == PROJECTILE_OWNER_NPC) {
            float hit_range = proj->width / 2.0f + 20.0f; // 20 = player hitbox

            // select bounded collision candidates from the snapshot grid
            int nearby[PROJECTILE_HIT_CANDIDATES];
            int n_near = snap ? tick_snapshot_query(snap, proj_px, proj_py, hit_range,
                                                    nearby, PROJECTILE_HIT_CANDIDATES)
                              : 0;

            player_registry_rdlock();
            for (int k = 0; k < n_near; k++) {
                int i = snap->slot[nearby[k]];
                if (!active_players[i].is_loaded) continue;
                // reject slots recycled since the snapshot
                if (active_players[i].character_id != snap->character_id[nearby[k]]) continue;

                // confirm collisions against live positions
                float d = dist2d(proj_px, proj_py,
                                 active_players[i].pos_x, active_players[i].pos_y);

                if (d <= hit_range) {
                    pthread_mutex_lock(&active_players[i].lock);

                    int hit_fd = active_players[i].client_fd;
                    uint32_t hit_char_id = active_players[i].character_id;

                    /* NPC projectile damage is flat, with no stat scaling. The
                     * player's own mitigation — armor, the race passive if in Animal
                     * Form, and any active reducers — is collected here. */
                    int damage = proj->damage;
                    int variance = (rand() % 21) - 10;
                    damage += (damage * variance) / 100;

                    DamageModifiers target_mods;
                    player_collect_modifiers(&active_players[i], 0, &target_mods);
                    damage = damage_resolve(damage, NULL, &target_mods);

                    active_players[i].health -= damage;
                    if (active_players[i].health < 0) active_players[i].health = 0;
                    active_players[i].last_combat_time = get_monotonic_time();
                    player_add_rage(&active_players[i], 0, damage);

                    int new_hp = active_players[i].health;
                    uint8_t is_kill = (new_hp == 0) ? 1 : 0;

                    pthread_mutex_unlock(&active_players[i].lock);

                    // Queue effect send
                    DeferredSend ds = {0};
                    ds.type = DSEND_ABILITY_EFFECT;
                    ds.client_fd = hit_fd;
                    ds.effect.caster_id = proj->owner_id;
                    ds.effect.target_id = hit_char_id;
                    ds.effect.ability_id = proj->ability_id;
                    ds.effect.damage = damage;
                    ds.effect.target_new_hp = new_hp;
                    ds.effect.is_kill = is_kill;
                    dq_push(&q, &ds);

                    // Queue destroy broadcast
                    queue_destroy_broadcast(&q, proj_px, proj_py, proj_id,
                                             PROJECTILE_DESTROY_HIT);

                    proj->is_active = 0;
                    break;
                }
            }
            player_registry_unlock();
        }
    }
    pthread_mutex_unlock(&g_projectiles_lock);

    // Flush all deferred sends OUTSIDE all locks
    dq_flush(&q);
}

/**
 * Broadcast active projectile positions to nearby snapshot players.
 *
 * Samples projectile state under its lock and performs sends after releasing it.
 *
 * @param snapshot  Broadcast-pass player snapshot; may be NULL or empty.
 */
void projectile_broadcast(const BroadcastSnapshot* snapshot, int shard, int shard_count) {
    // share the broadcast pass snapshot across entity streams
    if (!snapshot || snapshot->count == 0) return;

    const BroadcastPlayer* snapshots = snapshot->players;
    const int snapshot_count = snapshot->count;

    /* Snapshot projectile positions.
     *
     * Retained per thread for the same reason the deferred queue is: every shard
     * broadcasts concurrently, so one shared buffer would race. */
    if (g_projectile_capacity > t_snap_capacity) {
        ProjSnapshot* grown = realloc(t_proj_snaps,
                                      (size_t)g_projectile_capacity * sizeof(*grown));
        if (!grown) {
            LOG_ERROR("[PROJECTILE] could not size the broadcast snapshot to %d",
                      g_projectile_capacity);
            return;
        }
        t_proj_snaps    = grown;
        t_snap_capacity = g_projectile_capacity;
    }
    ProjSnapshot* proj_snaps = t_proj_snaps;
    int proj_count = 0;

    pthread_mutex_lock(&g_projectiles_lock);
    for (int i = 0; i < g_projectile_capacity; i++) {
        if (!g_projectiles[i].is_active) continue;
        proj_snaps[proj_count].id = g_projectiles[i].projectile_id;
        proj_snaps[proj_count].px = g_projectiles[i].pos_x;
        proj_snaps[proj_count].py = g_projectiles[i].pos_y;
        proj_count++;
    }
    pthread_mutex_unlock(&g_projectiles_lock);

    if (proj_count == 0) return;

    // Build and send packets OUTSIDE all locks.
    //
    // Sharded on the descriptor, the same way connections are pinned to an
    // epoll loop, so this shard only ever writes to sockets its matching loop
    // owns. The snapshot is immutable here, so shards share it freely.
    for (int s = 0; s < snapshot_count; s++) {
        if (shard_count > 1 && (snapshots[s].client_fd % shard_count) != shard) continue;

        ProjectileUpdatePacket pkt = {0};
        pkt.header.type      = PACKET_PROJECTILE_UPDATE;
        pkt.header.player_id = 0;
        pkt.count = 0;

        /* Fill nearest-first, so the wire cap behaves like a view radius rather
         * than like a slot-order accident.
         *
         * This used to walk the pool in slot order and break at the cap, so a
         * player standing in a firefight denser than MAX_PROJECTILES_PER_PACKET
         * saw an arbitrary subset chosen by pool index -- and because slots
         * recycle as projectiles expire and spawn, the subset changed every
         * tick, making projectiles flicker in and out at random rather than
         * simply thinning out at distance.
         *
         * The window is the packet's own capacity, so the insertion sort below
         * is bounded by the wire cap and not by the pool.
         */
        int   near_idx[MAX_PROJECTILES_PER_PACKET];
        float near_dist[MAX_PROJECTILES_PER_PACKET];
        int   near_count = 0;

        for (int p = 0; p < proj_count; p++) {
            float d = dist2d(proj_snaps[p].px, proj_snaps[p].py,
                             snapshots[s].pos_x, snapshots[s].pos_y);
            if (d > PROJECTILE_VIEW_RANGE) continue;

            if (near_count == MAX_PROJECTILES_PER_PACKET &&
                d >= near_dist[near_count - 1]) {
                continue;   // further than everything already kept
            }

            int at = (near_count < MAX_PROJECTILES_PER_PACKET) ? near_count++
                                                               : near_count - 1;
            while (at > 0 && near_dist[at - 1] > d) {
                near_dist[at] = near_dist[at - 1];
                near_idx[at]  = near_idx[at - 1];
                at--;
            }
            near_dist[at] = d;
            near_idx[at]  = p;
        }

        for (int k = 0; k < near_count; k++) {
            const int p = near_idx[k];
            pkt.projectiles[pkt.count].projectile_id = htonl(proj_snaps[p].id);
            pkt.projectiles[pkt.count].pos_x = proj_snaps[p].px;
            pkt.projectiles[pkt.count].pos_y = proj_snaps[p].py;
            pkt.count++;
        }

        if (pkt.count > 0) {
            size_t send_size = sizeof(PacketHeader) + 4 +
                               pkt.count * sizeof(ProjectilePositionData);
            pkt.header.payload_size = htons((uint16_t)(send_size - sizeof(PacketHeader)));
            server_send(snapshots[s].client_fd, &pkt, send_size);
        }
    }
}
