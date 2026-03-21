// ============================================================================
// combat.c — implementation (UPDATED with stat scaling, defense, evasion,
//            XP on kill, HP regen)
// ============================================================================

#include "combat.h"
#include "combat_stats.h"
#include "player_level.h"
#include "player_data.h"
#include "party.h"
#include "loot.h"
#include "quest_system.h"
#include "projectile.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <time.h>
#include "utils.h"

ClassAttackProfile g_class_profiles[5] = {
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
      .attack_type   = 0,
      .cone_half_angle = 0.0f,
      .line_width    = 0.0f },

    // [2] Ninja — cone cleave, fast melee
    { .cast_time     = 0.3f,
      .cooldown      = 0.2f,
      .range         = 50.0f,
      .base_damage   = 15,
      .damage_variance = 15,
      .attack_type   = 2,
      .cone_half_angle = 30.0f,
      .line_width    = 0.0f },

    // [3] Landweaver — targeted ranged bolt (earth)
    { .cast_time     = 0.4f,
      .cooldown      = 1.0f,
      .range         = 400.0f,
      .base_damage   = 35,
      .damage_variance = 25,
      .attack_type   = 0,      // SINGLE — target must be within range at intent
      .cone_half_angle = 0.0f,
      .line_width    = 0.0f,
      .is_ranged               = 1,
      .projectile_speed        = 350.0f,
      .projectile_width        = 14.0f,
      .projectile_damage_stat  = 6,   // STAT_INTELLIGENCE
      .projectile_damage_type  = 1 }, // ABILITY_DMG_EARTH

    // [4] Spirit — targeted ranged bolt (spirit)
    { .cast_time     = 0.3f,
      .cooldown      = 0.8f,
      .range         = 500.0f,
      .base_damage   = 20,
      .damage_variance = 20,
      .attack_type   = 0,      // SINGLE
      .cone_half_angle = 0.0f,
      .line_width    = 0.0f,
      .is_ranged               = 1,
      .projectile_speed        = 450.0f,
      .projectile_width        = 10.0f,
      .projectile_damage_stat  = 7,   // STAT_WISDOM
      .projectile_damage_type  = 2 }  // ABILITY_DMG_SPIRIT
};

// ---------------------------------------------------------------------------
// Load attack profiles from JSON. Falls back to compiled-in defaults on error.
// ---------------------------------------------------------------------------
static const char* ap_find_value(const char* json, const char* key) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char* pos = strstr(json, search);
    if (!pos) return NULL;
    pos = strchr(pos, ':');
    if (!pos) return NULL;
    pos++;
    while (*pos == ' ' || *pos == '\t') pos++;
    return pos;
}

int combat_profiles_load(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[COMBAT] Could not open %s — using compiled defaults\n", path);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    char* buf = malloc((size_t)size + 1);
    if (!buf) { fclose(f); return 0; }
    fread(buf, 1, (size_t)size, f);
    buf[size] = '\0';
    fclose(f);

    int loaded = 0;
    const char* cur = buf;
    while ((cur = strstr(cur, "\"class_id\"")) != NULL) {
        const char* obj_start = cur;

        // Find class_id
        const char* v = ap_find_value(obj_start, "class_id");
        if (!v) { cur++; continue; }
        int class_id = atoi(v);
        if (class_id < 1 || class_id > 4) { cur++; continue; }

        ClassAttackProfile* p = &g_class_profiles[class_id];

        v = ap_find_value(obj_start, "base_damage");
        if (v) p->base_damage = atoi(v);

        v = ap_find_value(obj_start, "damage_variance");
        if (v) p->damage_variance = atoi(v);

        v = ap_find_value(obj_start, "cast_time");
        if (v) p->cast_time = (float)atof(v);

        v = ap_find_value(obj_start, "cooldown");
        if (v) p->cooldown = (float)atof(v);

        v = ap_find_value(obj_start, "range");
        if (v) p->range = (float)atof(v);

        v = ap_find_value(obj_start, "attack_type");
        if (v) p->attack_type = (uint8_t)atoi(v);

        v = ap_find_value(obj_start, "cone_half_angle");
        if (v) p->cone_half_angle = (float)atof(v);

        v = ap_find_value(obj_start, "line_width");
        if (v) p->line_width = (float)atof(v);

        v = ap_find_value(obj_start, "is_ranged");
        if (v) p->is_ranged = (uint8_t)atoi(v);

        v = ap_find_value(obj_start, "projectile_speed");
        if (v) p->projectile_speed = (float)atof(v);

        v = ap_find_value(obj_start, "projectile_width");
        if (v) p->projectile_width = (float)atof(v);

        v = ap_find_value(obj_start, "projectile_damage_stat");
        if (v) p->projectile_damage_stat = (uint8_t)atoi(v);

        v = ap_find_value(obj_start, "projectile_damage_type");
        if (v) p->projectile_damage_type = (uint8_t)atoi(v);

        printf("[COMBAT] Loaded attack profile for class %d: "
               "dmg=%d ±%d%% cast=%.2fs cd=%.2fs range=%.0f%s\n",
               class_id, p->base_damage, p->damage_variance,
               p->cast_time, p->cooldown, p->range,
               p->is_ranged ? " [RANGED]" : "");
        loaded++;
        cur++;
    }

    free(buf);
    printf("[COMBAT] %d attack profile(s) loaded from %s\n", loaded, path);
    return loaded;
}

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
                          uint8_t is_interactable,
                          uint16_t npc_type_id,
                          float respawn_time,
                          uint8_t category) {
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
    npc->category       = category;
    npc->xp_reward      = (category == NPC_CATEGORY_HOSTILE) ? 50 : 0;
    npc->defense        = 0;
    npc->evasion        = 0;
    npc->dialogue_id    = dialogue_id;
    npc->is_interactable = is_interactable;
    npc->spawn_x        = x;
    npc->spawn_y        = y;
    npc->npc_type_id    = npc_type_id;
    npc->respawn_time   = respawn_time;
    npc->death_time     = 0.0;

    if (world->count < MAX_NPCS) world->count++;

    uint32_t id = npc->id;
    pthread_mutex_unlock(&world->lock);

    const char* cat_names[] = {"passive", "hostile", "quest"};
    printf("[COMBAT] Spawned NPC '%s' id=%u at (%.1f, %.1f) hp=%d category=%s\n",
           name, id, x, y, health, cat_names[category < 3 ? category : 0]);
    return id;
}

NPCEntity* combat_npc_find(NPCWorld* world, uint32_t npc_id) {
    for (int i = 0; i < MAX_NPCS; i++) {
        if (world->npcs[i].id == npc_id) {
            return &world->npcs[i];
        }
    }
    return NULL;
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
    (void)attacker_agi; (void)attacker_int; (void)attacker_wis; (void)attacker_class;
    // 1. STR scales basic attack damage for all classes equally.
    //    Ability damage uses per-class primary stat (in ability_handler.c).
    float mult = 1.0f + ((float)attacker_str / STAT_DAMAGE_DIVISOR);
    int damage = (int)((float)base_damage * mult);

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

    ActivePlayer* attacker = player_acquire(attacker_id);
    if (!attacker) {
        printf("[COMBAT] Attack intent from unknown attacker %u\n", attacker_id);
        return;
    }

    int player_slot = combat_find_player_slot(attacker_id);
    if (player_slot < 0) {
        player_release(attacker);
        return;
    }

    double now = combat_get_time();
    if (now - attacker->last_attack_time < attacker->attack_cooldown) {
        player_release(attacker);

        AttackResultPacket result = {0};
        result.header.type       = PACKET_ATTACK_RESULT;
        result.header.player_id  = htonl(attacker_id);
        result.result_code       = ATTACK_RESULT_ON_COOLDOWN;
        server_send(client_fd, &result, sizeof(result));
        return;
    }

    pthread_mutex_lock(&g_pending_casts_lock);
    if (g_pending_casts[player_slot].is_active) {
        pthread_mutex_unlock(&g_pending_casts_lock);
        player_release(attacker);

        AttackResultPacket result = {0};
        result.header.type       = PACKET_ATTACK_RESULT;
        result.header.player_id  = htonl(attacker_id);
        result.result_code       = ATTACK_RESULT_ALREADY_CASTING;
        server_send(client_fd, &result, sizeof(result));
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

    player_release(attacker);

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
        server_send(client_fd, &result, sizeof(result));
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
    cast->is_ranged              = profile->is_ranged;
    cast->projectile_speed       = profile->projectile_speed;
    cast->projectile_width       = profile->projectile_width;
    cast->projectile_damage_stat = profile->projectile_damage_stat;
    cast->projectile_damage_type = profile->projectile_damage_type;

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

    server_send(client_fd, &cast_pkt, sizeof(cast_pkt));

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
        cancel.header.type         = PACKET_CAST_CANCEL;
        cancel.header.player_id    = htonl(attacker_id);
        cancel.header.payload_size = htons(sizeof(CastCancelPacket) - sizeof(PacketHeader));
        cancel.caster_id           = htonl(attacker_id);
        cancel.reason              = 0;

        server_send(client_fd, &cancel, sizeof(cancel));
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
        int a_wpn   = active_players[i].weapon_damage;
        int a_luck  = active_players[i].luck;
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

        // Ranged auto-attack: spawn a projectile toward the aim point.
        // The projectile system handles hit detection, damage, and packets.
        if (cast->is_ranged) {
            ProjectileSpawnInfo info = {0};
            info.owner_type            = PROJECTILE_OWNER_PLAYER;
            info.owner_id              = attacker_id;
            info.owner_fd              = client_fd;
            info.ability_id            = 0;  // 0 = basic auto-attack (no ability icon)
            info.origin_x              = origin_x;
            info.origin_y              = origin_y;
            info.aim_x                 = aim_x;
            info.aim_y                 = aim_y;
            info.speed                 = cast->projectile_speed;
            info.width                 = cast->projectile_width;
            info.max_range             = range;
            info.damage                = cast->base_damage + a_wpn;
            info.damage_type           = (AbilityDamageType)cast->projectile_damage_type;
            info.caster_strength       = a_str;
            info.caster_agility        = a_agi;
            info.caster_intelligence   = a_int;
            info.caster_wisdom         = a_wis;
            info.damage_stat           = (int)cast->projectile_damage_stat;
            info.effect_count          = 0;
            projectile_spawn(&info);
            continue;
        }

        // Collect hit results under lock, send after unlock
        #define MAX_HIT_RESULTS 16
        typedef struct {
            uint32_t target_id;
            uint32_t damage;
            uint32_t new_health;
            uint8_t  is_kill;
            uint8_t  is_crit;
            uint64_t xp_reward;
            uint32_t gold_reward;
            uint16_t npc_type_id;   // For loot roll on kill
            float    npc_x, npc_y;  // NPC position for loot drop
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
                    int damage = compute_final_damage(cast->base_damage + a_wpn, cast->damage_variance,
                                                       a_str, a_agi, a_int, a_wis, a_class,
                                                       best->defense);
                    uint8_t is_crit = (uint8_t)combat_check_crit(a_luck);
                    if (is_crit) damage = (int)((float)damage * CRIT_DAMAGE_MULTIPLIER);

                    best->health -= damage;
                    if (best->health < 0) best->health = 0;

                    uint8_t is_kill = (best->health == 0) ? 1 : 0;
                    if (is_kill) {
                        best->is_alive = 0;
                        best->death_time = now;
                    }

                    hits[hit_count].target_id   = best->id;
                    hits[hit_count].damage      = (uint32_t)damage;
                    hits[hit_count].new_health  = (uint32_t)best->health;
                    hits[hit_count].is_kill     = is_kill;
                    hits[hit_count].is_crit     = is_crit;
                    hits[hit_count].xp_reward   = (is_kill && best->xp_reward > 0) ? best->xp_reward : 0;
                    hits[hit_count].gold_reward = is_kill ? best->gold_reward : 0;
                    hits[hit_count].npc_type_id = best->npc_type_id;
                    hits[hit_count].npc_x       = best->pos_x;
                    hits[hit_count].npc_y       = best->pos_y;
                    hit_count++;

                    printf("[COMBAT] %u hit NPC %u (%s) for %d dmg%s (hp=%d)%s\n",
                           attacker_id, best->id, best->name, damage,
                           is_crit ? " (CRIT)" : "",
                           best->health, is_kill ? " — KILLED" : "");
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
                server_send(client_fd, &dmg, sizeof(dmg));

                if (hits[h].is_kill) {
                    if (hits[h].xp_reward > 0) {
                        party_award_xp(attacker_id, hits[h].xp_reward);
                    }
                    if (hits[h].gold_reward > 0 || hits[h].xp_reward > 0) {
                        ActivePlayer* killer = player_acquire(attacker_id);
                        if (killer) {
                            if (hits[h].gold_reward > 0)
                                player_award_gold_locked(killer, hits[h].gold_reward);
                            player_send_kill_reward_locked(client_fd, killer,
                                (uint32_t)hits[h].xp_reward, hits[h].gold_reward);
                            player_release(killer);
                        }
                    }
                    loot_roll(hits[h].npc_type_id, hits[h].npc_x, hits[h].npc_y, attacker_id);
                    quest_on_npc_kill(attacker_id, client_fd, hits[h].npc_type_id);
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

            int damage = compute_final_damage(cast->base_damage + a_wpn, cast->damage_variance,
                                               a_str, a_agi, a_int, a_wis, a_class,
                                               npc->defense);
            uint8_t is_crit = (uint8_t)combat_check_crit(a_luck);
            if (is_crit) damage = (int)((float)damage * CRIT_DAMAGE_MULTIPLIER);

            npc->health -= damage;
            if (npc->health < 0) npc->health = 0;

            uint8_t is_kill = (npc->health == 0) ? 1 : 0;
            if (is_kill) {
                npc->is_alive = 0;
                npc->death_time = now;
            }

            hits[hit_count].target_id   = npc->id;
            hits[hit_count].damage      = (uint32_t)damage;
            hits[hit_count].new_health  = (uint32_t)npc->health;
            hits[hit_count].is_kill     = is_kill;
            hits[hit_count].is_crit     = is_crit;
            hits[hit_count].xp_reward   = (is_kill && npc->xp_reward > 0) ? npc->xp_reward : 0;
            hits[hit_count].gold_reward = is_kill ? npc->gold_reward : 0;
            hits[hit_count].npc_type_id = npc->npc_type_id;
            hits[hit_count].npc_x       = npc->pos_x;
            hits[hit_count].npc_y       = npc->pos_y;
            hit_count++;

            printf("[COMBAT] %u hit NPC %u (%s) for %d dmg%s (hp=%d)%s\n",
                   attacker_id, npc->id, npc->name, damage,
                   is_crit ? " (CRIT)" : "",
                   npc->health, is_kill ? " — KILLED" : "");
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
            dmg.is_crit             = hits[h].is_crit;
            server_send(client_fd, &dmg, sizeof(dmg));

            if (hits[h].is_kill) {
                if (hits[h].xp_reward > 0) {
                    party_award_xp(attacker_id, hits[h].xp_reward);
                }
                if (hits[h].gold_reward > 0 || hits[h].xp_reward > 0) {
                    ActivePlayer* killer = player_acquire(attacker_id);
                    if (killer) {
                        if (hits[h].gold_reward > 0)
                            player_award_gold_locked(killer, hits[h].gold_reward);
                        player_send_kill_reward_locked(client_fd, killer,
                            (uint32_t)hits[h].xp_reward, hits[h].gold_reward);
                        player_release(killer);
                    }
                }
                loot_roll(hits[h].npc_type_id, hits[h].npc_x, hits[h].npc_y, attacker_id);
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

                float regen = combat_hp_regen_rate(active_players[i].max_health) * combat_reg_heal_mult(active_players[i].reg) * dt;
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

    // -------------------------------------------------------------------
    // 3. Player death detection — snapshot deaths under lock, send after
    // -------------------------------------------------------------------
    {
        #define MAX_DEATH_EVENTS 16
        typedef struct {
            uint32_t player_id;
            int      client_fd;
        } DeathEvent;

        DeathEvent deaths[MAX_DEATH_EVENTS];
        int death_count = 0;

        pthread_mutex_lock(&active_players_lock);
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (!active_players[i].is_loaded) continue;
            pthread_mutex_lock(&active_players[i].lock);

            if (active_players[i].health <= 0 && !active_players[i].is_dead) {
                active_players[i].is_dead = 1;
                active_players[i].death_time = now;
                active_players[i].health = 0;

                if (death_count < MAX_DEATH_EVENTS) {
                    deaths[death_count].player_id = active_players[i].character_id;
                    deaths[death_count].client_fd = active_players[i].client_fd;
                    death_count++;
                }

                printf("[COMBAT] Player %u has died!\n", active_players[i].character_id);
            }

            pthread_mutex_unlock(&active_players[i].lock);
        }
        pthread_mutex_unlock(&active_players_lock);

        // Send death packets outside lock
        for (int d = 0; d < death_count; d++) {
            PlayerDeathPacket pkt = {0};
            pkt.header.type         = PACKET_PLAYER_DEATH;
            pkt.header.player_id    = htonl(deaths[d].player_id);
            pkt.header.payload_size = htons(sizeof(PlayerDeathPacket) - sizeof(PacketHeader));
            pkt.dead_player_id = htonl(deaths[d].player_id);
            pkt.killer_id = 0;
            pkt.killer_type = 0;
            server_send(deaths[d].client_fd, &pkt, sizeof(pkt));
        }
    }

    // -------------------------------------------------------------------
    // 4. Player respawn — 3 seconds after death
    // -------------------------------------------------------------------
    {
        #define RESPAWN_DELAY 3.0
        #define RESPAWN_X 608.0f
        #define RESPAWN_Y 608.0f

        #define MAX_RESPAWN_EVENTS 16
        typedef struct {
            uint32_t player_id;
            int      client_fd;
            int32_t  health;
            int32_t  max_health;
            int32_t  mana;
            int32_t  max_mana;
        } RespawnEvent;

        RespawnEvent respawns[MAX_RESPAWN_EVENTS];
        int respawn_count = 0;

        pthread_mutex_lock(&active_players_lock);
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (!active_players[i].is_loaded) continue;
            pthread_mutex_lock(&active_players[i].lock);

            if (active_players[i].is_dead &&
                (now - active_players[i].death_time) >= RESPAWN_DELAY) {

                active_players[i].is_dead = 0;
                active_players[i].health = active_players[i].max_health;
                active_players[i].mana = active_players[i].max_mana;
                active_players[i].pos_x = RESPAWN_X;
                active_players[i].pos_y = RESPAWN_Y;
                active_players[i].is_dirty = 1;

                if (respawn_count < MAX_RESPAWN_EVENTS) {
                    respawns[respawn_count].player_id  = active_players[i].character_id;
                    respawns[respawn_count].client_fd   = active_players[i].client_fd;
                    respawns[respawn_count].health      = active_players[i].health;
                    respawns[respawn_count].max_health   = active_players[i].max_health;
                    respawns[respawn_count].mana        = active_players[i].mana;
                    respawns[respawn_count].max_mana     = active_players[i].max_mana;
                    respawn_count++;
                }

                printf("[COMBAT] Player %u respawned at (%.0f, %.0f)\n",
                       active_players[i].character_id, RESPAWN_X, RESPAWN_Y);
            }

            pthread_mutex_unlock(&active_players[i].lock);
        }
        pthread_mutex_unlock(&active_players_lock);

        for (int r = 0; r < respawn_count; r++) {
            PlayerRespawnPacket pkt = {0};
            pkt.header.type         = PACKET_PLAYER_RESPAWN;
            pkt.header.player_id    = htonl(respawns[r].player_id);
            pkt.header.payload_size = htons(sizeof(PlayerRespawnPacket) - sizeof(PacketHeader));
            pkt.player_id = htonl(respawns[r].player_id);
            pkt.pos_x = RESPAWN_X;
            pkt.pos_y = RESPAWN_Y;
            pkt.health = htonl((uint32_t)respawns[r].health);
            pkt.max_health = htonl((uint32_t)respawns[r].max_health);
            pkt.mana = htonl((uint32_t)respawns[r].mana);
            pkt.max_mana = htonl((uint32_t)respawns[r].max_mana);
            server_send(respawns[r].client_fd, &pkt, sizeof(pkt));
        }
    }

    // -------------------------------------------------------------------
    // 5. NPC respawn — check dead NPCs with respawn timers
    // -------------------------------------------------------------------
    pthread_mutex_lock(&world->lock);
    for (int i = 0; i < MAX_NPCS; i++) {
        NPCEntity* npc = &world->npcs[i];
        if (npc->id == 0) continue;
        if (npc->is_alive) continue;
        if (npc->respawn_time <= 0.0f) continue;

        if (npc->death_time > 0.0 && (now - npc->death_time) >= npc->respawn_time) {
            npc->is_alive = 1;
            npc->health = npc->max_health;
            npc->pos_x = npc->spawn_x;
            npc->pos_y = npc->spawn_y;
            npc->death_time = 0.0;

            // Reset AI state so NPC doesn't resume mid-attack
            npc->ai_state = 0;          // NPC_AI_IDLE
            npc->ai_target_id = 0;
            npc->ai_is_casting = 0;
            npc->ai_cast_ability_idx = -1;
            npc->ai_cast_start = 0.0;
            for (int c = 0; c < MAX_NPC_ABILITIES_RT; c++) {
                npc->ai_ability_cooldowns[c] = 0.0;
            }
            npc->ai_cd_seeded = 0; // Re-seed phases on next npc_ai_tick entry

            printf("[COMBAT] NPC '%s' (id=%u) respawned at (%.1f, %.1f)\n",
                   npc->name, npc->id, npc->spawn_x, npc->spawn_y);
        }
    }
    pthread_mutex_unlock(&world->lock);
}