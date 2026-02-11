// ============================================================================
// combat.c — implementation (UPDATED with stat scaling, defense, evasion,
//            XP on kill, HP regen)
// ============================================================================

#include "combat.h"
#include "combat_stats.h"
#include "player_level.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <time.h>

const ClassAttackProfile g_class_profiles[5] = {
    // [0] — unused (classes are 1-indexed)
    { .cast_time = 0.0f, .cooldown = 0.0f, .range = 0.0f,
      .base_damage = 0, .damage_variance = 0, .attack_type = 0,
      .cone_half_angle = 0.0f, .line_width = 0.0f },

    // [1] Gladiator — single target, heavy melee
    { .cast_time     = 1.2f,
      .cooldown      = 0.8f,
      .range         = 50.0f,
      .base_damage   = 50,
      .damage_variance = 20,
      .attack_type   = 0,  // ATTACK_TYPE_SINGLE
      .cone_half_angle = 0.0f,
      .line_width    = 0.0f },

    // [2] Ninja — cone cleave, fast melee
    { .cast_time     = 0.3f,
      .cooldown      = 0.2f,
      .range         = 50.0f,
      .base_damage   = 15,
      .damage_variance = 15,
      .attack_type   = 2,  // ATTACK_TYPE_CONE
      .cone_half_angle = 30.0f,
      .line_width    = 0.0f },

    // [3] Landweaver — AoE, slow ranged
    { .cast_time     = 0.8f,
      .cooldown      = 1.0f,
      .range         = 80.0f,
      .base_damage   = 35,
      .damage_variance = 25,
      .attack_type   = 1,  // ATTACK_TYPE_AOE
      .cone_half_angle = 0.0f,
      .line_width    = 0.0f },

    // [4] Spirit — line/piercing, medium range
    { .cast_time     = 0.6f,
      .cooldown      = 0.8f,
      .range         = 150.0f,
      .base_damage   = 20,
      .damage_variance = 20,
      .attack_type   = 3,  // ATTACK_TYPE_LINE
      .cone_half_angle = 0.0f,
      .line_width    = 12.0f }
};

// ---------------------------------------------------------------------------
// Helpers: time
// ---------------------------------------------------------------------------
static double combat_get_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

// ---------------------------------------------------------------------------
// Helpers: geometry
// ---------------------------------------------------------------------------

static float combat_dist(float ax, float ay, float bx, float by) {
    float dx = bx - ax;
    float dy = by - ay;
    return sqrtf(dx * dx + dy * dy);
}

static int combat_point_in_cone(float ox, float oy,
                                float aim_x, float aim_y,
                                float half_angle_rad,
                                float range,
                                float px, float py) {
    float dist = combat_dist(ox, oy, px, py);
    if (dist > range || dist < 0.001f) return 0;

    float aim_angle   = atan2f(aim_y - oy, aim_x - ox);
    float point_angle = atan2f(py - oy, px - ox);
    float delta       = point_angle - aim_angle;

    while (delta >  M_PI) delta -= 2.0f * M_PI;
    while (delta < -M_PI) delta += 2.0f * M_PI;

    return (fabsf(delta) <= half_angle_rad) ? 1 : 0;
}

static int combat_point_in_line(float ox, float oy,
                                float dir_x, float dir_y,
                                float range,
                                float half_width,
                                float px, float py) {
    float to_px = px - ox;
    float to_py = py - oy;
    float proj  = to_px * dir_x + to_py * dir_y;

    if (proj < 0.0f || proj > range) return 0;

    float perp_x = to_px - proj * dir_x;
    float perp_y = to_py - proj * dir_y;
    float perp_dist = sqrtf(perp_x * perp_x + perp_y * perp_y);

    return (perp_dist <= half_width) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// NPC world management
// ---------------------------------------------------------------------------

static uint32_t g_next_npc_id = 1000;

void combat_npc_init(NPCWorld* world) {
    memset(world->npcs, 0, sizeof(world->npcs));
    world->count = 0;
    pthread_mutex_init(&world->lock, NULL);
    printf("[COMBAT] NPC world initialized\n");
}

uint32_t combat_npc_spawn(NPCWorld* world,
                          const char* name,
                          float x, float y,
                          int health,
                          float hitbox_radius,
                          uint32_t dialogue_id,
                          uint8_t is_interactable) {
    pthread_mutex_lock(&world->lock);

    if (world->count >= MAX_NPCS) {
        pthread_mutex_unlock(&world->lock);
        fprintf(stderr, "[COMBAT] NPC pool full (%d/%d)\n", world->count, MAX_NPCS);
        return 0;
    }

    int slot = -1;
    for (int i = 0; i < MAX_NPCS; i++) {
        if (!world->npcs[i].is_alive && world->npcs[i].id == 0) {
            slot = i;
            break;
        }
    }
    if (slot == -1) {
        for (int i = 0; i < MAX_NPCS; i++) {
            if (!world->npcs[i].is_alive) {
                slot = i;
                break;
            }
        }
    }
    if (slot == -1) {
        pthread_mutex_unlock(&world->lock);
        fprintf(stderr, "[COMBAT] No reclaimable NPC slot\n");
        return 0;
    }

    NPCEntity* npc = &world->npcs[slot];
    memset(npc, 0, sizeof(NPCEntity));
    npc->id             = g_next_npc_id++;
    strncpy(npc->name, name, sizeof(npc->name) - 1);
    npc->name[sizeof(npc->name) - 1] = '\0';
    npc->pos_x          = x;
    npc->pos_y          = y;
    npc->health         = health;
    npc->max_health     = health;
    npc->hitbox_radius  = hitbox_radius;
    npc->is_alive       = 1;
    npc->xp_reward      = 50;    // Default XP reward
    npc->defense        = 0;     // Default NPC defense
    npc->evasion        = 0;     // Default NPC evasion
    npc->dialogue_id    = dialogue_id;
    npc->is_interactable = is_interactable;

    if (world->count < MAX_NPCS) world->count++;

    uint32_t id = npc->id;
    pthread_mutex_unlock(&world->lock);

    printf("[COMBAT] Spawned NPC '%s' id=%u at (%.1f, %.1f) hp=%d xp=%u\n",
           name, id, x, y, health, npc->xp_reward);
    return id;
}

void combat_npc_remove(NPCWorld* world, uint32_t npc_id) {
    pthread_mutex_lock(&world->lock);
    for (int i = 0; i < MAX_NPCS; i++) {
        if (world->npcs[i].id == npc_id) {
            memset(&world->npcs[i], 0, sizeof(NPCEntity));
            printf("[COMBAT] Removed NPC id=%u\n", npc_id);
            break;
        }
    }
    pthread_mutex_unlock(&world->lock);
}

// ---------------------------------------------------------------------------
// Pending casts
// ---------------------------------------------------------------------------

static PendingCast g_pending_casts[MAX_PLAYERS];
static pthread_mutex_t g_pending_casts_lock = PTHREAD_MUTEX_INITIALIZER;

static int combat_find_player_slot(uint32_t character_id) {
    extern ActivePlayer active_players[];
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (active_players[i].is_loaded &&
            active_players[i].character_id == character_id) {
            return i;
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// NEW: Compute final damage with stat scaling, variance, and defense
// ---------------------------------------------------------------------------
static int compute_final_damage(int base_damage, int damage_variance,
                                int attacker_str, int attacker_agi,
                                int attacker_int, int attacker_wis,
                                uint8_t attacker_class,
                                int target_defense) {
    // 1. Base damage + stat bonus
    int stat_bonus = combat_stat_bonus_damage(attacker_str, attacker_agi,
                                               attacker_int, attacker_wis,
                                               attacker_class);
    int damage = base_damage + stat_bonus;

    // 2. Random variance
    if (damage_variance > 0) {
        int variance = (rand() % (damage_variance * 2 + 1)) - damage_variance;
        damage += (damage * variance) / 100;
    }

    // 3. Apply target defense
    damage = combat_apply_defense(damage, target_defense);

    if (damage < 1) damage = 1;
    return damage;
}

// ---------------------------------------------------------------------------
// Attack intent handler
// ---------------------------------------------------------------------------

void combat_handle_attack_intent(NPCWorld* world,
                                 int client_fd,
                                 uint32_t attacker_id,
                                 AttackIntentPacket* pkt) {
    extern ActivePlayer active_players[];

    ActivePlayer* attacker = player_find_active(attacker_id);
    if (!attacker) {
        printf("[COMBAT] Attack intent from unknown attacker %u\n", attacker_id);
        return;
    }

    int player_slot = combat_find_player_slot(attacker_id);
    if (player_slot < 0) return;

    pthread_mutex_lock(&attacker->lock);

    double now = combat_get_time();
    if (now - attacker->last_attack_time < attacker->attack_cooldown) {
        pthread_mutex_unlock(&attacker->lock);

        AttackResultPacket result = {0};
        result.header.type       = PACKET_ATTACK_RESULT;
        result.header.player_id  = htonl(attacker_id);
        result.result_code       = ATTACK_RESULT_ON_COOLDOWN;
        send(client_fd, &result, sizeof(result), 0);
        return;
    }

    pthread_mutex_lock(&g_pending_casts_lock);
    if (g_pending_casts[player_slot].is_active) {
        pthread_mutex_unlock(&g_pending_casts_lock);
        pthread_mutex_unlock(&attacker->lock);

        AttackResultPacket result = {0};
        result.header.type       = PACKET_ATTACK_RESULT;
        result.header.player_id  = htonl(attacker_id);
        result.result_code       = ATTACK_RESULT_ALREADY_CASTING;
        send(client_fd, &result, sizeof(result), 0);
        return;
    }

    uint8_t class_id = attacker->player_class;
    if (class_id < 1 || class_id > 4) class_id = 1;
    const ClassAttackProfile* profile = &g_class_profiles[class_id];

    float origin_x = attacker->pos_x;
    float origin_y = attacker->pos_y;
    float aim_x    = pkt->aim_x;
    float aim_y    = pkt->aim_y;

    // Mark combat timestamp for HP regen suppression
    attacker->last_combat_time = now;

    pthread_mutex_unlock(&attacker->lock);

    // --- Resolve targets ---
    uint32_t hit_targets[MAX_CAST_TARGETS];
    uint8_t  hit_count = 0;

    pthread_mutex_lock(&world->lock);

    float aim_dx = aim_x - origin_x;
    float aim_dy = aim_y - origin_y;
    float aim_len = sqrtf(aim_dx * aim_dx + aim_dy * aim_dy);
    float dir_x = (aim_len > 0.001f) ? aim_dx / aim_len : 1.0f;
    float dir_y = (aim_len > 0.001f) ? aim_dy / aim_len : 0.0f;

    float cone_half_rad = profile->cone_half_angle * (M_PI / 180.0f);
    float line_half_w   = profile->line_width / 2.0f;

    uint32_t best_single_id   = 0;
    float    best_single_dist = 1e9f;

    for (int i = 0; i < MAX_NPCS && hit_count < MAX_CAST_TARGETS; i++) {
        NPCEntity* npc = &world->npcs[i];
        if (!npc->is_alive || npc->id == 0) continue;

        float npc_cx = npc->pos_x;
        float npc_cy = npc->pos_y;

        switch (profile->attack_type) {
            case ATTACK_TYPE_SINGLE: {
                float d = combat_dist(origin_x, origin_y, npc_cx, npc_cy);
                if (d <= profile->range + npc->hitbox_radius && d < best_single_dist) {
                    best_single_dist = d;
                    best_single_id   = npc->id;
                }
                break;
            }
            case ATTACK_TYPE_AOE: {
                float d = combat_dist(origin_x, origin_y, npc_cx, npc_cy);
                if (d <= profile->range + npc->hitbox_radius) {
                    hit_targets[hit_count++] = npc->id;
                }
                break;
            }
            case ATTACK_TYPE_CONE: {
                if (combat_point_in_cone(origin_x, origin_y, aim_x, aim_y,
                                         cone_half_rad, profile->range,
                                         npc_cx, npc_cy)) {
                    hit_targets[hit_count++] = npc->id;
                } else {
                    float d = combat_dist(origin_x, origin_y, npc_cx, npc_cy);
                    if (d <= profile->range + npc->hitbox_radius && d > 0.001f) {
                        float nx = npc_cx + (origin_x - npc_cx) / d * npc->hitbox_radius;
                        float ny = npc_cy + (origin_y - npc_cy) / d * npc->hitbox_radius;
                        if (combat_point_in_cone(origin_x, origin_y, aim_x, aim_y,
                                                 cone_half_rad, profile->range, nx, ny)) {
                            hit_targets[hit_count++] = npc->id;
                        }
                    }
                }
                break;
            }
            case ATTACK_TYPE_LINE: {
                if (combat_point_in_line(origin_x, origin_y, dir_x, dir_y,
                                         profile->range, line_half_w,
                                         npc_cx, npc_cy)) {
                    hit_targets[hit_count++] = npc->id;
                } else {
                    float d = combat_dist(origin_x, origin_y, npc_cx, npc_cy);
                    if (d <= profile->range + npc->hitbox_radius && d > 0.001f) {
                        float e1x = npc_cx + (-dir_y) * npc->hitbox_radius;
                        float e1y = npc_cy +  dir_x  * npc->hitbox_radius;
                        float e2x = npc_cx - (-dir_y) * npc->hitbox_radius;
                        float e2y = npc_cy -  dir_x  * npc->hitbox_radius;
                        if (combat_point_in_line(origin_x, origin_y, dir_x, dir_y,
                                                 profile->range, line_half_w, e1x, e1y) ||
                            combat_point_in_line(origin_x, origin_y, dir_x, dir_y,
                                                 profile->range, line_half_w, e2x, e2y)) {
                            hit_targets[hit_count++] = npc->id;
                        }
                    }
                }
                break;
            }
        }
    }

    if (profile->attack_type == ATTACK_TYPE_SINGLE && best_single_id != 0) {
        hit_targets[0] = best_single_id;
        hit_count = 1;
    }

    pthread_mutex_unlock(&world->lock);

    if (hit_count == 0) {
        pthread_mutex_unlock(&g_pending_casts_lock);

        AttackResultPacket result = {0};
        result.header.type       = PACKET_ATTACK_RESULT;
        result.header.player_id  = htonl(attacker_id);
        result.result_code       = ATTACK_RESULT_NO_TARGETS;
        send(client_fd, &result, sizeof(result), 0);
        return;
    }

    PendingCast* cast = &g_pending_casts[player_slot];
    cast->is_active        = 1;
    cast->cast_start_time  = now;
    cast->cast_duration    = profile->cast_time;
    cast->attack_type      = profile->attack_type;
    cast->origin_x         = origin_x;
    cast->origin_y         = origin_y;
    cast->aim_x            = aim_x;
    cast->aim_y            = aim_y;
    cast->range            = profile->range;
    cast->base_damage      = profile->base_damage;
    cast->damage_variance  = profile->damage_variance;
    cast->cone_half_angle  = profile->cone_half_angle;
    cast->line_width       = profile->line_width;
    cast->cooldown         = profile->cooldown;

    pthread_mutex_unlock(&g_pending_casts_lock);

    CastStartV2Packet cast_pkt;
    memset(&cast_pkt, 0, sizeof(cast_pkt));
    cast_pkt.header.type        = PACKET_CAST_START_V2;
    cast_pkt.header.player_id   = htonl(attacker_id);
    cast_pkt.caster_id          = htonl(attacker_id);
    cast_pkt.cast_time          = profile->cast_time;
    cast_pkt.attack_type        = profile->attack_type;
    cast_pkt.target_count       = hit_count;
    cast_pkt.origin_x           = origin_x;
    cast_pkt.origin_y           = origin_y;
    cast_pkt.aim_x              = aim_x;
    cast_pkt.aim_y              = aim_y;

    for (uint8_t i = 0; i < hit_count; i++) {
        cast_pkt.target_ids[i] = htonl(hit_targets[i]);
    }

    send(client_fd, &cast_pkt, sizeof(cast_pkt), 0);

    printf("[COMBAT] Player %u cast started: type=%d, targets=%d, cast_time=%.2fs\n",
           attacker_id, profile->attack_type, hit_count, profile->cast_time);
}

// ---------------------------------------------------------------------------
// Cast cancel handler
// ---------------------------------------------------------------------------

void combat_handle_cast_cancel(int client_fd, uint32_t attacker_id) {
    int slot = combat_find_player_slot(attacker_id);
    if (slot < 0) return;

    pthread_mutex_lock(&g_pending_casts_lock);
    if (g_pending_casts[slot].is_active) {
        g_pending_casts[slot].is_active = 0;

        CastCancelPacket cancel;
        memset(&cancel, 0, sizeof(cancel));
        cancel.header.type      = PACKET_CAST_CANCEL;
        cancel.header.player_id = htonl(attacker_id);
        cancel.caster_id        = htonl(attacker_id);
        cancel.reason           = 0;

        send(client_fd, &cancel, sizeof(cancel), 0);
        printf("[COMBAT] Player %u cancelled cast\n", attacker_id);
    }
    pthread_mutex_unlock(&g_pending_casts_lock);
}

// ---------------------------------------------------------------------------
// Per-tick combat resolution + HP regen
// ---------------------------------------------------------------------------

void combat_tick(NPCWorld* world) {
    extern ActivePlayer active_players[];
    extern pthread_mutex_t active_players_lock;
    double now = combat_get_time();

    // -------------------------------------------------------------------
    // 1. Resolve pending basic-attack casts
    // -------------------------------------------------------------------
    pthread_mutex_lock(&g_pending_casts_lock);

    for (int i = 0; i < MAX_PLAYERS; i++) {
        PendingCast* cast = &g_pending_casts[i];
        if (!cast->is_active) continue;

        double elapsed = now - cast->cast_start_time;
        if (elapsed < cast->cast_duration) continue;

        cast->is_active = 0;
        uint32_t attacker_id = active_players[i].character_id;
        int      client_fd   = active_players[i].client_fd;

        // Snapshot attacker stats for damage calc
        pthread_mutex_lock(&active_players[i].lock);
        active_players[i].last_attack_time = now;
        active_players[i].attack_cooldown  = cast->cooldown;
        active_players[i].last_combat_time = now;

        int a_str   = active_players[i].strength;
        int a_agi   = active_players[i].agility;
        int a_int   = active_players[i].intelligence;
        int a_wis   = active_players[i].wisdom;
        uint8_t a_class = active_players[i].player_class;
        pthread_mutex_unlock(&active_players[i].lock);

        float origin_x      = cast->origin_x;
        float origin_y      = cast->origin_y;
        float aim_x         = cast->aim_x;
        float aim_y         = cast->aim_y;
        float range         = cast->range;
        uint8_t attack_type = cast->attack_type;

        float aim_dx  = aim_x - origin_x;
        float aim_dy  = aim_y - origin_y;
        float aim_len = sqrtf(aim_dx * aim_dx + aim_dy * aim_dy);
        float dir_x   = (aim_len > 0.001f) ? aim_dx / aim_len : 1.0f;
        float dir_y   = (aim_len > 0.001f) ? aim_dy / aim_len : 0.0f;
        float cone_half_rad = cast->cone_half_angle * (M_PI / 180.0f);
        float line_half_w   = cast->line_width / 2.0f;

        // Collect hit results under lock, send after unlock
        #define MAX_HIT_RESULTS 16
        typedef struct {
            uint32_t target_id;
            uint32_t damage;
            uint32_t new_health;
            uint8_t  is_kill;
            uint64_t xp_reward;     // >0 if should award XP
        } HitResult;

        HitResult hits[MAX_HIT_RESULTS];
        int hit_count = 0;

        pthread_mutex_lock(&world->lock);

        // --- SINGLE target: find closest, damage it ---
        if (attack_type == ATTACK_TYPE_SINGLE) {
            NPCEntity* best = NULL;
            float best_dist = 1e9f;

            for (int n = 0; n < MAX_NPCS; n++) {
                NPCEntity* npc = &world->npcs[n];
                if (!npc->is_alive || npc->id == 0) continue;
                float d = combat_dist(origin_x, origin_y, npc->pos_x, npc->pos_y);
                if (d <= range + npc->hitbox_radius && d < best_dist) {
                    best_dist = d;
                    best      = npc;
                }
            }

            if (best) {
                if (combat_check_evasion(best->evasion)) {
                    // MISS
                    hits[hit_count].target_id   = best->id;
                    hits[hit_count].damage      = 0;
                    hits[hit_count].new_health  = (uint32_t)best->health;
                    hits[hit_count].is_kill     = 0;
                    hits[hit_count].xp_reward   = 0;
                    hit_count++;
                    printf("[COMBAT] %u MISSED NPC %u (evasion)\n", attacker_id, best->id);
                } else {
                    int damage = compute_final_damage(cast->base_damage, cast->damage_variance,
                                                       a_str, a_agi, a_int, a_wis, a_class,
                                                       best->defense);
                    best->health -= damage;
                    if (best->health < 0) best->health = 0;

                    uint8_t is_kill = (best->health == 0) ? 1 : 0;
                    if (is_kill) best->is_alive = 0;

                    hits[hit_count].target_id   = best->id;
                    hits[hit_count].damage      = (uint32_t)damage;
                    hits[hit_count].new_health  = (uint32_t)best->health;
                    hits[hit_count].is_kill     = is_kill;
                    hits[hit_count].xp_reward   = (is_kill && best->xp_reward > 0) ? best->xp_reward : 0;
                    hit_count++;

                    printf("[COMBAT] %u hit NPC %u (%s) for %d dmg (hp=%d)%s\n",
                           attacker_id, best->id, best->name, damage, best->health,
                           is_kill ? " — KILLED" : "");
                }
            }

            pthread_mutex_unlock(&world->lock);

            // Send packets outside lock
            for (int h = 0; h < hit_count; h++) {
                DamageV2Packet dmg = {0};
                dmg.header.type         = PACKET_DAMAGE_V2;
                dmg.header.player_id    = htonl(attacker_id);
                dmg.attacker_id         = htonl(attacker_id);
                dmg.target_id           = htonl(hits[h].target_id);
                dmg.damage              = htonl(hits[h].damage);
                dmg.target_new_health   = htonl(hits[h].new_health);
                dmg.is_kill             = hits[h].is_kill;
                send(client_fd, &dmg, sizeof(dmg), 0);

                if (hits[h].xp_reward > 0) {
                    ActivePlayer* killer = player_find_active(attacker_id);
                    if (killer) player_award_xp(killer, hits[h].xp_reward);
                }
            }
            continue;  // Next pending cast
        }

        // --- AOE / CONE / LINE targets ---
        for (int n = 0; n < MAX_NPCS; n++) {
            NPCEntity* npc = &world->npcs[n];
            if (!npc->is_alive || npc->id == 0) continue;

            int hit = 0;
            float npc_cx = npc->pos_x;
            float npc_cy = npc->pos_y;

            switch (attack_type) {
                case ATTACK_TYPE_AOE: {
                    float d = combat_dist(origin_x, origin_y, npc_cx, npc_cy);
                    hit = (d <= range + npc->hitbox_radius);
                    break;
                }
                case ATTACK_TYPE_CONE: {
                    hit = combat_point_in_cone(origin_x, origin_y,
                                               aim_x, aim_y,
                                               cone_half_rad, range,
                                               npc_cx, npc_cy);
                    if (!hit) {
                        float d = combat_dist(origin_x, origin_y, npc_cx, npc_cy);
                        if (d <= range + npc->hitbox_radius && d > 0.001f) {
                            float nx = npc_cx + (origin_x - npc_cx) / d * npc->hitbox_radius;
                            float ny = npc_cy + (origin_y - npc_cy) / d * npc->hitbox_radius;
                            hit = combat_point_in_cone(origin_x, origin_y,
                                                       aim_x, aim_y,
                                                       cone_half_rad, range, nx, ny);
                        }
                    }
                    break;
                }
                case ATTACK_TYPE_LINE: {
                    hit = combat_point_in_line(origin_x, origin_y,
                                               dir_x, dir_y,
                                               range, line_half_w,
                                               npc_cx, npc_cy);
                    if (!hit) {
                        float d = combat_dist(origin_x, origin_y, npc_cx, npc_cy);
                        if (d <= range + npc->hitbox_radius && d > 0.001f) {
                            float e1x = npc_cx + (-dir_y) * npc->hitbox_radius;
                            float e1y = npc_cy +  dir_x  * npc->hitbox_radius;
                            float e2x = npc_cx - (-dir_y) * npc->hitbox_radius;
                            float e2y = npc_cy -  dir_x  * npc->hitbox_radius;
                            hit = combat_point_in_line(origin_x, origin_y, dir_x, dir_y,
                                                       range, line_half_w, e1x, e1y) ||
                                  combat_point_in_line(origin_x, origin_y, dir_x, dir_y,
                                                       range, line_half_w, e2x, e2y);
                        }
                    }
                    break;
                }
            }

            if (!hit) continue;
            if (hit_count >= MAX_HIT_RESULTS) break;

            // Evasion check per target
            if (combat_check_evasion(npc->evasion)) {
                hits[hit_count].target_id   = npc->id;
                hits[hit_count].damage      = 0;
                hits[hit_count].new_health  = (uint32_t)npc->health;
                hits[hit_count].is_kill     = 0;
                hits[hit_count].xp_reward   = 0;
                hit_count++;
                printf("[COMBAT] %u MISSED NPC %u (evasion)\n", attacker_id, npc->id);
                continue;
            }

            int damage = compute_final_damage(cast->base_damage, cast->damage_variance,
                                               a_str, a_agi, a_int, a_wis, a_class,
                                               npc->defense);

            npc->health -= damage;
            if (npc->health < 0) npc->health = 0;

            uint8_t is_kill = (npc->health == 0) ? 1 : 0;
            if (is_kill) npc->is_alive = 0;

            hits[hit_count].target_id   = npc->id;
            hits[hit_count].damage      = (uint32_t)damage;
            hits[hit_count].new_health  = (uint32_t)npc->health;
            hits[hit_count].is_kill     = is_kill;
            hits[hit_count].xp_reward   = (is_kill && npc->xp_reward > 0) ? npc->xp_reward : 0;
            hit_count++;

            printf("[COMBAT] %u hit NPC %u (%s) for %d dmg (hp=%d)%s\n",
                   attacker_id, npc->id, npc->name, damage, npc->health,
                   is_kill ? " — KILLED" : "");
        }

        pthread_mutex_unlock(&world->lock);

        // Send all damage packets outside lock
        for (int h = 0; h < hit_count; h++) {
            DamageV2Packet dmg = {0};
            dmg.header.type         = PACKET_DAMAGE_V2;
            dmg.header.player_id    = htonl(attacker_id);
            dmg.attacker_id         = htonl(attacker_id);
            dmg.target_id           = htonl(hits[h].target_id);
            dmg.damage              = htonl(hits[h].damage);
            dmg.target_new_health   = htonl(hits[h].new_health);
            dmg.is_kill             = hits[h].is_kill;
            send(client_fd, &dmg, sizeof(dmg), 0);

            if (hits[h].xp_reward > 0) {
                ActivePlayer* killer = player_find_active(attacker_id);
                if (killer) player_award_xp(killer, hits[h].xp_reward);
            }
        }
    }

    pthread_mutex_unlock(&g_pending_casts_lock);

    // -------------------------------------------------------------------
    // 2. HP regen (out-of-combat only)
    // -------------------------------------------------------------------
    {
        static float hp_accum[MAX_PLAYERS];
        float dt = 0.05f;  // 20Hz tick

        pthread_mutex_lock(&active_players_lock);
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (!active_players[i].is_loaded) continue;

            pthread_mutex_lock(&active_players[i].lock);

            // Check out-of-combat
            double time_since_combat = now - active_players[i].last_combat_time;
            if (time_since_combat >= OOC_THRESHOLD_SECONDS &&
                active_players[i].health > 0 &&
                active_players[i].health < active_players[i].max_health) {

                float regen = combat_hp_regen_rate(active_players[i].max_health) * dt;
                hp_accum[i] += regen;

                if (hp_accum[i] >= 1.0f) {
                    int regen_int = (int)hp_accum[i];
                    hp_accum[i] -= (float)regen_int;

                    active_players[i].health += regen_int;
                    if (active_players[i].health > active_players[i].max_health)
                        active_players[i].health = active_players[i].max_health;

                    // Don't spam packets — client can interpolate
                    // Only send on significant changes (every ~1 second)
                }
            } else {
                hp_accum[i] = 0.0f;  // Reset accumulator when in combat
            }

            pthread_mutex_unlock(&active_players[i].lock);
        }
        pthread_mutex_unlock(&active_players_lock);
    }
}