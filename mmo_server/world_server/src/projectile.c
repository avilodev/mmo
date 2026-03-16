// ============================================================================
// projectile.c — Modular projectile system
//
// Handles spawning, movement, collision, damage, and broadcasting for all
// projectiles (player skillshots and NPC ranged attacks).
//
// Threading: All state mutations happen under g_projectiles_lock.
// All send() calls happen OUTSIDE locks to prevent blocking.
// ============================================================================

#include "projectile.h"
#include "combat_stats.h"
#include "player_data.h"
#include "player_level.h"
#include "party.h"
#include "loot.h"

#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <time.h>
#include "utils.h"

// ---------------------------------------------------------------------------
// Extern references
// ---------------------------------------------------------------------------

extern ActivePlayer active_players[];
extern pthread_mutex_t active_players_lock;

// ---------------------------------------------------------------------------
// Static state
// ---------------------------------------------------------------------------

static Projectile      g_projectiles[MAX_PROJECTILES];
static pthread_mutex_t g_projectiles_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t        g_next_projectile_id = 1;

// ---------------------------------------------------------------------------
// Deferred send queue — filled under lock, flushed after unlock
// ---------------------------------------------------------------------------

#define MAX_DEFERRED_SENDS 256

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
            uint32_t gold;
            int      client_fd;
        } xp;
        struct {
            uint16_t npc_type_id;
            float    npc_x, npc_y;
            uint32_t killer_id;
        } loot;
    };
} DeferredSend;

typedef struct {
    DeferredSend items[MAX_DEFERRED_SENDS];
    int count;
} DeferredQueue;

static void dq_init(DeferredQueue* q) { q->count = 0; }

static void dq_push(DeferredQueue* q, const DeferredSend* item) {
    if (q->count < MAX_DEFERRED_SENDS) {
        q->items[q->count++] = *item;
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Send helpers (called OUTSIDE locks)
// ---------------------------------------------------------------------------

static void send_projectile_spawn_pkt(int client_fd, uint32_t projectile_id,
                                       uint16_t ability_id, uint32_t owner_id,
                                       uint8_t owner_type,
                                       float px, float py,
                                       float dx, float dy, float speed) {
    ProjectileSpawnPacket pkt = {0};
    pkt.header.type      = PACKET_PROJECTILE_SPAWN;
    pkt.header.player_id = htonl(owner_id);
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
    pkt.header.type      = PACKET_PROJECTILE_DESTROY;
    pkt.header.player_id = 0;
    pkt.projectile_id    = htonl(projectile_id);
    pkt.reason           = reason;
    server_send(client_fd, &pkt, sizeof(pkt));
}

static void send_ability_effect(int client_fd, uint32_t caster_id, uint32_t target_id,
                                uint16_t ability_id, int damage, int healing,
                                int target_new_hp, uint8_t is_kill) {
    AbilityEffectPacket pkt = {0};
    pkt.header.type         = PACKET_ABILITY_EFFECT;
    pkt.header.player_id    = htonl(caster_id);
    pkt.caster_id           = htonl(caster_id);
    pkt.target_id           = htonl(target_id);
    pkt.ability_id          = htons(ability_id);
    pkt.damage              = htonl((uint32_t)damage);
    pkt.healing             = htonl((uint32_t)healing);
    pkt.target_new_health   = htonl((uint32_t)target_new_hp);
    pkt.is_kill             = is_kill;
    server_send(client_fd, &pkt, sizeof(pkt));
}

// ---------------------------------------------------------------------------
// Flush deferred queue (called OUTSIDE all locks)
// ---------------------------------------------------------------------------

static void dq_flush(DeferredQueue* q) {
    for (int i = 0; i < q->count; i++) {
        DeferredSend* ds = &q->items[i];
        switch (ds->type) {
            case DSEND_PROJECTILE_DESTROY:
                send_projectile_destroy(ds->client_fd,
                                        ds->destroy.projectile_id,
                                        ds->destroy.reason);
                break;
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
                if (ds->xp.gold > 0 || ds->xp.xp > 0) {
                    ActivePlayer* killer = player_acquire(ds->xp.killer_id);
                    if (killer) {
                        if (ds->xp.gold > 0)
                            player_award_gold_locked(killer, ds->xp.gold);
                        player_send_kill_reward_locked(ds->xp.client_fd, killer,
                            (uint32_t)ds->xp.xp, ds->xp.gold);
                        player_release(killer);
                    }
                }
                break;
            }
            case DSEND_LOOT_ROLL:
                loot_roll(ds->loot.npc_type_id, ds->loot.npc_x,
                          ds->loot.npc_y, ds->loot.killer_id);
                break;
        }
    }
}

// ---------------------------------------------------------------------------
// Helper: queue destroy broadcast to nearby players
// Caller must hold active_players_lock.
// ---------------------------------------------------------------------------

static void queue_destroy_broadcast(DeferredQueue* q, float px, float py,
                                     uint32_t projectile_id, uint8_t reason) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!active_players[i].is_loaded) continue;
        float d = dist2d(px, py, active_players[i].pos_x, active_players[i].pos_y);
        if (d <= PROJECTILE_VIEW_RANGE) {
            DeferredSend ds = {0};
            ds.type = DSEND_PROJECTILE_DESTROY;
            ds.client_fd = active_players[i].client_fd;
            ds.destroy.projectile_id = projectile_id;
            ds.destroy.reason = reason;
            dq_push(q, &ds);
        }
    }
}

// ---------------------------------------------------------------------------
// Damage calculation (same formula as ability_handler)
// ---------------------------------------------------------------------------

static int calc_projectile_damage(const Projectile* proj,
                                   int target_health, int target_max_health,
                                   int target_defense) {
    float mult = combat_ability_damage_mult(proj->damage_stat,
                                             proj->caster_strength,
                                             proj->caster_agility,
                                             proj->caster_intelligence,
                                             proj->caster_wisdom);
    int damage = (int)((float)proj->damage * mult);

    // Bonus damage condition (e.g. execute)
    if (proj->bonus_damage.condition == 1 && target_max_health > 0) {
        float hp_pct = (float)target_health / (float)target_max_health * 100.0f;
        if (hp_pct <= proj->bonus_damage.threshold) {
            damage = (int)((float)damage * proj->bonus_damage.multiplier);
        }
    }

    // Random variance (+-10%)
    int variance = (rand() % 21) - 10;
    damage += (damage * variance) / 100;

    // Defense reduction
    damage = combat_apply_defense(damage, target_defense);

    if (damage < 1) damage = 1;
    return damage;
}

// ---------------------------------------------------------------------------
// Apply effects to NPC on hit
// ---------------------------------------------------------------------------

static void apply_effect_to_npc(NPCEntity* npc, const AbilityEffectDef* effect,
                                uint32_t source_id) {
    (void)source_id;
    printf("[PROJ] Applied effect %d to NPC %u (val=%d, dur=%.1fs)\n",
           effect->type, npc->id, effect->value, effect->duration);
}

// ============================================================================
// INIT / CLEANUP
// ============================================================================

void projectile_init(void) {
    memset(g_projectiles, 0, sizeof(g_projectiles));
    g_next_projectile_id = 1;
    printf("[PROJECTILE] System initialized\n");
}

void projectile_cleanup(void) {
    pthread_mutex_lock(&g_projectiles_lock);
    memset(g_projectiles, 0, sizeof(g_projectiles));
    pthread_mutex_unlock(&g_projectiles_lock);
    printf("[PROJECTILE] System cleaned up\n");
}

// ============================================================================
// SPAWN
// ============================================================================

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
    for (int i = 0; i < MAX_PROJECTILES; i++) {
        if (!g_projectiles[i].is_active) { slot = i; break; }
    }

    if (slot == -1) {
        pthread_mutex_unlock(&g_projectiles_lock);
        printf("[PROJ] No free projectile slots\n");
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

    proj->caster_strength     = info->caster_strength;
    proj->caster_agility      = info->caster_agility;
    proj->caster_intelligence = info->caster_intelligence;
    proj->caster_wisdom       = info->caster_wisdom;
    proj->damage_stat         = info->damage_stat;

    proj->effect_count = info->effect_count;
    for (int i = 0; i < info->effect_count && i < MAX_ABILITY_EFFECTS; i++) {
        proj->effects[i] = info->effects[i];
    }

    id = proj->projectile_id;
    spawn_x = proj->pos_x;
    spawn_y = proj->pos_y;

    pthread_mutex_unlock(&g_projectiles_lock);

    // Broadcast spawn to nearby players (outside projectile lock)
    pthread_mutex_lock(&active_players_lock);
    typedef struct { int fd; } SpawnTarget;
    SpawnTarget spawn_targets[MAX_PLAYERS];
    int spawn_count = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!active_players[i].is_loaded) continue;
        float d = dist2d(spawn_x, spawn_y,
                         active_players[i].pos_x, active_players[i].pos_y);
        if (d <= PROJECTILE_VIEW_RANGE) {
            spawn_targets[spawn_count++].fd = active_players[i].client_fd;
        }
    }
    pthread_mutex_unlock(&active_players_lock);

    // Send outside all locks
    for (int i = 0; i < spawn_count; i++) {
        send_projectile_spawn_pkt(spawn_targets[i].fd, id,
                                   info->ability_id, info->owner_id,
                                   info->owner_type,
                                   spawn_x, spawn_y, dir_x, dir_y,
                                   info->speed);
    }

    printf("[PROJ] Spawned projectile %u (ability=%u, owner=%s %u) at (%.1f, %.1f) dir=(%.2f, %.2f)\n",
           id, info->ability_id,
           info->owner_type == PROJECTILE_OWNER_PLAYER ? "player" : "npc",
           info->owner_id, info->origin_x, info->origin_y, dir_x, dir_y);

    return id;
}

// ============================================================================
// REMOVE
// ============================================================================

void projectile_remove(uint32_t projectile_id) {
    float px = 0, py = 0;
    int found = 0;

    pthread_mutex_lock(&g_projectiles_lock);
    for (int i = 0; i < MAX_PROJECTILES; i++) {
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
    pthread_mutex_lock(&active_players_lock);
    queue_destroy_broadcast(&q, px, py, projectile_id, PROJECTILE_DESTROY_CANCELLED);
    pthread_mutex_unlock(&active_players_lock);
    dq_flush(&q);
}

// ============================================================================
// TICK — movement, collision, damage
//
// Pattern: snapshot state under lock, release, then send.
// ============================================================================

void projectile_tick(NPCWorld* world, double delta_time) {
    float dt = (float)delta_time;
    DeferredQueue q;
    dq_init(&q);

    pthread_mutex_lock(&g_projectiles_lock);

    for (int p = 0; p < MAX_PROJECTILES; p++) {
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
            pthread_mutex_lock(&active_players_lock);
            queue_destroy_broadcast(&q, proj_px, proj_py, proj_id,
                                     PROJECTILE_DESTROY_EXPIRED);
            pthread_mutex_unlock(&active_players_lock);
            continue;
        }

        // --- Player projectile: collide with NPCs ---
        if (proj->owner_type == PROJECTILE_OWNER_PLAYER) {
            pthread_mutex_lock(&world->lock);
            for (int n = 0; n < MAX_NPCS; n++) {
                NPCEntity* npc = &world->npcs[n];
                if (!npc->is_alive || npc->id == 0) continue;

                float d = dist2d(proj_px, proj_py, npc->pos_x, npc->pos_y);
                float hit_range = (proj->width / 2.0f) + npc->hitbox_radius;

                if (d <= hit_range) {
                    int owner_fd = proj->owner_fd;

                    // Evasion check
                    if (combat_check_evasion(npc->evasion)) {
                        if (owner_fd >= 0) {
                            DeferredSend ds = {0};
                            ds.type = DSEND_ABILITY_EFFECT;
                            ds.client_fd = owner_fd;
                            ds.effect.caster_id = proj->owner_id;
                            ds.effect.target_id = npc->id;
                            ds.effect.ability_id = proj->ability_id;
                            ds.effect.damage = 0;
                            ds.effect.target_new_hp = npc->health;
                            ds.effect.is_kill = 0;
                            dq_push(&q, &ds);
                        }
                        proj->is_active = 0;
                        // Queue destroy broadcast
                        pthread_mutex_lock(&active_players_lock);
                        queue_destroy_broadcast(&q, proj_px, proj_py, proj_id,
                                                 PROJECTILE_DESTROY_HIT);
                        pthread_mutex_unlock(&active_players_lock);
                        break;
                    }

                    int damage = calc_projectile_damage(proj,
                                                         npc->health, npc->max_health,
                                                         npc->defense);
                    npc->health -= damage;
                    if (npc->health < 0) npc->health = 0;

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
                        if (npc->xp_reward > 0 || npc->gold_reward > 0) {
                            DeferredSend ds = {0};
                            ds.type = DSEND_XP_AWARD;
                            ds.xp.killer_id = proj->owner_id;
                            ds.xp.xp = npc->xp_reward;
                            ds.xp.gold = npc->gold_reward;
                            ds.xp.client_fd = owner_fd;
                            dq_push(&q, &ds);
                        }
                        {
                            DeferredSend ds = {0};
                            ds.type = DSEND_LOOT_ROLL;
                            ds.loot.npc_type_id = npc->npc_type_id;
                            ds.loot.npc_x = npc->pos_x;
                            ds.loot.npc_y = npc->pos_y;
                            ds.loot.killer_id = proj->owner_id;
                            dq_push(&q, &ds);
                        }
                    }

                    proj->is_active = 0;
                    // Queue destroy broadcast
                    pthread_mutex_lock(&active_players_lock);
                    queue_destroy_broadcast(&q, proj_px, proj_py, proj_id,
                                             PROJECTILE_DESTROY_HIT);
                    pthread_mutex_unlock(&active_players_lock);
                    break;
                }
            }
            pthread_mutex_unlock(&world->lock);

        // --- NPC projectile: collide with players ---
        } else if (proj->owner_type == PROJECTILE_OWNER_NPC) {
            pthread_mutex_lock(&active_players_lock);
            for (int i = 0; i < MAX_PLAYERS; i++) {
                if (!active_players[i].is_loaded) continue;

                float d = dist2d(proj_px, proj_py,
                                 active_players[i].pos_x, active_players[i].pos_y);
                float hit_range = proj->width / 2.0f + 20.0f; // 20 = player hitbox

                if (d <= hit_range) {
                    pthread_mutex_lock(&active_players[i].lock);

                    int hit_fd = active_players[i].client_fd;
                    uint32_t hit_char_id = active_players[i].character_id;

                    // Evasion check
                    if (combat_check_evasion(active_players[i].evasion)) {
                        int hp = active_players[i].health;
                        pthread_mutex_unlock(&active_players[i].lock);

                        DeferredSend ds = {0};
                        ds.type = DSEND_ABILITY_EFFECT;
                        ds.client_fd = hit_fd;
                        ds.effect.caster_id = proj->owner_id;
                        ds.effect.target_id = hit_char_id;
                        ds.effect.ability_id = proj->ability_id;
                        ds.effect.damage = 0;
                        ds.effect.target_new_hp = hp;
                        ds.effect.is_kill = 0;
                        dq_push(&q, &ds);

                        queue_destroy_broadcast(&q, proj_px, proj_py, proj_id,
                                                 PROJECTILE_DESTROY_HIT);
                        proj->is_active = 0;
                        break;
                    }

                    // NPC projectile damage (flat, no stat scaling)
                    int damage = proj->damage;
                    int variance = (rand() % 21) - 10;
                    damage += (damage * variance) / 100;
                    damage = combat_apply_defense(damage, active_players[i].defense);
                    if (damage < 1) damage = 1;

                    active_players[i].health -= damage;
                    if (active_players[i].health < 0) active_players[i].health = 0;
                    active_players[i].last_combat_time = get_monotonic_time();

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
            pthread_mutex_unlock(&active_players_lock);
        }
    }
    pthread_mutex_unlock(&g_projectiles_lock);

    // Flush all deferred sends OUTSIDE all locks
    dq_flush(&q);
}

// ============================================================================
// BROADCAST — send positions to nearby players at 30Hz
// ============================================================================

void projectile_broadcast(void) {
    // Snapshot active player positions and fds
    typedef struct {
        int client_fd;
        float pos_x, pos_y;
    } PlayerSnapshot;

    PlayerSnapshot snapshots[MAX_PLAYERS];
    int snapshot_count = 0;

    pthread_mutex_lock(&active_players_lock);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!active_players[i].is_loaded) continue;
        snapshots[snapshot_count].client_fd = active_players[i].client_fd;
        snapshots[snapshot_count].pos_x     = active_players[i].pos_x;
        snapshots[snapshot_count].pos_y     = active_players[i].pos_y;
        snapshot_count++;
    }
    pthread_mutex_unlock(&active_players_lock);

    if (snapshot_count == 0) return;

    // Snapshot projectile positions
    typedef struct {
        uint32_t id;
        float px, py;
    } ProjSnapshot;

    ProjSnapshot proj_snaps[MAX_PROJECTILES];
    int proj_count = 0;

    pthread_mutex_lock(&g_projectiles_lock);
    for (int i = 0; i < MAX_PROJECTILES; i++) {
        if (!g_projectiles[i].is_active) continue;
        proj_snaps[proj_count].id = g_projectiles[i].projectile_id;
        proj_snaps[proj_count].px = g_projectiles[i].pos_x;
        proj_snaps[proj_count].py = g_projectiles[i].pos_y;
        proj_count++;
    }
    pthread_mutex_unlock(&g_projectiles_lock);

    if (proj_count == 0) return;

    // Build and send packets OUTSIDE all locks
    for (int s = 0; s < snapshot_count; s++) {
        ProjectileUpdatePacket pkt = {0};
        pkt.header.type      = PACKET_PROJECTILE_UPDATE;
        pkt.header.player_id = 0;
        pkt.count = 0;

        for (int p = 0; p < proj_count; p++) {
            float d = dist2d(proj_snaps[p].px, proj_snaps[p].py,
                             snapshots[s].pos_x, snapshots[s].pos_y);

            if (d <= PROJECTILE_VIEW_RANGE) {
                if (pkt.count >= MAX_PROJECTILES_PER_PACKET) break;

                pkt.projectiles[pkt.count].projectile_id =
                    htonl(proj_snaps[p].id);
                pkt.projectiles[pkt.count].pos_x = proj_snaps[p].px;
                pkt.projectiles[pkt.count].pos_y = proj_snaps[p].py;
                pkt.count++;
            }
        }

        if (pkt.count > 0) {
            size_t send_size = sizeof(PacketHeader) + 4 +
                               pkt.count * sizeof(ProjectilePositionData);
            pkt.header.payload_size = htons((uint16_t)(send_size - sizeof(PacketHeader)));
            server_send(snapshots[s].client_fd, &pkt, send_size);
        }
    }
}
