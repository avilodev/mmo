/**
 * @file
 * Load basic-attack profiles and manage NPC combat, cast resolution, regeneration, death, and respawn.
 */

#include "combat.h"
#include "npc_snapshot.h"
#include "npc_query.h"
#include "npc_world.h"
#include "npc_mitigation.h"
#include "npc_effects.h"
#include "player_effects.h"
#include "npc_triggers.h"
#include "player_cast_flags.h"
#include "log.h"
#include "combat_stats.h"
#include "player_level.h"
#include "player_data.h"
#include "party.h"
#include "loot.h"
#include "quest_system.h"
#include "projectile.h"
#include "world_regions.h"
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

/** One resolved hit, collected under lock and sent after the lock is dropped. */
typedef struct {
    uint32_t target_id;
    uint32_t damage;
    uint32_t new_health;
    uint8_t  is_kill;
    uint8_t  is_crit;
    uint64_t xp_reward;
    uint16_t npc_type_id;   // For loot roll on kill
    float    npc_x, npc_y;  // NPC position for loot drop
} HitResult;

/* Cast-resolution scratch, sized to the NPC pool.
 *
 * Both of these used to be fixed stack arrays, and the pair of them hid a bug in
 * each other. candidates[] was 64 with a comment explaining that 64 left room
 * for shape-test rejections above MAX_CAST_TARGETS -- which was true, and
 * irrelevant, because hits[] was 16 and the resolve loop broke out at it. No
 * attack in this build has ever damaged more than 16 NPCs, whatever the query
 * returned, and nothing said so. Neither number is a wire cap: damage goes out
 * as one DamageV2Packet per hit, not packed into a bounded array.
 *
 * The producer for both is the NPC pool: the grid can legitimately return every
 * NPC in the world, and an AoE can legitimately damage every NPC it returns.
 * File-scope rather than stack because at NPC_CAPACITY_MAX these are megabytes,
 * and file-scope is safe for the same reason expired[] below is -- combat_tick
 * runs only on the single gameplay thread.
 */
static int*       g_cast_candidates;
static HitResult* g_cast_hits;
static int        g_cast_scratch_capacity;

/**
 * Grow the cast-resolution scratch to hold one entry per NPC slot.
 *
 * Allocation happens on the first tick and never again: the NPC pool is sized
 * once at startup and never grows.
 *
 * @param capacity  Required entry count, normally npc_world_capacity().
 * @return          Entries the scratch can actually hold, which is less than
 *                  `capacity` only when allocation failed.
 */
static int combat_scratch_reserve(int capacity) {
    if (capacity <= g_cast_scratch_capacity) return g_cast_scratch_capacity;

    int* candidates = realloc(g_cast_candidates, (size_t)capacity * sizeof(*candidates));
    if (!candidates) {
        LOG_ERROR("[COMBAT] could not size cast scratch to %d candidates; "
                  "resolution is capped at %d this tick",
                  capacity, g_cast_scratch_capacity);
        return g_cast_scratch_capacity;
    }
    g_cast_candidates = candidates;

    HitResult* hits = realloc(g_cast_hits, (size_t)capacity * sizeof(*hits));
    if (!hits) {
        LOG_ERROR("[COMBAT] could not size cast scratch to %d hits; "
                  "resolution is capped at %d this tick",
                  capacity, g_cast_scratch_capacity);
        return g_cast_scratch_capacity;
    }
    g_cast_hits = hits;

    g_cast_scratch_capacity = capacity;
    return g_cast_scratch_capacity;
}

/** Widen a query radius to cover NPCs whose body is in range but whose centre is not. */
static float npc_search_margin(const NPCTickSnapshot* npcs) {
    return npcs ? npcs->max_hitbox_radius : 0.0f;
}

static PendingCast g_pending_casts[MAX_PLAYERS];
static pthread_mutex_t g_pending_casts_lock = PTHREAD_MUTEX_INITIALIZER;

/**
 * Report whether a slot is free to start a new cast for a character.
 *
 * A cast left behind by a previous occupant of the slot does not block the current
 * one; it is stale and will be overwritten. Only the caller's own in-flight cast
 * counts as busy.
 *
 * @return 1 when a new cast may be started, or 0 when one is already in flight.
 */
static int pending_cast_slot_is_free(int slot, uint32_t character_id) {
    pthread_mutex_lock(&g_pending_casts_lock);
    int busy = (g_pending_casts[slot].is_active &&
                g_pending_casts[slot].character_id == character_id);
    pthread_mutex_unlock(&g_pending_casts_lock);
    return !busy;
}

/**
 * Publish a fully resolved cast into a slot.
 *
 * The whole record is written under the lock, so a drain never observes a half-filled
 * cast. The busy check is repeated here because target resolution runs unlocked.
 *
 * @return 1 when the cast was stored, or 0 when another cast won the slot first.
 */
static int pending_cast_commit(int slot, uint32_t character_id, const PendingCast* src) {
    pthread_mutex_lock(&g_pending_casts_lock);
    if (g_pending_casts[slot].is_active &&
        g_pending_casts[slot].character_id == character_id) {
        pthread_mutex_unlock(&g_pending_casts_lock);
        return 0;
    }
    g_pending_casts[slot] = *src;
    g_pending_casts[slot].character_id = character_id;
    g_pending_casts[slot].is_active    = 1;
    pthread_mutex_unlock(&g_pending_casts_lock);
    return 1;
}

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
    /* Resolve the slot BEFORE acquiring the player, not after.
     *
     * combat_find_player_slot() takes the player registry's read lock, and
     * player_acquire() returns holding that player's slot mutex. Doing the
     * lookup second meant taking the registry lock while holding a slot mutex —
     * the exact reverse of player_add_active(), which takes the registry write
     * lock and then the slot mutex. A login landing between the two halves of
     * an attack deadlocked the loop thread against the auth worker.
     *
     * Both calls release their own lock before returning, so ordering them this
     * way means the two are never held at once. ThreadSanitizer flags the
     * original ordering; see `make net-loop-sanitize`.
     */
    int player_slot = combat_find_player_slot(attacker_id);
    if (player_slot < 0) {
        LOG_DEBUG("[COMBAT] Attack intent from offline attacker %u", attacker_id);
        return;
    }

    ActivePlayer* attacker = player_acquire_slot(player_slot, attacker_id);
    if (!attacker) {
        LOG_DEBUG("[COMBAT] Attack intent from unknown attacker %u", attacker_id);
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

    /* Nothing clamps to a fixed race count: an unknown race resolves to the fallback
     * profile rather than being silently rewritten as race 1. */
    const RaceAttackProfile* profile = combat_profile_for_race(attacker->race_id);

    float origin_x = attacker->pos_x;
    float origin_y = attacker->pos_y;
    float aim_x    = pkt->aim_x;
    float aim_y    = pkt->aim_y;

    /* Blind scatters aim rather than preventing the attack: the swing still
     * happens, it just may not go where it was pointed. Applied to the
     * snapshotted aim, so what resolves is what the blind spoiled -- and applied
     * server-side, because a client that scattered its own aim would simply not
     * bother. */
    float blind = player_blind_spread(attacker);
    if (blind > 0.0f) {
        float ax = aim_x - origin_x, ay = aim_y - origin_y;
        float r = ((float)(rand() % 2001) / 1000.0f - 1.0f) * blind *
                  (float)(M_PI / 180.0);
        float c = cosf(r), sn = sinf(r);
        aim_x = origin_x + ax * c - ay * sn;
        aim_y = origin_y + ax * sn + ay * c;
    }

    uint32_t charm_source = player_charm_source(attacker);

    // Mark combat timestamp for HP regen suppression
    attacker->last_combat_time = now;

    /* Release the slot before touching g_pending_casts_lock. This is a lock-order
     * rule, not a tidiness one: combat_tick holds no slot lock while it drains the
     * pending casts, and this path must hold no slot lock while it takes the pending
     * lock. Taking them in opposite orders froze the gameplay thread against an
     * epoll loop for good. */
    player_release(attacker);
    attacker = NULL;

    if (!pending_cast_slot_is_free(player_slot, attacker_id)) {
        AttackResultPacket result = {0};
        result.header.type       = PACKET_ATTACK_RESULT;
        result.header.player_id  = htonl(attacker_id);
        result.result_code       = ATTACK_RESULT_ALREADY_CASTING;
        server_send(client_fd, &result, sizeof(result));
        return;
    }

    // resolve targets
    uint32_t hit_targets[MAX_CAST_TARGETS];
    uint8_t  hit_count = 0;

    float aim_dx = aim_x - origin_x;
    float aim_dy = aim_y - origin_y;
    float aim_len = sqrtf(aim_dx * aim_dx + aim_dy * aim_dy);
    float dir_x = (aim_len > 0.001f) ? aim_dx / aim_len : 1.0f;
    float dir_y = (aim_len > 0.001f) ? aim_dy / aim_len : 0.0f;

    float cone_half_rad = profile->cone_half_angle * (M_PI / 180.0f);
    float line_half_w   = profile->line_width / 2.0f;

    /* Query the shared spatial index rather than scanning the pool.
     *
     * This ran on a network loop thread and walked every slot in the NPC pool,
     * taking each NPC's mutex in turn, for every attack packet -- so the cost
     * of one player attacking scaled with how many NPCs the world holds, not
     * with how many are near them, and it was paid at whatever rate players
     * choose to attack. The gameplay thread publishes an index once per tick
     * (npc_query.h); this reads it under a read lock with its own scratch.
     *
     * The index is a copy taken at the top of a tick, which is the right thing
     * to reject candidates with and the wrong thing to damage. Survivors are
     * resolved through the pool by the caller, exactly as before.
     *
     * The search radius is widened by the largest hitbox in the index: a shape
     * test is against an NPC's edge, but the index holds its centre. */
    float search_radius = profile->range + npc_query_max_hitbox_radius();

    /* A line or cone reaches `range` along its axis, so a circle of `range`
     * around the origin already contains everything either can hit. */
    int npc_capacity = npc_world_capacity(world);
    int max_hits = npc_capacity > 0 ? npc_capacity : 1;

    NpcQueryHit* candidates = malloc(sizeof(*candidates) * (size_t)max_hits);
    if (!candidates) {
        LOG_ERROR("[COMBAT] out of memory resolving attack targets");
        AttackResultPacket result = {0};
        result.header.type       = PACKET_ATTACK_RESULT;
        result.header.player_id  = htonl(attacker_id);
        result.result_code       = ATTACK_RESULT_NO_TARGETS;
        server_send(client_fd, &result, sizeof(result));
        return;
    }

    int candidate_count = npc_query_near(origin_x, origin_y, search_radius,
                                         candidates, max_hits);

    /* A charmed player cannot bring themselves to strike the charmer's own kind.
     * That is what charm can honestly mean in a game whose client drives its own
     * movement: not "you now walk over there", but "these are your friends and
     * you will not hit them". Server-authoritative and explainable in a sentence.
     *
     * Resolved once, here, rather than per candidate -- the charmer's faction is
     * a property of the charm, not of who is standing nearby. */
    int charmed_faction = -1;
    if (charm_source) {
        NpcQueryHit charmer;
        if (npc_query_lookup(charm_source, &charmer))
            charmed_faction = npc_faction_of_type(charmer.npc_type_id);
    }

    uint32_t best_single_id   = 0;
    float    best_single_dist = 1e9f;

    for (int i = 0; i < candidate_count && hit_count < MAX_CAST_TARGETS; i++) {
        const NpcQueryHit* hit = &candidates[i];
        if (charmed_faction >= 0 &&
            npc_faction_of_type(hit->npc_type_id) == charmed_faction) continue;
        float npc_cx = hit->pos_x;
        float npc_cy = hit->pos_y;

        switch (profile->attack_type) {
            case ATTACK_TYPE_SINGLE: {
                float d = combat_dist(origin_x, origin_y, npc_cx, npc_cy);
                if (d <= profile->range + hit->hitbox_radius && d < best_single_dist) {
                    best_single_dist = d;
                    best_single_id   = hit->id;
                }
                break;
            }
            case ATTACK_TYPE_AOE: {
                float d = combat_dist(origin_x, origin_y, npc_cx, npc_cy);
                if (d <= profile->range + hit->hitbox_radius) {
                    /* Nearest-first, because npc_query_near() returns them that
                     * way -- which is the ordering rule protocol.h states for
                     * MAX_CAST_TARGETS and the pool scan never honoured. */
                    hit_targets[hit_count++] = hit->id;
                }
                break;
            }
            case ATTACK_TYPE_CONE: {
                if (combat_point_in_cone(origin_x, origin_y, aim_x, aim_y,
                                         cone_half_rad, profile->range,
                                         npc_cx, npc_cy)) {
                    hit_targets[hit_count++] = hit->id;
                } else {
                    float d = combat_dist(origin_x, origin_y, npc_cx, npc_cy);
                    if (d <= profile->range + hit->hitbox_radius && d > 0.001f) {
                        float nx = npc_cx + (origin_x - npc_cx) / d * hit->hitbox_radius;
                        float ny = npc_cy + (origin_y - npc_cy) / d * hit->hitbox_radius;
                        if (combat_point_in_cone(origin_x, origin_y, aim_x, aim_y,
                                                 cone_half_rad, profile->range, nx, ny)) {
                            hit_targets[hit_count++] = hit->id;
                        }
                    }
                }
                break;
            }
            case ATTACK_TYPE_LINE: {
                if (combat_point_in_line(origin_x, origin_y, dir_x, dir_y,
                                         profile->range, line_half_w,
                                         npc_cx, npc_cy)) {
                    hit_targets[hit_count++] = hit->id;
                } else {
                    float d = combat_dist(origin_x, origin_y, npc_cx, npc_cy);
                    if (d <= profile->range + hit->hitbox_radius && d > 0.001f) {
                        float e1x = npc_cx + (-dir_y) * hit->hitbox_radius;
                        float e1y = npc_cy +  dir_x  * hit->hitbox_radius;
                        float e2x = npc_cx - (-dir_y) * hit->hitbox_radius;
                        float e2y = npc_cy -  dir_x  * hit->hitbox_radius;
                        if (combat_point_in_line(origin_x, origin_y, dir_x, dir_y,
                                                 profile->range, line_half_w, e1x, e1y) ||
                            combat_point_in_line(origin_x, origin_y, dir_x, dir_y,
                                                 profile->range, line_half_w, e2x, e2y)) {
                            hit_targets[hit_count++] = hit->id;
                        }
                    }
                }
                break;
            }
        }
    }

    free(candidates);

    if (profile->attack_type == ATTACK_TYPE_SINGLE && best_single_id != 0) {
        hit_targets[0] = best_single_id;
        hit_count = 1;
    }

    if (hit_count == 0) {
        AttackResultPacket result = {0};
        result.header.type       = PACKET_ATTACK_RESULT;
        result.header.player_id  = htonl(attacker_id);
        result.result_code       = ATTACK_RESULT_NO_TARGETS;
        server_send(client_fd, &result, sizeof(result));
        return;
    }

    PendingCast staged;
    memset(&staged, 0, sizeof(staged));
    PendingCast* cast = &staged;
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

    if (!pending_cast_commit(player_slot, attacker_id, &staged)) {
        AttackResultPacket result = {0};
        result.header.type       = PACKET_ATTACK_RESULT;
        result.header.player_id  = htonl(attacker_id);
        result.result_code       = ATTACK_RESULT_ALREADY_CASTING;
        server_send(client_fd, &result, sizeof(result));
        return;
    }

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
    /* Only cancel a cast this character actually owns. A slot recycled from a
     * disconnected player can still hold their cast. */
    if (g_pending_casts[slot].is_active &&
        g_pending_casts[slot].character_id == attacker_id) {
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
void combat_tick(NPCWorld* world, NPCTickSnapshot* npcs) {
    extern ActivePlayer active_players[];
    double now = combat_get_time();

    /* Every bounded query and every hit list below is capped at this, and this
     * is the NPC pool -- so none of them can truncate a real result. */
    const int scratch_cap = combat_scratch_reserve(npc_world_capacity(world));

    /* Drain every expired cast under g_pending_casts_lock, then release it before
     * resolving any of them.
     *
     * Resolution acquires player slot locks, world->lock, and reaches into
     * party_award_xp, loot_roll, and quest_on_npc_kill (which does file I/O). Holding
     * the pending lock across all of that both serialised every attack packet in the
     * world behind this loop and deadlocked against combat_handle_attack_intent, which
     * takes the same two locks in the opposite order.
     *
     * Owned by the single gameplay thread that calls combat_tick, like the tick
     * snapshot in combat_update_thread, so it is static rather than 90 KB of stack. */
    static PendingCast expired[MAX_PLAYERS];
    static int         expired_slot[MAX_PLAYERS];
    int expired_count = 0;

    pthread_mutex_lock(&g_pending_casts_lock);

    /* Republish who is mid-cast for the NPC trigger evaluator, from the array
     * that actually knows. Once per tick from the source rather than a flag set
     * at each of the places a cast starts, cancels or resolves: a missed clear is
     * impossible when the clear is a fresh read.
     *
     * This is the authoritative write of the tick and clears as well as sets;
     * ability_tick() runs after it and adds ability casts on top. Both finish
     * before npc_ai_tick() reads them. See player_cast_flags.h. */
    for (int i = 0; i < MAX_PLAYERS; i++)
        player_cast_flag_set(i, g_pending_casts[i].is_active);

    for (int i = 0; i < MAX_PLAYERS; i++) {
        PendingCast* pending = &g_pending_casts[i];
        if (!pending->is_active) continue;
        if (now - pending->cast_start_time < pending->cast_duration) continue;

        pending->is_active = 0;
        expired[expired_count]      = *pending;
        expired_slot[expired_count] = i;
        expired_count++;
    }
    pthread_mutex_unlock(&g_pending_casts_lock);

    for (int e = 0; e < expired_count; e++) {
        const PendingCast* cast = &expired[e];
        uint32_t attacker_id    = cast->character_id;

        /* Confirm the caster is still the one in this slot. A player who logged out
         * mid-cast leaves the cast behind; without this check it would resolve as
         * whoever took the slot next, crediting them with the damage, XP, loot, and
         * quest kill. Reading client_fd here also happens under the slot lock, where
         * every other reader in the codebase takes it. */
        ActivePlayer* attacker = player_acquire_slot(expired_slot[e], attacker_id);
        if (!attacker) continue;

        int client_fd = attacker->client_fd;

        attacker->last_attack_time = now;
        attacker->attack_cooldown  = cast->cooldown;
        attacker->last_combat_time = now;

        int a_stats[STAT_COUNT];
        memcpy(a_stats, attacker->stats, sizeof(a_stats));
        int a_wpn = attacker->weapon_damage;

        DamageModifiers a_mods;
        player_collect_modifiers(attacker, 0, &a_mods);
        float a_form_power = player_form_power(attacker);
        player_release(attacker);

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
        HitResult* hits = g_cast_hits;
        int hit_count = 0;

        /* Candidates come from this tick's NPC grid rather than a scan of the
         * whole pool.
         *
         * Two widenings, both required or the query silently drops real targets:
         * the largest hitbox in the snapshot, because the grid indexes centres
         * while the shape tests are against edges; and, for a line, half its
         * width, because a line reaches sqrt(range^2 + half_width^2) from the
         * origin at its far corners rather than just `range`. line_half_w is
         * zero for every other attack type, so this costs them nothing. */
        const float search_radius = range + line_half_w + npc_search_margin(npcs);

        // resolve the closest single target
        if (attack_type == ATTACK_TYPE_SINGLE) {
            int* candidates = g_cast_candidates;
            int candidate_count = npc_snapshot_query(npcs, origin_x, origin_y,
                                                     search_radius, candidates,
                                                     scratch_cap);

            /* Nearest-first, so the first candidate whose own hitbox brings it
             * into range is also the closest such NPC — the same target the old
             * full scan picked, without visiting anything further away. */
            for (int k = 0; k < candidate_count; k++) {
                int c = candidates[k];
                float d = combat_dist(origin_x, origin_y, npcs->pos_x[c], npcs->pos_y[c]);
                if (d > range + npcs->hitbox_radius[c]) continue;

                NPCEntity* best = npc_world_acquire_slot(world, npcs->slot[c], npcs->id[c]);
                if (!best) continue;              // died or was recycled this tick
                if (!best->is_alive) { npc_world_release(world, best); continue; }

                /* An attack that reaches its target connects. There is no dodge roll. */
                uint8_t is_crit = 0;
                int damage = compute_final_damage(cast->base_damage + a_wpn,
                                                   cast->damage_variance,
                                                   a_stats, cast->damage_stat,
                                                   a_form_power, &a_mods,
                                                   best->armor, &is_crit);

                damage = npc_mitigation_apply(world, npcs->slot[c], best, damage,
                                              origin_x, origin_y, now, NULL);

                best->health -= damage;
                if (best->health < 0) best->health = 0;
                npc_trigger_note_damage(best, origin_x, origin_y);

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
                hits[hit_count].npc_type_id = best->npc_type_id;
                hits[hit_count].npc_x       = best->pos_x;
                hits[hit_count].npc_y       = best->pos_y;
                hit_count++;

                LOG_DEBUG("[COMBAT] %u hit NPC %u (%s) for %d dmg%s (hp=%d)%s", attacker_id, best->id, best->name, damage, is_crit ? " (CRIT)" : "", best->health, is_kill ? " — KILLED" : "");

                npc_world_release(world, best);
                break;
            }

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
                    if (hits[h].xp_reward > 0) {
                        ActivePlayer* killer = player_acquire(attacker_id);
                        if (killer) {
                            player_send_kill_reward_locked(client_fd, killer,
                                                           (uint32_t)hits[h].xp_reward);
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
        int* candidates = g_cast_candidates;
        int candidate_count = npc_snapshot_query(npcs, origin_x, origin_y,
                                                 search_radius, candidates,
                                                 scratch_cap);

        for (int k = 0; k < candidate_count; k++) {
            int c = candidates[k];

            /* Shape tests run against the snapshot's copy. They only decide
             * whether an NPC is worth locking, and positions do not move within
             * a tick — the damage below reads live health and armor. */
            int hit = 0;
            float npc_cx  = npcs->pos_x[c];
            float npc_cy  = npcs->pos_y[c];
            float npc_hit = npcs->hitbox_radius[c];

            switch (attack_type) {
                case ATTACK_TYPE_AOE: {
                    float d = combat_dist(origin_x, origin_y, npc_cx, npc_cy);
                    hit = (d <= range + npc_hit);
                    break;
                }
                case ATTACK_TYPE_CONE: {
                    hit = combat_point_in_cone(origin_x, origin_y,
                                               aim_x, aim_y,
                                               cone_half_rad, range,
                                               npc_cx, npc_cy);
                    if (!hit) {
                        float d = combat_dist(origin_x, origin_y, npc_cx, npc_cy);
                        if (d <= range + npc_hit && d > 0.001f) {
                            float nx = npc_cx + (origin_x - npc_cx) / d * npc_hit;
                            float ny = npc_cy + (origin_y - npc_cy) / d * npc_hit;
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
                        if (d <= range + npc_hit && d > 0.001f) {
                            float e1x = npc_cx + (-dir_y) * npc_hit;
                            float e1y = npc_cy +  dir_x  * npc_hit;
                            float e2x = npc_cx - (-dir_y) * npc_hit;
                            float e2y = npc_cy -  dir_x  * npc_hit;
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
            /* Cannot trigger: hits[] and candidates[] are both scratch_cap
             * entries and this loop runs at most candidate_count times. Kept as
             * an assertion against a future caller that sizes them differently. */
            if (hit_count >= scratch_cap) break;

            NPCEntity* npc = npc_world_acquire_slot(world, npcs->slot[c], npcs->id[c]);
            if (!npc) continue;
            if (!npc->is_alive) { npc_world_release(world, npc); continue; }

            uint8_t is_crit = 0;
            int damage = compute_final_damage(cast->base_damage + a_wpn, cast->damage_variance,
                                               a_stats, cast->damage_stat,
                                               a_form_power, &a_mods,
                                               npc->armor, &is_crit);

            damage = npc_mitigation_apply(world, npcs->slot[c], npc, damage,
                                          origin_x, origin_y, now, NULL);

            npc->health -= damage;
            if (npc->health < 0) npc->health = 0;
            npc_trigger_note_damage(npc, origin_x, origin_y);

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
            hits[hit_count].npc_type_id = npc->npc_type_id;
            hits[hit_count].npc_x       = npc->pos_x;
            hits[hit_count].npc_y       = npc->pos_y;
            hit_count++;

            LOG_DEBUG("[COMBAT] %u hit NPC %u (%s) for %d dmg%s (hp=%d)%s", attacker_id, npc->id, npc->name, damage, is_crit ? " (CRIT)" : "", npc->health, is_kill ? " — KILLED" : "");

            npc_world_release(world, npc);
        }

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
                if (hits[h].xp_reward > 0) {
                    ActivePlayer* killer = player_acquire(attacker_id);
                    if (killer) {
                        player_send_kill_reward_locked(client_fd, killer,
                                                       (uint32_t)hits[h].xp_reward);
                        player_release(killer);
                    }
                }
                loot_roll(hits[h].npc_type_id, hits[h].npc_x, hits[h].npc_y, attacker_id);
            }
        }
    }

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
        typedef struct {
            uint32_t player_id;
            int      client_fd;
        } DeathEvent;

        /* One slot per player, because one tick can kill every player online.
         *
         * This was [16] with the record guarded by `if (death_count < 16)` while
         * the kill itself -- is_dead, death_time, health -- ran unconditionally.
         * Past the 16th death in a tick the server considered a player dead and
         * never told them: their client kept them upright, accepting input, in a
         * world that had stopped acknowledging it. Sixteen is well inside one
         * AoE wipe or one contested spawn, and deaths in a tick are correlated
         * rather than spread out, which is exactly the case that overran it.
         *
         * Static for the same reason expired[] is: gameplay thread only, and
         * this is 8 KB that does not belong on a thread stack. */
        static DeathEvent deaths[MAX_PLAYERS];
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

                deaths[death_count].player_id = active_players[i].character_id;
                deaths[death_count].client_fd = active_players[i].client_fd;
                death_count++;

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

        /* The world bible sends a dead player to their house, or to the nearest
         * known city when they have none. Housing does not exist yet, so every
         * respawn resolves to the nearest capital from where the player died. */
        typedef struct {
            uint32_t player_id;
            int      client_fd;
            int32_t  health;
            int32_t  max_health;
            int32_t  mana;
            int32_t  max_mana;
            float    pos_x;
            float    pos_y;
        } RespawnEvent;

        /* One slot per player, for the same reason deaths[] above is: the
         * respawn moved the player and restored their health whether or not
         * there was room to record it, so past the 16th respawn in a tick the
         * server teleported someone across the map and left their client
         * standing at the corpse with the old health bar. Deaths cluster, so
         * respawns cluster three seconds later. */
        static RespawnEvent respawns[MAX_PLAYERS];
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
                const WorldCity* home =
                    world_nearest_city_px(active_players[i].pos_x,
                                          active_players[i].pos_y);
                float respawn_x = 0.0f, respawn_y = 0.0f;
                world_city_center_px(home, &respawn_x, &respawn_y);

                active_players[i].pos_x = respawn_x;
                active_players[i].pos_y = respawn_y;
                active_players[i].is_dirty = 1;

                respawns[respawn_count].player_id   = active_players[i].character_id;
                respawns[respawn_count].client_fd   = active_players[i].client_fd;
                respawns[respawn_count].health      = active_players[i].health;
                respawns[respawn_count].max_health  = active_players[i].max_health;
                respawns[respawn_count].mana        = active_players[i].resource;
                respawns[respawn_count].max_mana    = active_players[i].max_resource;
                respawns[respawn_count].pos_x       = respawn_x;
                respawns[respawn_count].pos_y       = respawn_y;
                respawn_count++;

                LOG_DEBUG("[COMBAT] Player %u respawned in %s at (%.0f, %.0f)",
                          active_players[i].character_id, home->city_name,
                          respawn_x, respawn_y);
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
            pkt.pos_x = respawns[r].pos_x;
            pkt.pos_y = respawns[r].pos_y;
            pkt.health = htonl((uint32_t)respawns[r].health);
            pkt.max_health = htonl((uint32_t)respawns[r].max_health);
            pkt.mana = htonl((uint32_t)respawns[r].mana);
            pkt.max_mana = htonl((uint32_t)respawns[r].max_mana);
            server_send(respawns[r].client_fd, &pkt, sizeof(pkt));
        }
    }

    /* Respawn eligible NPCs.
     *
     * This one genuinely has to consider every NPC — a corpse anywhere in the
     * world can come due — so it iterates rather than queries. It holds only
     * the pool read lock plus one slot at a time, so a large pool no longer
     * blocks combat for the length of the sweep, and it walks the occupied-slot
     * list rather than the pool, so an empty slot costs nothing. */
    npc_world_read_begin(world);

    int live_count = 0;
    const int* live = npc_world_live_slots(world, &live_count);

    for (int n = 0; n < live_count; n++) {
        int i = live[n];
        NPCEntity* npc = npc_world_slot(world, i);
        if (!npc || npc->id == 0) continue;   // unlocked pre-filter only

        npc_world_slot_lock(world, i);

        if (npc->id == 0 || npc->is_alive || npc->respawn_time <= 0.0f) {
            npc_world_slot_unlock(world, i);
            continue;
        }

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
            /* Clears cooldowns, and with them any effects and fired trigger
             * latches the NPC died holding -- a respawn is a fresh enemy, not a
             * resumed one, and that state no longer lives inside NPCEntity for
             * the memset above to have caught. */
            npc_world_clear_state(world, i);
            npc->ai_cd_seeded = 0; // Re-seed phases on next npc_ai_tick entry

            LOG_DEBUG("[COMBAT] NPC '%s' (id=%u) respawned at (%.1f, %.1f)", npc->name, npc->id, npc->spawn_x, npc->spawn_y);
        }

        npc_world_slot_unlock(world, i);
    }
    npc_world_read_end(world);
}
