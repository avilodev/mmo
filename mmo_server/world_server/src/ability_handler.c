// ============================================================================
// ability_handler.c — Server-side ability casting and resolution
// UPDATED: stat-scaled damage, defense, evasion, wisdom mana regen, XP on kill
// ============================================================================

#include "ability_handler.h"
#include "ability_def.h"
#include "combat.h"
#include "combat_stats.h"
#include "player_level.h"
#include "projectile.h"
#include "party.h"
#include "loot.h"

#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <time.h>

// ---------------------------------------------------------------------------
// Extern references
// ---------------------------------------------------------------------------

extern ActivePlayer active_players[];
extern pthread_mutex_t active_players_lock;

// ---------------------------------------------------------------------------
// Static state
// ---------------------------------------------------------------------------

static PendingAbilityCast g_ability_casts[MAX_PLAYERS];
static pthread_mutex_t    g_ability_casts_lock = PTHREAD_MUTEX_INITIALIZER;

static ActiveZone         g_zones[MAX_ZONES];
static pthread_mutex_t    g_zones_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t           g_next_zone_id = 1;

// Per-player mana regen accumulator (fractional mana between ticks)
static float g_mana_accum[MAX_PLAYERS];

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static double get_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static float dist2d(float ax, float ay, float bx, float by) {
    float dx = bx - ax;
    float dy = by - ay;
    return sqrtf(dx * dx + dy * dy);
}

// Closest distance from point (px,py) to line segment (ax,ay)-(bx,by)
static float point_to_segment_dist(float px, float py,
                                   float ax, float ay,
                                   float bx, float by) {
    float abx = bx - ax, aby = by - ay;
    float apx = px - ax, apy = py - ay;
    float ab_sq = abx * abx + aby * aby;
    if (ab_sq < 0.001f) return dist2d(px, py, ax, ay);  // Degenerate segment

    float t = (apx * abx + apy * aby) / ab_sq;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    float closest_x = ax + t * abx;
    float closest_y = ay + t * aby;
    return dist2d(px, py, closest_x, closest_y);
}

static int find_player_slot(uint32_t character_id) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (active_players[i].is_loaded &&
            active_players[i].character_id == character_id) {
            return i;
        }
    }
    return -1;
}

static int player_has_ability(ActivePlayer* player, uint16_t ability_id, int* out_slot) {
    for (int i = 0; i < player->ability_count; i++) {
        if (player->ability_slots[i] == ability_id) {
            if (out_slot) *out_slot = i;
            return 1;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Send helpers (unchanged)
// ---------------------------------------------------------------------------

static void send_ability_cast_cancel(int client_fd, uint32_t caster_id,
                                     uint16_t ability_id, uint8_t reason) {
    AbilityCastCancelPacket pkt = {0};
    pkt.header.type      = PACKET_ABILITY_CAST_CANCEL;
    pkt.header.player_id = htonl(caster_id);
    pkt.caster_id        = htonl(caster_id);
    pkt.ability_id       = htons(ability_id);
    pkt.reason           = reason;
    send(client_fd, &pkt, sizeof(pkt), 0);
}

static void send_ability_cast_start(int client_fd, uint32_t caster_id,
                                    uint16_t ability_id, float cast_time,
                                    float ox, float oy, float ax, float ay) {
    AbilityCastStartPacket pkt = {0};
    pkt.header.type      = PACKET_ABILITY_CAST_START;
    pkt.header.player_id = htonl(caster_id);
    pkt.caster_id        = htonl(caster_id);
    pkt.ability_id       = htons(ability_id);
    pkt.cast_time        = cast_time;
    pkt.origin_x         = ox;
    pkt.origin_y         = oy;
    pkt.aim_x            = ax;
    pkt.aim_y            = ay;
    send(client_fd, &pkt, sizeof(pkt), 0);
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
    send(client_fd, &pkt, sizeof(pkt), 0);
}

static void send_status_effect_apply(int client_fd, uint32_t target_id,
                                     uint8_t effect_type, int value,
                                     float duration, uint32_t source_id) {
    StatusEffectApplyPacket pkt = {0};
    pkt.header.type      = PACKET_STATUS_EFFECT_APPLY;
    pkt.header.player_id = htonl(target_id);
    pkt.target_id        = htonl(target_id);
    pkt.effect_type      = effect_type;
    pkt.value            = htonl((uint32_t)value);
    pkt.duration         = duration;
    pkt.source_id        = htonl(source_id);
    send(client_fd, &pkt, sizeof(pkt), 0);
}

static void send_mana_update(int client_fd, uint32_t player_id,
                             int32_t mana, int32_t max_mana) {
    ManaUpdatePacket pkt = {0};
    pkt.header.type      = PACKET_MANA_UPDATE;
    pkt.header.player_id = htonl(player_id);
    pkt.mana             = htonl((uint32_t)mana);
    pkt.max_mana         = htonl((uint32_t)max_mana);
    send(client_fd, &pkt, sizeof(pkt), 0);
}

static void send_spawn_zone(int client_fd, uint32_t zone_id, uint32_t caster_id,
                            uint16_t ability_id, float px, float py,
                            float duration, float radius, uint8_t has_collision) {
    SpawnZonePacket pkt = {0};
    pkt.header.type      = PACKET_SPAWN_ZONE;
    pkt.header.player_id = htonl(caster_id);
    pkt.zone_id          = htonl(zone_id);
    pkt.caster_id        = htonl(caster_id);
    pkt.ability_id       = htons(ability_id);
    pkt.pos_x            = px;
    pkt.pos_y            = py;
    pkt.duration         = duration;
    pkt.radius           = radius;
    pkt.has_collision     = has_collision;
    send(client_fd, &pkt, sizeof(pkt), 0);
}

static void send_remove_zone(int client_fd, uint32_t zone_id) {
    RemoveZonePacket pkt = {0};
    pkt.header.type      = PACKET_REMOVE_ZONE;
    pkt.header.player_id = 0;
    pkt.zone_id          = htonl(zone_id);
    send(client_fd, &pkt, sizeof(pkt), 0);
}

// ---------------------------------------------------------------------------
// Apply status effects (unchanged)
// ---------------------------------------------------------------------------

static void apply_effect_to_player(ActivePlayer* player, const AbilityEffectDef* effect,
                                   uint32_t source_id) {
    for (int i = 0; i < MAX_ACTIVE_EFFECTS; i++) {
        if (!player->active_effects[i].active) {
            player->active_effects[i].active             = 1;
            player->active_effects[i].effect_type        = (uint8_t)effect->type;
            player->active_effects[i].value              = effect->value;
            player->active_effects[i].duration_remaining = effect->duration;
            player->active_effects[i].tick_rate          = effect->tick_rate;
            player->active_effects[i].tick_remaining     = effect->tick_rate;
            player->active_effects[i].source_id          = source_id;
            return;
        }
    }
    printf("[EFFECT] No free effect slot on player %u\n", player->character_id);
}

static void apply_effect_to_npc(NPCEntity* npc, const AbilityEffectDef* effect,
                                uint32_t source_id) {
    (void)source_id;
    printf("[EFFECT] Applied %d to NPC %u (val=%d, dur=%.1fs)\n",
           effect->type, npc->id, effect->value, effect->duration);
}

// ---------------------------------------------------------------------------
// Zone spawning (unchanged)
// ---------------------------------------------------------------------------

static void spawn_zone(const AbilityDef* ability, uint32_t caster_id,
                       float pos_x, float pos_y, int client_fd) {
    pthread_mutex_lock(&g_zones_lock);

    int slot = -1;
    for (int i = 0; i < MAX_ZONES; i++) {
        if (!g_zones[i].is_active) { slot = i; break; }
    }

    if (slot == -1) {
        pthread_mutex_unlock(&g_zones_lock);
        printf("[ZONE] No free zone slots\n");
        return;
    }

    ActiveZone* zone = &g_zones[slot];
    memset(zone, 0, sizeof(ActiveZone));

    zone->is_active          = 1;
    zone->zone_id            = g_next_zone_id++;
    zone->caster_id          = caster_id;
    zone->ability_id         = ability->id;
    zone->pos_x              = pos_x;
    zone->pos_y              = pos_y;
    zone->has_collision       = ability->spawn.has_collision;
    zone->hp                 = ability->spawn.hp;
    zone->max_hp             = ability->spawn.hp;
    zone->duration_remaining = ability->spawn.duration;
    zone->radius             = ability->aoe.radius;
    zone->healing_per_tick   = ability->healing;

    zone->effect_count = ability->effect_count;
    for (int i = 0; i < ability->effect_count && i < MAX_ABILITY_EFFECTS; i++) {
        zone->effects[i] = ability->effects[i];
    }

    zone->tick_rate = 1.0f;
    for (int i = 0; i < zone->effect_count; i++) {
        if (zone->effects[i].tick_rate > 0.0f) {
            zone->tick_rate = zone->effects[i].tick_rate;
            break;
        }
    }
    zone->tick_timer = zone->tick_rate;

    pthread_mutex_unlock(&g_zones_lock);

    send_spawn_zone(client_fd, zone->zone_id, caster_id, ability->id,
                    pos_x, pos_y, ability->spawn.duration, zone->radius,
                    zone->has_collision);

    printf("[ZONE] Spawned zone %u ('%s') at (%.1f, %.1f) dur=%.1fs\n",
           zone->zone_id, ability->name, pos_x, pos_y, ability->spawn.duration);
}

// ---------------------------------------------------------------------------
// Projectile spawning — delegates to the modular projectile system
// ---------------------------------------------------------------------------

static void spawn_ability_projectile(const AbilityDef* ability, uint32_t caster_id,
                                     int client_fd, float ox, float oy,
                                     float aim_x, float aim_y) {
    ProjectileSpawnInfo info = {0};
    info.owner_type  = PROJECTILE_OWNER_PLAYER;
    info.owner_id    = caster_id;
    info.ability_id  = ability->id;
    info.owner_fd    = client_fd;
    info.origin_x    = ox;
    info.origin_y    = oy;
    info.aim_x       = aim_x;
    info.aim_y       = aim_y;
    info.speed       = ability->projectile.speed;
    info.width       = ability->projectile.width;
    info.max_range   = ability->range;
    info.damage      = ability->damage;  // weapon_damage added below after snapshot
    info.damage_type = ability->damage_type;
    info.bonus_damage = ability->bonus_damage;

    // Snapshot caster stats
    ActivePlayer* caster = player_find_active(caster_id);
    if (caster) {
        pthread_mutex_lock(&caster->lock);
        info.caster_strength     = caster->strength;
        info.caster_agility      = caster->agility;
        info.caster_intelligence = caster->intelligence;
        info.caster_wisdom       = caster->wisdom;
        info.caster_class        = caster->player_class;
        info.damage             += caster->weapon_damage;
        pthread_mutex_unlock(&caster->lock);
    }

    info.effect_count = ability->effect_count;
    for (int i = 0; i < ability->effect_count && i < MAX_ABILITY_EFFECTS; i++) {
        info.effects[i] = ability->effects[i];
    }

    projectile_spawn(&info);
}

// ---------------------------------------------------------------------------
// UPDATED: calc_damage now includes stat scaling and defense
// ---------------------------------------------------------------------------

static int calc_ability_damage(int base_damage, const AbilityBonusDamageDef* bonus,
                               int target_health, int target_max_health,
                               int target_defense,
                               int caster_str, int caster_agi,
                               int caster_int, int caster_wis,
                               uint8_t caster_class) {
    // 1. Stat bonus
    int stat_bonus = combat_stat_bonus_damage(caster_str, caster_agi,
                                               caster_int, caster_wis,
                                               caster_class);
    int damage = base_damage + stat_bonus;

    // 2. Bonus damage condition (e.g. execute)
    if (bonus && bonus->condition == 1 && target_max_health > 0) {
        float hp_pct = (float)target_health / (float)target_max_health * 100.0f;
        if (hp_pct <= bonus->threshold) {
            damage = (int)((float)damage * bonus->multiplier);
        }
    }

    // 3. Random variance (±10%)
    int variance = (rand() % 21) - 10;
    damage += (damage * variance) / 100;

    // 4. Defense reduction
    damage = combat_apply_defense(damage, target_defense);

    if (damage < 1) damage = 1;
    return damage;
}

// ============================================================================
// INIT / CLEANUP
// ============================================================================

void ability_handler_init(void) {
    memset(g_ability_casts, 0, sizeof(g_ability_casts));
    memset(g_zones, 0, sizeof(g_zones));
    memset(g_mana_accum, 0, sizeof(g_mana_accum));
    g_next_zone_id = 1;
    printf("[ABILITY] Handler initialized\n");
}

void ability_handler_cleanup(void) {
    printf("[ABILITY] Handler cleaned up\n");
}

// ============================================================================
// CAST INTENT (unchanged logic, but mark combat time)
// ============================================================================

void ability_handle_cast_intent(NPCWorld* world,
                                int client_fd,
                                uint32_t caster_id,
                                AbilityCastIntentPacket* pkt) {
    (void)world;

    uint16_t ability_id = ntohs(pkt->ability_id);
    float aim_x = pkt->aim_x;
    float aim_y = pkt->aim_y;
    uint32_t target_id = ntohl(pkt->target_id);

    const AbilityDef* ability = ability_get(ability_id);
    if (!ability) {
        send_ability_cast_cancel(client_fd, caster_id, ability_id, 3);
        return;
    }

    ActivePlayer* caster = player_find_active(caster_id);
    if (!caster) return;

    int player_slot = find_player_slot(caster_id);
    if (player_slot < 0) return;

    pthread_mutex_lock(&caster->lock);

    if (ability->class_id != caster->player_class) {
        pthread_mutex_unlock(&caster->lock);
        send_ability_cast_cancel(client_fd, caster_id, ability_id, 3);
        return;
    }

    if (caster->level < ability->unlock_level) {
        pthread_mutex_unlock(&caster->lock);
        send_ability_cast_cancel(client_fd, caster_id, ability_id, 3);
        return;
    }

    int ability_slot = -1;
    if (!player_has_ability(caster, ability_id, &ability_slot)) {
        pthread_mutex_unlock(&caster->lock);
        send_ability_cast_cancel(client_fd, caster_id, ability_id, 3);
        return;
    }

    if (caster->ability_cooldowns[ability_slot] > 0.0f) {
        pthread_mutex_unlock(&caster->lock);
        send_ability_cast_cancel(client_fd, caster_id, ability_id, 3);
        return;
    }

    pthread_mutex_lock(&g_ability_casts_lock);
    if (g_ability_casts[player_slot].is_active) {
        pthread_mutex_unlock(&g_ability_casts_lock);
        pthread_mutex_unlock(&caster->lock);
        send_ability_cast_cancel(client_fd, caster_id, ability_id, 2);
        return;
    }

    if (ability->mana_cost > 0 && caster->mana < ability->mana_cost) {
        pthread_mutex_unlock(&g_ability_casts_lock);
        pthread_mutex_unlock(&caster->lock);
        send_ability_cast_cancel(client_fd, caster_id, ability_id, 3);
        return;
    }

    if (ability->mana_cost > 0) {
        caster->mana -= ability->mana_cost;
        send_mana_update(client_fd, caster_id, caster->mana, caster->max_mana);
    }

    float origin_x = caster->pos_x;
    float origin_y = caster->pos_y;

    // Mark combat time for HP regen suppression
    caster->last_combat_time = get_time();

    pthread_mutex_unlock(&caster->lock);

    PendingAbilityCast* cast = &g_ability_casts[player_slot];
    cast->is_active       = 1;
    cast->cast_start_time = get_time();
    cast->cast_duration   = ability->cast_time;
    cast->ability_id      = ability_id;
    cast->caster_id       = caster_id;
    cast->caster_slot     = player_slot;
    cast->client_fd       = client_fd;
    cast->origin_x        = origin_x;
    cast->origin_y        = origin_y;
    cast->aim_x           = aim_x;
    cast->aim_y           = aim_y;
    cast->target_id       = target_id;

    pthread_mutex_unlock(&g_ability_casts_lock);

    send_ability_cast_start(client_fd, caster_id, ability_id,
                            ability->cast_time, origin_x, origin_y, aim_x, aim_y);

    printf("[ABILITY] Player %u casting '%s' (id=%u, cast=%.1fs)\n",
           caster_id, ability->name, ability_id, ability->cast_time);
}

// ============================================================================
// CAST CANCEL (unchanged)
// ============================================================================

void ability_handle_cast_cancel(int client_fd, uint32_t caster_id) {
    int slot = find_player_slot(caster_id);
    if (slot < 0) return;

    pthread_mutex_lock(&g_ability_casts_lock);

    if (g_ability_casts[slot].is_active) {
        uint16_t ability_id = g_ability_casts[slot].ability_id;
        g_ability_casts[slot].is_active = 0;

        const AbilityDef* ability = ability_get(ability_id);
        if (ability && ability->mana_cost > 0) {
            ActivePlayer* caster = player_find_active(caster_id);
            if (caster) {
                pthread_mutex_lock(&caster->lock);
                caster->mana += ability->mana_cost;
                if (caster->mana > caster->max_mana) caster->mana = caster->max_mana;
                send_mana_update(client_fd, caster_id, caster->mana, caster->max_mana);
                pthread_mutex_unlock(&caster->lock);
            }
        }

        send_ability_cast_cancel(client_fd, caster_id, ability_id, 0);
    }

    pthread_mutex_unlock(&g_ability_casts_lock);
}

// ============================================================================
// RESOLVE CAST — UPDATED with stat scaling, defense, evasion, XP
// ============================================================================

static void resolve_cast(PendingAbilityCast* cast, NPCWorld* world) {
    const AbilityDef* ability = ability_get(cast->ability_id);
    if (!ability) {
        cast->is_active = 0;
        return;
    }

    uint32_t caster_id = cast->caster_id;
    int client_fd      = cast->client_fd;
    float origin_x     = cast->origin_x;
    float origin_y     = cast->origin_y;
    float aim_x        = cast->aim_x;
    float aim_y        = cast->aim_y;

    // Snapshot caster stats
    int c_str = 0, c_agi = 0, c_int = 0, c_wis = 0, c_wpn = 0;
    uint8_t c_class = 1;

    ActivePlayer* caster = player_find_active(caster_id);
    if (caster) {
        pthread_mutex_lock(&caster->lock);
        int ability_slot = -1;
        if (player_has_ability(caster, cast->ability_id, &ability_slot)) {
            caster->ability_cooldowns[ability_slot] = ability->cooldown;
        }
        c_str   = caster->strength;
        c_agi   = caster->agility;
        c_int   = caster->intelligence;
        c_wis   = caster->wisdom;
        c_wpn   = caster->weapon_damage;
        c_class = caster->player_class;
        caster->last_combat_time = get_time();
        pthread_mutex_unlock(&caster->lock);
    }

    // --- Movement abilities ---
    float dash_start_x = origin_x;
    float dash_start_y = origin_y;
    int had_movement = 0;

    if (ability->movement.type != MOVEMENT_NONE && caster) {
        pthread_mutex_lock(&caster->lock);
        float dx = aim_x - caster->pos_x;
        float dy = aim_y - caster->pos_y;
        float len = sqrtf(dx * dx + dy * dy);
        if (len > 0.001f) {
            float move_dist = ability->movement.distance;
            if (len < move_dist) move_dist = len;
            dash_start_x = caster->pos_x;
            dash_start_y = caster->pos_y;
            caster->pos_x += (dx / len) * move_dist;
            caster->pos_y += (dy / len) * move_dist;
            caster->is_dirty = 1;
            origin_x = caster->pos_x;
            origin_y = caster->pos_y;
            had_movement = 1;
        }
        pthread_mutex_unlock(&caster->lock);
    }

    // --- Projectile abilities ---
    if (ability->projectile.type != PROJECTILE_NONE) {
        spawn_ability_projectile(ability, caster_id, client_fd,
                                 origin_x, origin_y, aim_x, aim_y);
        cast->is_active = 0;
        return;
    }

    // --- Zone spawning ---
    if (ability->spawn.type != SPAWN_NONE) {
        float zone_x = (ability->range > 0.0f) ? aim_x : origin_x;
        float zone_y = (ability->range > 0.0f) ? aim_y : origin_y;
        spawn_zone(ability, caster_id, zone_x, zone_y, client_fd);
    }

    // --- Self-buff abilities ---
    if (ability->range == 0.0f && ability->damage == 0 &&
        ability->effect_count > 0 && ability->spawn.type == SPAWN_NONE) {
        if (caster) {
            pthread_mutex_lock(&caster->lock);
            for (int e = 0; e < ability->effect_count; e++) {
                apply_effect_to_player(caster, &ability->effects[e], caster_id);
                send_status_effect_apply(client_fd, caster_id,
                                         (uint8_t)ability->effects[e].type,
                                         ability->effects[e].value,
                                         ability->effects[e].duration,
                                         caster_id);
            }
            pthread_mutex_unlock(&caster->lock);
        }
        cast->is_active = 0;
        return;
    }

    // --- Ally healing (unchanged) ---
    if (ability->target_type == ABILITY_TARGET_ALLY && ability->healing > 0) {
        if (cast->target_id != 0) {
            ActivePlayer* target = player_find_active(cast->target_id);
            if (target) {
                pthread_mutex_lock(&target->lock);
                float d = dist2d(origin_x, origin_y, target->pos_x, target->pos_y);
                if (d <= ability->range || ability->range == 0.0f) {
                    // Healing scales with wisdom
                    int heal_bonus = c_wis / 3;
                    int total_heal = ability->healing + heal_bonus;

                    target->health += total_heal;
                    if (target->health > target->max_health)
                        target->health = target->max_health;

                    send_ability_effect(client_fd, caster_id, cast->target_id,
                                        cast->ability_id, 0, total_heal,
                                        target->health, 0);

                    for (int e = 0; e < ability->effect_count; e++) {
                        apply_effect_to_player(target, &ability->effects[e], caster_id);
                        send_status_effect_apply(client_fd, cast->target_id,
                                                 (uint8_t)ability->effects[e].type,
                                                 ability->effects[e].value,
                                                 ability->effects[e].duration,
                                                 caster_id);
                    }
                }
                pthread_mutex_unlock(&target->lock);
            }
        } else if (ability->aoe.shape != ABILITY_AOE_NONE) {
            int heal_bonus = c_wis / 3;
            int total_heal = ability->healing + heal_bonus;

            pthread_mutex_lock(&active_players_lock);
            for (int i = 0; i < MAX_PLAYERS; i++) {
                if (!active_players[i].is_loaded) continue;
                pthread_mutex_lock(&active_players[i].lock);
                float d = dist2d(origin_x, origin_y,
                                 active_players[i].pos_x, active_players[i].pos_y);
                if (d <= ability->aoe.radius) {
                    if (total_heal > 0) {
                        active_players[i].health += total_heal;
                        if (active_players[i].health > active_players[i].max_health)
                            active_players[i].health = active_players[i].max_health;
                        send_ability_effect(client_fd, caster_id,
                                            active_players[i].character_id,
                                            cast->ability_id, 0, total_heal,
                                            active_players[i].health, 0);
                    }
                    for (int e = 0; e < ability->effect_count; e++) {
                        apply_effect_to_player(&active_players[i],
                                               &ability->effects[e], caster_id);
                        send_status_effect_apply(client_fd,
                                                 active_players[i].character_id,
                                                 (uint8_t)ability->effects[e].type,
                                                 ability->effects[e].value,
                                                 ability->effects[e].duration,
                                                 caster_id);
                    }
                }
                pthread_mutex_unlock(&active_players[i].lock);
            }
            pthread_mutex_unlock(&active_players_lock);
        }
        cast->is_active = 0;
        return;
    }

    // --- Damage to NPCs — snapshot under lock, send after unlock ---
    if (ability->damage > 0) {
        float aim_dx = aim_x - origin_x;
        float aim_dy = aim_y - origin_y;

        // Snapshot results under lock
        typedef struct {
            uint32_t npc_id;
            int      damage;
            int      new_health;
            uint8_t  is_kill;
            uint8_t  evaded;
            uint64_t xp_reward;
            uint16_t npc_type_id;
            float    npc_x, npc_y;
        } AbilityHitResult;

        #define MAX_ABILITY_HITS 32
        AbilityHitResult hits[MAX_ABILITY_HITS];
        int hit_count = 0;

        pthread_mutex_lock(&world->lock);

        for (int n = 0; n < MAX_NPCS; n++) {
            NPCEntity* npc = &world->npcs[n];
            if (!npc->is_alive || npc->id == 0) continue;

            float d = dist2d(origin_x, origin_y, npc->pos_x, npc->pos_y);
            int hit = 0;

            if (ability->aoe.shape == ABILITY_AOE_CIRCLE) {
                float check_range = ability->aoe.radius + npc->hitbox_radius;
                if (ability->range > 0.0f) {
                    float d_aim = dist2d(aim_x, aim_y, npc->pos_x, npc->pos_y);
                    hit = (d_aim <= check_range);
                } else {
                    hit = (d <= check_range);
                }
            } else if (ability->aoe.shape == ABILITY_AOE_CONE) {
                float cone_half_rad = ability->aoe.angle / 2.0f * (M_PI / 180.0f);
                float check_range = ability->aoe.radius + npc->hitbox_radius;
                if (d <= check_range && d > 0.001f) {
                    float to_npc_x = npc->pos_x - origin_x;
                    float to_npc_y = npc->pos_y - origin_y;
                    float aim_angle = atan2f(aim_dy, aim_dx);
                    float npc_angle = atan2f(to_npc_y, to_npc_x);
                    float delta = npc_angle - aim_angle;
                    while (delta > M_PI)  delta -= 2.0f * M_PI;
                    while (delta < -M_PI) delta += 2.0f * M_PI;
                    hit = (fabsf(delta) <= cone_half_rad);
                }
            } else if (ability->aoe.shape == ABILITY_AOE_RECTANGLE) {
                hit = (d <= ability->range + npc->hitbox_radius);
            } else {
                if (had_movement) {
                    float dash_width = 50.0f;
                    float seg_dist = point_to_segment_dist(
                        npc->pos_x, npc->pos_y,
                        dash_start_x, dash_start_y,
                        origin_x, origin_y);
                    hit = (seg_dist <= dash_width + npc->hitbox_radius);
                } else {
                    float check_range = ability->range + npc->hitbox_radius;
                    hit = (d <= check_range);
                }
            }

            if (!hit) continue;
            if (hit_count >= MAX_ABILITY_HITS) break;

            // Evasion check
            if (combat_check_evasion(npc->evasion)) {
                hits[hit_count].npc_id     = npc->id;
                hits[hit_count].damage     = 0;
                hits[hit_count].new_health = npc->health;
                hits[hit_count].is_kill    = 0;
                hits[hit_count].evaded     = 1;
                hits[hit_count].xp_reward  = 0;
                hit_count++;
                if (ability->aoe.shape == ABILITY_AOE_NONE && !had_movement) break;
                continue;
            }

            int damage = calc_ability_damage(ability->damage + c_wpn, &ability->bonus_damage,
                                              npc->health, npc->max_health,
                                              npc->defense,
                                              c_str, c_agi, c_int, c_wis, c_class);

            npc->health -= damage;
            if (npc->health < 0) npc->health = 0;

            uint8_t is_kill = (npc->health == 0) ? 1 : 0;
            if (is_kill) {
                npc->is_alive = 0;
                struct timespec _ts;
                clock_gettime(CLOCK_MONOTONIC, &_ts);
                npc->death_time = _ts.tv_sec + _ts.tv_nsec / 1e9;
            }

            for (int e = 0; e < ability->effect_count; e++) {
                apply_effect_to_npc(npc, &ability->effects[e], caster_id);
            }

            hits[hit_count].npc_id     = npc->id;
            hits[hit_count].damage     = damage;
            hits[hit_count].new_health = npc->health;
            hits[hit_count].is_kill    = is_kill;
            hits[hit_count].evaded      = 0;
            hits[hit_count].xp_reward   = is_kill ? npc->xp_reward : 0;
            hits[hit_count].npc_type_id = npc->npc_type_id;
            hits[hit_count].npc_x       = npc->pos_x;
            hits[hit_count].npc_y       = npc->pos_y;
            hit_count++;

            if (ability->aoe.shape == ABILITY_AOE_NONE && !had_movement) break;
        }

        pthread_mutex_unlock(&world->lock);

        // Send results outside world lock
        for (int h = 0; h < hit_count; h++) {
            if (hits[h].evaded) {
                send_ability_effect(client_fd, caster_id, hits[h].npc_id,
                                    cast->ability_id, 0, 0, hits[h].new_health, 0);
                printf("[ABILITY] '%s' MISSED NPC %u (evasion)\n",
                       ability->name, hits[h].npc_id);
            } else {
                send_ability_effect(client_fd, caster_id, hits[h].npc_id,
                                    cast->ability_id, hits[h].damage, 0,
                                    hits[h].new_health, hits[h].is_kill);
                printf("[ABILITY] '%s' hit NPC %u for %d dmg (hp=%d)%s\n",
                       ability->name, hits[h].npc_id, hits[h].damage,
                       hits[h].new_health, hits[h].is_kill ? " — KILLED" : "");
            }

            if (hits[h].xp_reward > 0) {
                party_award_xp(caster_id, hits[h].xp_reward);
            }
            if (hits[h].is_kill) {
                loot_roll(hits[h].npc_type_id, hits[h].npc_x, hits[h].npc_y, caster_id);
            }
        }
    }

    // --- Self-buff on damage abilities ---
    if (ability->aoe.shape != ABILITY_AOE_NONE && ability->effect_count > 0 &&
        ability->target_type == ABILITY_TARGET_ENEMY) {
        if (caster) {
            pthread_mutex_lock(&caster->lock);
            for (int e = 0; e < ability->effect_count; e++) {
                if (ability->effects[e].type == EFFECT_BUFF) {
                    apply_effect_to_player(caster, &ability->effects[e], caster_id);
                    send_status_effect_apply(client_fd, caster_id,
                                             (uint8_t)ability->effects[e].type,
                                             ability->effects[e].value,
                                             ability->effects[e].duration,
                                             caster_id);
                }
            }
            pthread_mutex_unlock(&caster->lock);
        }
    }

    cast->is_active = 0;
}

// ============================================================================
// TICK — UPDATED: wisdom-scaled mana regen
// ============================================================================

void ability_tick(NPCWorld* world, double delta_time) {
    float dt = (float)delta_time;
    double now = get_time();

    // -----------------------------------------------------------------------
    // 1. Resolve completed ability casts
    // -----------------------------------------------------------------------
    pthread_mutex_lock(&g_ability_casts_lock);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        PendingAbilityCast* cast = &g_ability_casts[i];
        if (!cast->is_active) continue;
        double elapsed = now - cast->cast_start_time;
        if (elapsed < cast->cast_duration) continue;
        resolve_cast(cast, world);
    }
    pthread_mutex_unlock(&g_ability_casts_lock);

    // -----------------------------------------------------------------------
    // 2. Tick cooldowns
    // -----------------------------------------------------------------------
    pthread_mutex_lock(&active_players_lock);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!active_players[i].is_loaded) continue;
        pthread_mutex_lock(&active_players[i].lock);
        for (int s = 0; s < active_players[i].ability_count; s++) {
            if (active_players[i].ability_cooldowns[s] > 0.0f) {
                active_players[i].ability_cooldowns[s] -= dt;
                if (active_players[i].ability_cooldowns[s] < 0.0f)
                    active_players[i].ability_cooldowns[s] = 0.0f;
            }
        }
        pthread_mutex_unlock(&active_players[i].lock);
    }
    pthread_mutex_unlock(&active_players_lock);

    // -----------------------------------------------------------------------
    // 3. Tick status effects
    // -----------------------------------------------------------------------
    pthread_mutex_lock(&active_players_lock);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!active_players[i].is_loaded) continue;
        pthread_mutex_lock(&active_players[i].lock);

        for (int e = 0; e < MAX_ACTIVE_EFFECTS; e++) {
            if (!active_players[i].active_effects[e].active) continue;

            active_players[i].active_effects[e].duration_remaining -= dt;

            if (active_players[i].active_effects[e].duration_remaining <= 0.0f) {
                active_players[i].active_effects[e].active = 0;
                StatusEffectRemovePacket pkt = {0};
                pkt.header.type      = PACKET_STATUS_EFFECT_REMOVE;
                pkt.header.player_id = htonl(active_players[i].character_id);
                pkt.target_id        = htonl(active_players[i].character_id);
                pkt.effect_type      = active_players[i].active_effects[e].effect_type;
                send(active_players[i].client_fd, &pkt, sizeof(pkt), 0);
                continue;
            }

            uint8_t etype = active_players[i].active_effects[e].effect_type;
            if ((etype == EFFECT_DOT || etype == EFFECT_HOT) &&
                active_players[i].active_effects[e].tick_rate > 0.0f) {

                active_players[i].active_effects[e].tick_remaining -= dt;
                if (active_players[i].active_effects[e].tick_remaining <= 0.0f) {
                    active_players[i].active_effects[e].tick_remaining =
                        active_players[i].active_effects[e].tick_rate;

                    int value = active_players[i].active_effects[e].value;
                    if (etype == EFFECT_HOT) {
                        active_players[i].health += value;
                        if (active_players[i].health > active_players[i].max_health)
                            active_players[i].health = active_players[i].max_health;
                    } else if (etype == EFFECT_DOT) {
                        active_players[i].health -= value;
                        if (active_players[i].health < 0)
                            active_players[i].health = 0;
                        // DOTs count as combat for HP regen suppression
                        active_players[i].last_combat_time = now;
                    }
                }
            }
        }

        pthread_mutex_unlock(&active_players[i].lock);
    }
    pthread_mutex_unlock(&active_players_lock);

    // -----------------------------------------------------------------------
    // 4. Tick zones (unchanged)
    // -----------------------------------------------------------------------
    pthread_mutex_lock(&g_zones_lock);
    for (int z = 0; z < MAX_ZONES; z++) {
        if (!g_zones[z].is_active) continue;

        g_zones[z].duration_remaining -= dt;
        if (g_zones[z].duration_remaining <= 0.0f) {
            pthread_mutex_lock(&active_players_lock);
            for (int p = 0; p < MAX_PLAYERS; p++) {
                if (active_players[p].is_loaded) {
                    send_remove_zone(active_players[p].client_fd, g_zones[z].zone_id);
                }
            }
            pthread_mutex_unlock(&active_players_lock);
            g_zones[z].is_active = 0;
            continue;
        }

        g_zones[z].tick_timer -= dt;
        if (g_zones[z].tick_timer <= 0.0f) {
            g_zones[z].tick_timer = g_zones[z].tick_rate;

            pthread_mutex_lock(&active_players_lock);
            for (int p = 0; p < MAX_PLAYERS; p++) {
                if (!active_players[p].is_loaded) continue;
                pthread_mutex_lock(&active_players[p].lock);
                float d = dist2d(g_zones[z].pos_x, g_zones[z].pos_y,
                                 active_players[p].pos_x, active_players[p].pos_y);
                if (d <= g_zones[z].radius) {
                    if (g_zones[z].healing_per_tick > 0) {
                        active_players[p].health += g_zones[z].healing_per_tick;
                        if (active_players[p].health > active_players[p].max_health)
                            active_players[p].health = active_players[p].max_health;
                    }
                    for (int e = 0; e < g_zones[z].effect_count; e++) {
                        if (g_zones[z].effects[e].reapply) {
                            apply_effect_to_player(&active_players[p],
                                                   &g_zones[z].effects[e],
                                                   g_zones[z].caster_id);
                        }
                    }
                }
                pthread_mutex_unlock(&active_players[p].lock);
            }
            pthread_mutex_unlock(&active_players_lock);

            pthread_mutex_lock(&world->lock);
            for (int n = 0; n < MAX_NPCS; n++) {
                NPCEntity* npc = &world->npcs[n];
                if (!npc->is_alive || npc->id == 0) continue;
                float d = dist2d(g_zones[z].pos_x, g_zones[z].pos_y,
                                 npc->pos_x, npc->pos_y);
                if (d <= g_zones[z].radius) {
                    for (int e = 0; e < g_zones[z].effect_count; e++) {
                        apply_effect_to_npc(npc, &g_zones[z].effects[e],
                                            g_zones[z].caster_id);
                    }
                }
            }
            pthread_mutex_unlock(&world->lock);
        }
    }
    pthread_mutex_unlock(&g_zones_lock);

    // -----------------------------------------------------------------------
    // 5. Tick projectiles — handled by projectile.c (projectile_tick)
    // -----------------------------------------------------------------------

    // -----------------------------------------------------------------------
    // 6. Mana regen — UPDATED: scaled by wisdom
    // -----------------------------------------------------------------------
    pthread_mutex_lock(&active_players_lock);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!active_players[i].is_loaded) continue;
        if (active_players[i].max_mana == 0) continue;

        pthread_mutex_lock(&active_players[i].lock);

        if (active_players[i].mana < active_players[i].max_mana) {
            float regen_rate = combat_mana_regen_rate(active_players[i].wisdom);
            float regen = regen_rate * dt;
            g_mana_accum[i] += regen;

            if (g_mana_accum[i] >= 1.0f) {
                int regen_int = (int)g_mana_accum[i];
                g_mana_accum[i] -= (float)regen_int;

                active_players[i].mana += regen_int;
                if (active_players[i].mana > active_players[i].max_mana)
                    active_players[i].mana = active_players[i].max_mana;

                send_mana_update(active_players[i].client_fd,
                                 active_players[i].character_id,
                                 active_players[i].mana,
                                 active_players[i].max_mana);
            }
        }

        pthread_mutex_unlock(&active_players[i].lock);
    }
    pthread_mutex_unlock(&active_players_lock);
}