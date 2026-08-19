/**
 * @file
 * Load basic-attack profiles and manage NPC combat, cast resolution, regeneration, death, and respawn.
 */

#include "combat.h"
#include "log.h"
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
#include "json_util.h"
#include "race_registry.h"
#include "player_effects.h"

/** Hold one profile per race, filled from attack_profiles.json at startup.
 *
 * There is no compiled table of profiles any more. A race with no entry in the file
 * gets the fallback below, which is deliberately unremarkable rather than absent — a
 * race without a basic attack could not fight at all.
 */
RaceAttackProfile g_race_profiles[MAX_RACES + 1];

/** Supply a plain melee attack for any race the data file does not describe. */
static const RaceAttackProfile k_fallback_profile = {
    .cast_time       = 0.6f,
    .cooldown        = 0.8f,
    .range           = 55.0f,
    .base_damage     = 12,
    .damage_variance = 15,
    .attack_type     = 0,          /* SINGLE */
    .cone_half_angle = 0.0f,
    .line_width      = 0.0f,
    .is_ranged       = 0,
    .damage_stat     = STAT_STRENGTH,
};

/**
 * Return the attack profile for a race, falling back for an unknown one.
 *
 * @return A profile that is always safe to read.
 */
const RaceAttackProfile* combat_profile_for_race(uint32_t race_id) {
    if (race_id >= 1 && race_id <= MAX_RACES && g_race_profiles[race_id].is_loaded) {
        return &g_race_profiles[race_id];
    }
    return &k_fallback_profile;
}

/**
 * Resolve an attack type name to its wire value.
 *
 * @return The AttackType, or 0 (single target) when the name is unrecognised.
 */
static uint8_t parse_attack_type(const char* name) {
    if (!name) return 0;
    if (strcmp(name, "single") == 0) return 0;
    if (strcmp(name, "aoe") == 0)    return 1;
    if (strcmp(name, "cone") == 0)   return 2;
    if (strcmp(name, "line") == 0)   return 3;
    return 0;
}

/**
 * Resolve a damage school name to its wire value.
 */
static uint8_t parse_projectile_damage_type(const char* name) {
    if (!name) return 0;
    if (strcmp(name, "earth") == 0)  return 1;
    if (strcmp(name, "spirit") == 0) return 2;
    return 0;
}

/**
 * Load one basic-attack profile per race from JSON.
 *
 * Profiles name their race by key, so adding a race's basic attack is one more entry
 * in the file. Races the file does not mention keep the fallback profile.
 *
 * @return The number of profiles loaded, or 0 on failure.
 */
int combat_profiles_load(const char* path) {
    memset(g_race_profiles, 0, sizeof(g_race_profiles));

    const char* err = NULL;
    JsonValue* root = json_parse_file(path, &err);
    if (!root) {
        LOG_ERROR("[COMBAT] %s: %s — every race falls back to a plain melee attack",
                  path ? path : "(no path)", err ? err : "unreadable");
        return 0;
    }

    const JsonValue* profiles = json_get(root, "profiles");
    int listed = json_count(profiles);
    int loaded = 0;

    for (int i = 0; i < listed; i++) {
        const JsonValue* obj = json_at(profiles, i);

        const char* race_key = json_get_string(obj, "race", NULL);
        const RaceDef* race = race_get_by_key(race_key);
        if (!race) {
            LOG_ERROR("[COMBAT] attack profile names unknown race \"%s\"; ignored",
                      race_key ? race_key : "(missing)");
            continue;
        }

        RaceAttackProfile* profile = &g_race_profiles[race->id];
        *profile = k_fallback_profile;

        profile->base_damage     = json_get_int(obj, "base_damage", profile->base_damage);
        profile->damage_variance = json_get_int(obj, "damage_variance", profile->damage_variance);
        profile->cast_time       = (float)json_get_number(obj, "cast_time", profile->cast_time);
        profile->cooldown        = (float)json_get_number(obj, "cooldown", profile->cooldown);
        profile->range           = (float)json_get_number(obj, "range", profile->range);
        profile->attack_type     = parse_attack_type(json_get_string(obj, "attack_type", "single"));
        profile->cone_half_angle = (float)json_get_number(obj, "cone_half_angle", 0.0);
        profile->line_width      = (float)json_get_number(obj, "line_width", 0.0);
        profile->is_ranged       = (uint8_t)json_get_bool(obj, "is_ranged", 0);
        profile->projectile_speed = (float)json_get_number(obj, "projectile_speed", 0.0);
        profile->projectile_width = (float)json_get_number(obj, "projectile_width", 0.0);
        profile->projectile_damage_type =
            parse_projectile_damage_type(json_get_string(obj, "projectile_damage_type", NULL));

        int stat = stat_from_key(json_get_string(obj, "damage_stat", "strength"));
        profile->damage_stat = (uint8_t)(stat >= 0 ? stat : STAT_STRENGTH);
        profile->projectile_damage_stat = profile->damage_stat;

        profile->is_loaded = 1;
        loaded++;

        LOG_INFO("[COMBAT] %s basic attack: dmg=%d +-%d%% cast=%.2fs cd=%.2fs range=%.0f%s",
                 race->key, profile->base_damage, profile->damage_variance,
                 profile->cast_time, profile->cooldown, profile->range,
                 profile->is_ranged ? " [RANGED]" : "");
    }

    json_free(root);
    LOG_INFO("[COMBAT] %d attack profile(s) loaded from %s", loaded, path);
    return loaded;
}

static double combat_get_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static float combat_dist(float ax, float ay, float bx, float by) {
    float dx = bx - ax;
    float dy = by - ay;
    return sqrtf(dx * dx + dy * dy);
}

/**
 * Test whether a point lies within an aimed cone.
 *
 * @return 1 when inside, or 0 otherwise.
 */
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

/**
 * Test whether a point lies within an oriented line rectangle.
 *
 * @return 1 when inside, or 0 otherwise.
 */
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

static uint32_t g_next_npc_id = 1000;

/**
 * Initialize an NPC world and its mutex.
 */
void combat_npc_init(NPCWorld* world) {
    memset(world->npcs, 0, sizeof(world->npcs));
    world->count = 0;
    pthread_mutex_init(&world->lock, NULL);
    LOG_INFO("[COMBAT] NPC world initialized");
}

/**
 * Spawn an NPC in a free or reclaimable world slot.
 *
 * @param world            NPC world receiving the entity.
 * @param name             Terminated NPC display name.
 * @param x                Spawn X coordinate in world units.
 * @param y                Spawn Y coordinate in world units.
 * @param health           Initial and maximum health.
 * @param hitbox_radius    Collision radius in world units.
 * @param dialogue_id      Associated dialogue identifier, or zero.
 * @param is_interactable  Nonzero when client interaction is allowed.
 * @param npc_type_id      Content type used for AI, loot, and quests.
 * @param respawn_time     Respawn delay in seconds; nonpositive disables respawn.
 * @param category         NPC_CATEGORY_* value controlling default rewards.
 * @return                 The assigned identifier, or 0 when the pool is full.
 */
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
        LOG_ERROR("[COMBAT] NPC pool full (%d/%d)", world->count, MAX_NPCS);
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
        LOG_ERROR("[COMBAT] No reclaimable NPC slot");
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
    npc->armor          = 0;
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
    LOG_DEBUG("[COMBAT] Spawned NPC '%s' id=%u at (%.1f, %.1f) hp=%d category=%s", name, id, x, y, health, cat_names[category < 3 ? category : 0]);
    return id;
}

/**
 * Find an NPC by identifier.
 *
 * The caller must hold world->lock while using the returned pool pointer.
 *
 * @return The matching entity, or NULL when absent.
 */
NPCEntity* combat_npc_find(NPCWorld* world, uint32_t npc_id) {
    for (int i = 0; i < MAX_NPCS; i++) {
        if (world->npcs[i].id == npc_id) {
            return &world->npcs[i];
        }
    }
    return NULL;
}

/**
 * Remove an NPC from the world by identifier.
 */
void combat_npc_remove(NPCWorld* world, uint32_t npc_id) {
    pthread_mutex_lock(&world->lock);
    for (int i = 0; i < MAX_NPCS; i++) {
        if (world->npcs[i].id == npc_id) {
            memset(&world->npcs[i], 0, sizeof(NPCEntity));
            LOG_DEBUG("[COMBAT] Removed NPC id=%u", npc_id);
            break;
        }
    }
    pthread_mutex_unlock(&world->lock);
}

static PendingCast g_pending_casts[MAX_PLAYERS];
static pthread_mutex_t g_pending_casts_lock = PTHREAD_MUTEX_INITIALIZER;

static int combat_find_player_slot(uint32_t character_id) {
    return player_slot_of(character_id);
}

/**
 * Calculate basic-attack damage after scaling, form power, crit, variance, and armor.
 *
 * @param attacker_stats  The attacker's attributes, indexed by StatId.
 * @param damage_stat     Which attribute the race's profile scales the attack with.
 * @param form_power      The Animal Form multiplier, or 1.0 in Human Form.
 * @param attacker_mods   The attacker's outgoing modifiers.
 * @param target_armor    The target's flat mitigation.
 * @param out_is_crit     Receives 1 when the attack critically struck; may be NULL.
 * @return                Final damage, never below one.
 */
static int compute_final_damage(int base_damage, int damage_variance,
                                const int* attacker_stats, uint8_t damage_stat,
                                float form_power,
                                const DamageModifiers* attacker_mods,
                                int target_armor,
                                uint8_t* out_is_crit) {
    float mult = combat_ability_damage_mult(damage_stat, attacker_stats);
    int damage = (int)((float)base_damage * mult * form_power);

    uint8_t is_crit = (uint8_t)combat_check_crit(attacker_stats[STAT_PRECISION]);
    if (is_crit) {
        damage = (int)((float)damage * combat_crit_multiplier(attacker_stats[STAT_FEROCITY]));
    }
    if (out_is_crit) *out_is_crit = is_crit;

    if (damage_variance > 0) {
        int variance = (rand() % (damage_variance * 2 + 1)) - damage_variance;
        damage += (damage * variance) / 100;
    }

    DamageModifiers target_mods;
    damage_mods_reset(&target_mods);
    damage_mods_add_armor(&target_mods, target_armor);

    return damage_resolve(damage, attacker_mods, &target_mods);
}

/**
 * Validate a basic-attack intent and queue a cast with snapshot targeting data.
 *
 * @param world       NPC world searched for eligible targets.
 * @param client_fd   Socket receiving cast or rejection packets.
 * @param attacker_id Character issuing the attack.
 * @param pkt         Attack intent containing aim coordinates.
 */
void combat_handle_attack_intent(NPCWorld* world,
                                 int client_fd,
                                 uint32_t attacker_id,
                                 AttackIntentPacket* pkt) {
    ActivePlayer* attacker = player_acquire(attacker_id);
    if (!attacker) {
        LOG_DEBUG("[COMBAT] Attack intent from unknown attacker %u", attacker_id);
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

    /* Nothing clamps to a fixed race count: an unknown race resolves to the fallback
     * profile rather than being silently rewritten as race 1. */
    const RaceAttackProfile* profile = combat_profile_for_race(attacker->race_id);

    float origin_x = attacker->pos_x;
    float origin_y = attacker->pos_y;
    float aim_x    = pkt->aim_x;
    float aim_y    = pkt->aim_y;

    // Mark combat timestamp for HP regen suppression
    attacker->last_combat_time = now;

    player_release(attacker);

    // resolve targets
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
    cast->damage_stat            = profile->damage_stat;
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

    LOG_INFO("[COMBAT] Player %u cast started: type=%d, targets=%d, cast_time=%.2fs", attacker_id, profile->attack_type, hit_count, profile->cast_time);
}

/**
 * Cancel a character's pending basic-attack cast.
 */
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
        LOG_DEBUG("[COMBAT] Player %u cancelled cast", attacker_id);
    }
    pthread_mutex_unlock(&g_pending_casts_lock);
}

/**
 * Resolve pending attacks and advance regeneration, deaths, and respawns.
 */
void combat_tick(NPCWorld* world) {
    extern ActivePlayer active_players[];
    double now = combat_get_time();

    // resolve pending basic attacks
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

        int a_stats[STAT_COUNT];
        memcpy(a_stats, active_players[i].stats, sizeof(a_stats));
        int a_wpn = active_players[i].weapon_damage;

        DamageModifiers a_mods;
        player_collect_modifiers(&active_players[i], 0, &a_mods);
        float a_form_power = player_form_power(&active_players[i]);
        uint32_t a_race = active_players[i].race_id;
        pthread_mutex_unlock(&active_players[i].lock);
        (void)a_race;

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
            memcpy(info.caster_stats, a_stats, sizeof(info.caster_stats));
            info.damage_stat           = (int)cast->projectile_damage_stat;
            info.caster_mods           = a_mods;
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

        // resolve the closest single target
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
                /* An attack that reaches its target connects. There is no dodge roll. */
                {
                    uint8_t is_crit = 0;
                    int damage = compute_final_damage(cast->base_damage + a_wpn,
                                                       cast->damage_variance,
                                                       a_stats, cast->damage_stat,
                                                       a_form_power, &a_mods,
                                                       best->armor, &is_crit);

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

                    LOG_DEBUG("[COMBAT] %u hit NPC %u (%s) for %d dmg%s (hp=%d)%s", attacker_id, best->id, best->name, damage, is_crit ? " (CRIT)" : "", best->health, is_kill ? " — KILLED" : "");
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

        // resolve area targets
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

            uint8_t is_crit = 0;
            int damage = compute_final_damage(cast->base_damage + a_wpn, cast->damage_variance,
                                               a_stats, cast->damage_stat,
                                               a_form_power, &a_mods,
                                               npc->armor, &is_crit);

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

            LOG_DEBUG("[COMBAT] %u hit NPC %u (%s) for %d dmg%s (hp=%d)%s", attacker_id, npc->id, npc->name, damage, is_crit ? " (CRIT)" : "", npc->health, is_kill ? " — KILLED" : "");
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

    // regenerate out-of-combat health
    {
        static float hp_accum[MAX_PLAYERS];
        float dt = 0.05f;  // 20Hz tick

        player_registry_rdlock();
        int online_count = 0;
        const int* online = player_active_list_locked(&online_count);
        for (int n = 0; n < online_count; n++) {
            int i = online[n];
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

                    // clients interpolate regeneration between updates
                }
            } else {
                hp_accum[i] = 0.0f;  // Reset accumulator when in combat
            }

            pthread_mutex_unlock(&active_players[i].lock);
        }
        player_registry_unlock();
    }

    // snapshot newly dead players before sending
    {
        #define MAX_DEATH_EVENTS 16
        typedef struct {
            uint32_t player_id;
            int      client_fd;
        } DeathEvent;

        DeathEvent deaths[MAX_DEATH_EVENTS];
        int death_count = 0;

        player_registry_rdlock();
        int online_count = 0;
        const int* online = player_active_list_locked(&online_count);
        for (int n = 0; n < online_count; n++) {
            int i = online[n];
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

                LOG_DEBUG("[COMBAT] Player %u has died!", active_players[i].character_id);
            }

            pthread_mutex_unlock(&active_players[i].lock);
        }
        player_registry_unlock();

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

    // respawn players after the configured local delay
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

        player_registry_rdlock();
        int online_count = 0;
        const int* online = player_active_list_locked(&online_count);
        for (int n = 0; n < online_count; n++) {
            int i = online[n];
            if (!active_players[i].is_loaded) continue;
            pthread_mutex_lock(&active_players[i].lock);

            if (active_players[i].is_dead &&
                (now - active_players[i].death_time) >= RESPAWN_DELAY) {

                active_players[i].is_dead = 0;
                active_players[i].health = active_players[i].max_health;
                /* Rage builds through combat rather than being granted, so a respawn
                 * fills a mana or stamina pool but leaves a rage bar empty. */
                active_players[i].resource =
                    (active_players[i].resource_type == RESOURCE_RAGE)
                        ? 0 : active_players[i].max_resource;
                active_players[i].pos_x = RESPAWN_X;
                active_players[i].pos_y = RESPAWN_Y;
                active_players[i].is_dirty = 1;

                if (respawn_count < MAX_RESPAWN_EVENTS) {
                    respawns[respawn_count].player_id  = active_players[i].character_id;
                    respawns[respawn_count].client_fd   = active_players[i].client_fd;
                    respawns[respawn_count].health      = active_players[i].health;
                    respawns[respawn_count].max_health   = active_players[i].max_health;
                    respawns[respawn_count].mana        = active_players[i].resource;
                    respawns[respawn_count].max_mana     = active_players[i].max_resource;
                    respawn_count++;
                }

                LOG_DEBUG("[COMBAT] Player %u respawned at (%.0f, %.0f)", active_players[i].character_id, RESPAWN_X, RESPAWN_Y);
            }

            pthread_mutex_unlock(&active_players[i].lock);
        }
        player_registry_unlock();

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

    // respawn eligible NPCs
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

            LOG_DEBUG("[COMBAT] NPC '%s' (id=%u) respawned at (%.1f, %.1f)", npc->name, npc->id, npc->spawn_x, npc->spawn_y);
        }
    }
    pthread_mutex_unlock(&world->lock);
}
