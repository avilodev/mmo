/**
 * @file
 * Validate, schedule, resolve, and tick world-server abilities and zones.
 */

#include "ability_handler.h"
#include "player_effects.h"
#include "progression.h"
#include "ability_def.h"
#include "combat.h"
#include "combat_stats.h"
#include "player_level.h"
#include "player_data.h"
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
#include "utils.h"

extern ActivePlayer active_players[];
extern NPCWorld g_npc_world;

static PendingAbilityCast g_ability_casts[MAX_PLAYERS];
static pthread_mutex_t    g_ability_casts_lock = PTHREAD_MUTEX_INITIALIZER;

static ActiveZone         g_zones[MAX_ZONES];
static pthread_mutex_t    g_zones_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t           g_next_zone_id = 1;

// Per-player mana regen accumulator (fractional mana between ticks)
/** Carry the fractional part of resource regeneration between ticks.
 *
 * Mana and stamina accumulate upward; rage decay accumulates downward. A form swap
 * changes which of the two a slot is doing, so the accumulator is cleared on the
 * swap — otherwise a fraction banked as regeneration would be spent as decay. */
static float g_mana_accum[MAX_PLAYERS];



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

/**
 * Calculate the shortest distance from a point to a line segment.
 *
 * @return The distance in world units.
 */
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
    return player_slot_of(character_id);
}

/**
 * Find an ability on the hotbar of the form the player is currently in.
 *
 * Only the active form's row is searched: an ability the player owns but which
 * belongs to the other form is not castable right now.
 *
 * @param out_slot  Receives the slot index; may be NULL.
 * @return          1 when the ability is on the active bar, or 0 otherwise.
 */
static int player_has_ability(ActivePlayer* player, uint16_t ability_id, int* out_slot) {
    uint8_t form = player->form < FORM_COUNT ? player->form : FORM_HUMAN;

    int count = player->ability_count[form];
    if (count > MAX_ABILITY_SLOTS) count = MAX_ABILITY_SLOTS;

    for (int i = 0; i < count; i++) {
        if (player->ability_slots[form][i] == ability_id) {
            if (out_slot) *out_slot = i;
            return 1;
        }
    }
    return 0;
}

/**
 * Send a cast-cancellation packet to one client.
 */
static void send_ability_cast_cancel(int client_fd, uint32_t caster_id,
                                     uint16_t ability_id, uint8_t reason) {
    AbilityCastCancelPacket pkt = {0};
    pkt.header.type         = PACKET_ABILITY_CAST_CANCEL;
    pkt.header.player_id    = htonl(caster_id);
    pkt.header.payload_size = htons(sizeof(AbilityCastCancelPacket) - sizeof(PacketHeader));
    pkt.caster_id        = htonl(caster_id);
    pkt.ability_id       = htons(ability_id);
    pkt.reason           = reason;
    server_send(client_fd, &pkt, sizeof(pkt));
}

/**
 * Send a cast-start packet to one client.
 */
static void send_ability_cast_start(int client_fd, uint32_t caster_id,
                                    uint16_t ability_id, float cast_time,
                                    float ox, float oy, float ax, float ay) {
    AbilityCastStartPacket pkt = {0};
    pkt.header.type         = PACKET_ABILITY_CAST_START;
    pkt.header.player_id    = htonl(caster_id);
    pkt.header.payload_size = htons(sizeof(AbilityCastStartPacket) - sizeof(PacketHeader));
    pkt.caster_id        = htonl(caster_id);
    pkt.ability_id       = htons(ability_id);
    pkt.cast_time        = cast_time;
    pkt.origin_x         = ox;
    pkt.origin_y         = oy;
    pkt.aim_x            = ax;
    pkt.aim_y            = ay;
    server_send(client_fd, &pkt, sizeof(pkt));
}

/**
 * Send resolved damage or healing for one ability target.
 */
static void send_ability_effect(int client_fd, uint32_t caster_id, uint32_t target_id,
                                uint16_t ability_id, int damage, int healing,
                                int target_new_hp, uint8_t is_kill, uint8_t is_crit) {
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
    pkt.is_crit             = is_crit;
    server_send(client_fd, &pkt, sizeof(pkt));
}

/**
 * Send a status-effect application to one client.
 */
static void send_status_effect_apply(int client_fd, uint32_t target_id,
                                     uint8_t effect_type, int value,
                                     float duration, uint32_t source_id) {
    StatusEffectApplyPacket pkt = {0};
    pkt.header.type         = PACKET_STATUS_EFFECT_APPLY;
    pkt.header.player_id    = htonl(target_id);
    pkt.header.payload_size = htons(sizeof(StatusEffectApplyPacket) - sizeof(PacketHeader));
    pkt.target_id        = htonl(target_id);
    pkt.effect_type      = effect_type;
    pkt.value            = htonl((uint32_t)value);
    pkt.duration         = duration;
    pkt.source_id        = htonl(source_id);
    server_send(client_fd, &pkt, sizeof(pkt));
}

/**
 * Send a player's current mana values to one client.
 */
static void send_mana_update(int client_fd, uint32_t player_id,
                             int32_t mana, int32_t max_mana) {
    ManaUpdatePacket pkt = {0};
    pkt.header.type         = PACKET_MANA_UPDATE;
    pkt.header.player_id    = htonl(player_id);
    pkt.header.payload_size = htons(sizeof(ManaUpdatePacket) - sizeof(PacketHeader));
    pkt.mana             = htonl((uint32_t)mana);
    pkt.max_mana         = htonl((uint32_t)max_mana);
    server_send(client_fd, &pkt, sizeof(pkt));
}

/**
 * Send an active-zone spawn to one client.
 */
static void send_spawn_zone(int client_fd, uint32_t zone_id, uint32_t caster_id,
                            uint16_t ability_id, float px, float py,
                            float duration, float radius, uint8_t has_collision) {
    SpawnZonePacket pkt = {0};
    pkt.header.type         = PACKET_SPAWN_ZONE;
    pkt.header.player_id    = htonl(caster_id);
    pkt.header.payload_size = htons(sizeof(SpawnZonePacket) - sizeof(PacketHeader));
    pkt.zone_id          = htonl(zone_id);
    pkt.caster_id        = htonl(caster_id);
    pkt.ability_id       = htons(ability_id);
    pkt.pos_x            = px;
    pkt.pos_y            = py;
    pkt.duration         = duration;
    pkt.radius           = radius;
    pkt.has_collision     = has_collision;
    server_send(client_fd, &pkt, sizeof(pkt));
}

/**
 * Send an active-zone removal to one client.
 */
static void send_remove_zone(int client_fd, uint32_t zone_id) {
    RemoveZonePacket pkt = {0};
    pkt.header.type         = PACKET_REMOVE_ZONE;
    pkt.header.player_id    = 0;
    pkt.header.payload_size = htons(sizeof(RemoveZonePacket) - sizeof(PacketHeader));
    pkt.zone_id          = htonl(zone_id);
    server_send(client_fd, &pkt, sizeof(pkt));
}

/**
 * Install an ability effect in a player's first free effect slot.
 *
 * The caller must hold the player's lock.
 */
static void apply_effect_to_player(ActivePlayer* player, const AbilityEffectDef* effect,
                                   uint32_t source_id) {
    player_effect_apply(player, effect, source_id);
}

/**
 * Apply one ability effect to an NPC.
 *
 * The caller must hold the NPC world's lock.
 */
static void apply_effect_to_npc(NPCEntity* npc, const AbilityEffectDef* effect,
                                uint32_t source_id) {
    if (effect->type == EFFECT_TAUNT) {
        npc->taunt_source_id  = source_id;
        npc->taunt_expires_at = get_time() + effect->duration;
        npc->ai_target_id     = source_id;
        printf("[EFFECT] NPC %u taunted by %u for %.1fs\n",
               npc->id, source_id, effect->duration);
        return;
    }

    printf("[EFFECT] Applied %d to NPC %u (val=%d, dur=%.1fs)\n",
           effect->type, npc->id, effect->value, effect->duration);
}

/**
 * Allocate an active zone and notify the casting client.
 */
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

/**
 * Snapshot caster statistics and submit an ability projectile.
 */
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
    ActivePlayer* caster = player_acquire(caster_id);
    if (caster) {
        memcpy(info.caster_stats, caster->stats, sizeof(info.caster_stats));
        info.damage += caster->weapon_damage;

        /* Snapshot the caster's outgoing modifiers and form power at launch: the
         * projectile may land long after the cast, and its damage should reflect the
         * moment it was fired. */
        player_collect_modifiers(caster, 0, &info.caster_mods);
        info.damage = (int)((float)info.damage * player_form_power(caster));
        player_release(caster);
    }
    info.damage_stat = (int)ability->damage_stat;

    info.effect_count = ability->effect_count;
    for (int i = 0; i < ability->effect_count && i < MAX_ABILITY_EFFECTS; i++) {
        info.effects[i] = ability->effects[i];
    }

    projectile_spawn(&info);
}

/**
 * Calculate ability damage after scaling, conditional bonuses, crit, variance, and armor.
 *
 * @param out_condition_held  Receives 1 when the execute branch fired; may be NULL.
 * @param out_is_crit         Receives 1 on a critical strike; may be NULL.
 * @return                    Final damage, never below one.
 */
static int calc_ability_damage(int base_damage, const AbilityBonusDamageDef* bonus,
                               int target_health, int target_max_health,
                               int target_armor,
                               const int* caster_stats,
                               const DamageModifiers* caster_mods,
                               int damage_stat,
                               uint8_t* out_condition_held,
                               uint8_t* out_is_crit) {
    /* 1. Scale base damage by the attribute named in the ability definition.
     *    damage_stat is a StatType int; anything outside 0..STAT_COUNT means no scaling. */
    float mult = combat_ability_damage_mult(damage_stat, caster_stats);
    int damage = (int)((float)base_damage * mult);

    /* 2. The execute branch. Whether it fired is reported back, because effects
     *    marked onCondition fire on exactly the same condition. */
    if (out_condition_held) *out_condition_held = 0;
    if (bonus && bonus->condition == 1 && target_max_health > 0) {
        float hp_pct = (float)target_health / (float)target_max_health * 100.0f;
        if (hp_pct <= bonus->threshold) {
            damage = (int)((float)damage * bonus->multiplier);
            if (out_condition_held) *out_condition_held = 1;
        }
    }

    uint8_t is_crit = (uint8_t)combat_check_crit(caster_stats[STAT_PRECISION]);
    if (is_crit) {
        damage = (int)((float)damage * combat_crit_multiplier(caster_stats[STAT_FEROCITY]));
    }
    if (out_is_crit) *out_is_crit = is_crit;

    // 3. Random variance (±10%)
    int variance = (rand() % 21) - 10;
    damage += (damage * variance) / 100;

    DamageModifiers target_mods;
    damage_mods_reset(&target_mods);
    damage_mods_add_armor(&target_mods, target_armor);

    return damage_resolve(damage, caster_mods, &target_mods);
}

/**
 * Initialize pending casts, active zones, and mana accumulators.
 */
void ability_handler_init(void) {
    memset(g_ability_casts, 0, sizeof(g_ability_casts));
    memset(g_zones, 0, sizeof(g_zones));
    memset(g_mana_accum, 0, sizeof(g_mana_accum));
    g_next_zone_id = 1;
    printf("[ABILITY] Handler initialized\n");
}

/**
 * Log ability-handler shutdown.
 */
void ability_handler_cleanup(void) {
    printf("[ABILITY] Handler cleaned up\n");
}

/**
 * Rebuild both forms' hotbars from the race registry and the player's level.
 *
 * The caller must hold the player's lock.
 */
void ability_refresh_hotbars(ActivePlayer* player) {
    if (!player) return;

    for (uint8_t form = 0; form < FORM_COUNT; form++) {
        uint16_t candidates[MAX_ABILITY_SLOTS * 2];
        int found = ability_get_form_abilities(
            (uint8_t)player->race_id, form, candidates,
            (int)(sizeof(candidates) / sizeof(candidates[0])));

        uint8_t filled = 0;
        for (int i = 0; i < found && filled < MAX_ABILITY_SLOTS; i++) {
            const AbilityDef* ability = ability_get(candidates[i]);
            if (!ability || player->level < ability->unlock_level) continue;
            player->ability_slots[form][filled++] = ability->id;
        }
        for (uint8_t i = filled; i < MAX_ABILITY_SLOTS; i++) {
            player->ability_slots[form][i] = 0;
        }
        player->ability_count[form] = filled;
    }
}

/**
 * Return the seconds remaining on one hotbar slot's cooldown.
 *
 * @return Zero when the slot is ready, out of range, or empty.
 */
float ability_slot_cooldown_remaining(const ActivePlayer* player, uint8_t form, int slot) {
    return (float)player_cooldown_remaining(player, form, slot, get_time());
}

/**
 * Send the ability bar for one form.
 */
void ability_send_form_data(int client_fd, ActivePlayer* player, uint8_t form) {
    if (!player || form >= FORM_COUNT) return;

    AbilityDataPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type         = PACKET_ABILITY_DATA;
    pkt.header.player_id    = htonl(player->character_id);
    pkt.header.payload_size = htons((uint16_t)(sizeof(AbilityDataPacket) - sizeof(PacketHeader)));
    pkt.form                = form;

    uint8_t count = player->ability_count[form];
    if (count > MAX_ABILITY_SLOTS) count = MAX_ABILITY_SLOTS;
    pkt.count = count;

    for (int i = 0; i < count; i++) {
        const AbilityDef* ability = ability_get(player->ability_slots[form][i]);
        if (!ability) continue;

        pkt.slots[i].id        = htons(ability->id);
        strncpy(pkt.slots[i].name, ability->name, 23);
        pkt.slots[i].name[23]  = '\0';
        pkt.slots[i].cooldown  = ability->cooldown;
        pkt.slots[i].cast_time = ability->cast_time;
        pkt.slots[i].resource_cost = (int16_t)ability->resource_cost;
        strncpy(pkt.slots[i].image, ability->image, 31);
        pkt.slots[i].image[31] = '\0';
    }

    server_send(client_fd, &pkt, sizeof(pkt));
}

/**
 * Send the ability bars for both forms.
 *
 * Both are sent, not just the active one, so the client can render the other bar
 * the instant a swap is acknowledged rather than waiting for a round trip.
 */
void ability_send_data(int client_fd, ActivePlayer* player) {
    if (!player) return;

    for (uint8_t form = 0; form < FORM_COUNT; form++) {
        ability_send_form_data(client_fd, player, form);
    }
    printf("[ABILITY] Sent %d/%d slots (human/animal) to player %u\n",
           player->ability_count[FORM_HUMAN], player->ability_count[FORM_ANIMAL],
           player->character_id);
}

/**
 * Reply to a form-swap request with the authoritative state either way.
 *
 * The caller must hold the player's lock.
 */
static void send_form_swap_ack(int client_fd, const ActivePlayer* player, uint8_t accepted) {
    FormSwapAckPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type         = PACKET_FORM_SWAP_ACK;
    pkt.header.player_id    = htonl(player->character_id);
    pkt.header.payload_size = htons((uint16_t)(sizeof(FormSwapAckPacket) - sizeof(PacketHeader)));

    pkt.accepted      = accepted;
    pkt.form          = player->form;
    pkt.resource_type = player->resource_type;
    pkt.resource      = htonl(player->resource);
    pkt.max_resource  = htonl(player->max_resource);

    double swap_wait = player->form_swap_ready_at - get_time();
    pkt.swap_ready_in = swap_wait > 0.0 ? (float)swap_wait : 0.0f;

    for (int i = 0; i < MAX_ABILITY_SLOTS; i++) {
        pkt.ability_ready_in[i] = ability_slot_cooldown_remaining(player, player->form, i);
    }

    server_send(client_fd, &pkt, sizeof(pkt));
}

/**
 * Handle a client's request to swap forms.
 *
 * Nothing here touches a cooldown. They are absolute expiry instants that kept
 * running while the player was in the other form, so swapping out and back cannot
 * produce a shorter wait than never swapping at all.
 */
void ability_handle_form_swap(int client_fd, uint32_t caster_id, uint8_t requested_form) {
    ActivePlayer* player = player_acquire(caster_id);
    if (!player) return;

    if (requested_form >= FORM_COUNT || requested_form == player->form) {
        send_form_swap_ack(client_fd, player, 0);
        player_release(player);
        return;
    }

    /* A swap while stunned or channelling would be a free escape from both. */
    if (player_is_action_locked(player)) {
        send_form_swap_ack(client_fd, player, 0);
        player_release(player);
        return;
    }

    double now = get_time();
    if (now < player->form_swap_ready_at) {
        send_form_swap_ack(client_fd, player, 0);
        player_release(player);
        return;
    }

    player->form = requested_form;
    player->form_swap_ready_at = now + progression_config()->form_swap_cooldown;
    /* Discard any banked regeneration fraction: the accumulator counts upward for
     * mana and stamina and downward for rage, and the swap may change which. */
    int accum_slot = find_player_slot(caster_id);
    if (accum_slot >= 0 && accum_slot < MAX_PLAYERS) g_mana_accum[accum_slot] = 0.0f;

    /* The resource pool follows the form: Human Form has none at all. Recomputing
     * also re-resolves the race passive, which applies in Animal Form only. */
    player_recompute_stats(player);

    int fd = player->client_fd;
    send_form_swap_ack(client_fd, player, 1);
    ability_send_form_data(fd, player, player->form);

    printf("[FORM] Player %u swapped to %s form\n", caster_id,
           requested_form == FORM_ANIMAL ? "animal" : "human");
    player_release(player);
}

/**
 * Validate an ability-cast request and queue its pending cast.
 *
 * Acquires player, NPC-world, and pending-cast locks during validation.
 *
 * @param world      NPC world used when the cast later resolves.
 * @param client_fd  Socket that receives cast status packets.
 * @param caster_id  Character issuing the cast.
 * @param pkt        Decoded cast-intent payload in network byte order where specified.
 */
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

    // snapshot targets before acquiring the caster lock
    float target_x = 0.0f, target_y = 0.0f;
    int   has_range_target = 0;
    if (target_id != 0 && ability->range > 0.0f) {
        pthread_mutex_lock(&g_npc_world.lock);
        for (int i = 0; i < MAX_NPCS; i++) {
            if (g_npc_world.npcs[i].id == target_id && g_npc_world.npcs[i].is_alive) {
                target_x = g_npc_world.npcs[i].pos_x;
                target_y = g_npc_world.npcs[i].pos_y;
                has_range_target = 1;
                break;
            }
        }
        pthread_mutex_unlock(&g_npc_world.lock);

        if (!has_range_target) {
            ActivePlayer* tgt = player_acquire(target_id);
            if (tgt) {
                target_x = tgt->pos_x;
                target_y = tgt->pos_y;
                has_range_target = 1;
                player_release(tgt);
            }
        }
    }

    ActivePlayer* caster = player_acquire(caster_id);
    if (!caster) return;

    // Dead check — dead players cannot cast
    if (caster->is_dead) {
        player_release(caster);
        send_ability_cast_cancel(client_fd, caster_id, ability_id, 3);
        return;
    }

    // Stun check — stunned players cannot cast
    for (int i = 0; i < MAX_ACTIVE_EFFECTS; i++) {
        if (caster->active_effects[i].active &&
            caster->active_effects[i].effect_type == EFFECT_STUN) {
            player_release(caster);
            send_ability_cast_cancel(client_fd, caster_id, ability_id, 3);
            return;
        }
    }

    int player_slot = find_player_slot(caster_id);
    if (player_slot < 0) {
        player_release(caster);
        return;
    }

    /* An ability belongs to a race, except the universal Human Form kit, which
     * belongs to none. Both cases are covered by one check. */
    if (ability->race_id != 0 && ability->race_id != caster->race_id) {
        player_release(caster);
        send_ability_cast_cancel(client_fd, caster_id, ability_id, 3);
        return;
    }

    if (ability->form != caster->form) {
        player_release(caster);
        send_ability_cast_cancel(client_fd, caster_id, ability_id, 3);
        return;
    }

    /* Hibernate locks the bear out of acting for its duration; so does a stun. */
    if (player_is_action_locked(caster)) {
        player_release(caster);
        send_ability_cast_cancel(client_fd, caster_id, ability_id, 3);
        return;
    }

    if (caster->level < ability->unlock_level) {
        player_release(caster);
        send_ability_cast_cancel(client_fd, caster_id, ability_id, 3);
        return;
    }

    int ability_slot = -1;
    if (!player_has_ability(caster, ability_id, &ability_slot)) {
        player_release(caster);
        send_ability_cast_cancel(client_fd, caster_id, ability_id, 3);
        return;
    }

    /* Cooldowns are absolute instants, so this comparison is the whole of "always
     * ticking": nothing decremented them while the player was in the other form, and
     * nothing needs to. */
    if (!player_slot_is_ready(caster, caster->form, ability_slot, get_time())) {
        player_release(caster);
        send_ability_cast_cancel(client_fd, caster_id, ability_id, 3);
        return;
    }

    // Range check for targeted abilities
    if (has_range_target) {
        float dx = caster->pos_x - target_x;
        float dy = caster->pos_y - target_y;
        if (sqrtf(dx * dx + dy * dy) > ability->range) {
            player_release(caster);
            send_ability_cast_cancel(client_fd, caster_id, ability_id, 3);
            return;
        }
    }

    pthread_mutex_lock(&g_ability_casts_lock);
    if (g_ability_casts[player_slot].is_active) {
        pthread_mutex_unlock(&g_ability_casts_lock);
        player_release(caster);
        send_ability_cast_cancel(client_fd, caster_id, ability_id, 2);
        return;
    }

    if (ability->resource_cost > 0 && caster->resource < ability->resource_cost) {
        pthread_mutex_unlock(&g_ability_casts_lock);
        player_release(caster);
        send_ability_cast_cancel(client_fd, caster_id, ability_id, 3);
        return;
    }

    if (ability->resource_cost > 0) {
        caster->resource -= ability->resource_cost;
        send_mana_update(client_fd, caster_id, caster->resource, caster->max_resource);
    }

    float origin_x = caster->pos_x;
    float origin_y = caster->pos_y;

    // Mark combat time for HP regen suppression
    caster->last_combat_time = get_time();

    player_release(caster);

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

/**
 * Cancel a pending cast and refund its resource cost.
 */
void ability_handle_cast_cancel(int client_fd, uint32_t caster_id) {
    int slot = find_player_slot(caster_id);
    if (slot < 0) return;

    pthread_mutex_lock(&g_ability_casts_lock);

    if (g_ability_casts[slot].is_active) {
        uint16_t ability_id = g_ability_casts[slot].ability_id;
        g_ability_casts[slot].is_active = 0;

        const AbilityDef* ability = ability_get(ability_id);
        if (ability && ability->resource_cost > 0) {
            ActivePlayer* caster = player_acquire(caster_id);
            if (caster) {
                caster->resource += ability->resource_cost;
                if (caster->resource > caster->max_resource) {
                    caster->resource = caster->max_resource;
                }
                send_mana_update(client_fd, caster_id, caster->resource, caster->max_resource);
                player_release(caster);
            }
        }

        send_ability_cast_cancel(client_fd, caster_id, ability_id, 0);
    }

    pthread_mutex_unlock(&g_ability_casts_lock);
}

/**
 * Resolve a completed cast against players, NPCs, projectiles, or zones.
 *
 * The caller must hold the pending-cast lock.
 */
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

    /* Snapshot the caster's attributes and modifiers once, then resolve against
     * them. Anything that happens to the caster mid-resolution — a buff expiring, a
     * form swap — must not change the outcome of a cast already committed. */
    int c_stats[STAT_COUNT] = {0};
    int c_wpn = 0;
    float c_form_power = 1.0f;
    DamageModifiers c_mods;
    damage_mods_reset(&c_mods);
    int caster_found = 0;

    /* Track whether any target met the ability's bonus-damage condition, so the
     * caster's own onCondition effects fire once for the cast rather than once per
     * target — Rend costs the wolf one bite of self damage, not one per enemy. */
    uint8_t any_condition_held = 0;

    // --- Movement abilities ---
    float dash_start_x = origin_x;
    float dash_start_y = origin_y;
    int had_movement = 0;

    {
        ActivePlayer* caster = player_acquire(caster_id);
        if (caster) {
            caster_found = 1;
            int ability_slot = -1;
            if (player_has_ability(caster, cast->ability_id, &ability_slot)) {
                /* Store when the slot becomes ready, not how long is left. Dexterity
                 * is attack and cast speed, so it shortens the wait. */
                float cd = ability->cooldown *
                           combat_haste_multiplier(caster->stats[STAT_DEXTERITY]);
                player_start_cooldown(caster, caster->form, ability_slot, get_time(), cd);
            }
            memcpy(c_stats, caster->stats, sizeof(c_stats));
            c_wpn = caster->weapon_damage;
            c_form_power = player_form_power(caster);
            player_collect_modifiers(caster, 0, &c_mods);
            caster->last_combat_time = get_time();

            if (ability->movement.type != MOVEMENT_NONE) {
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
            }
            player_release(caster);
        }
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
        if (caster_found) {
            ActivePlayer* p = player_acquire(caster_id);
            if (p) {
                for (int e = 0; e < ability->effect_count; e++) {
                    apply_effect_to_player(p, &ability->effects[e], caster_id);
                    send_status_effect_apply(client_fd, caster_id,
                                             (uint8_t)ability->effects[e].type,
                                             ability->effects[e].value,
                                             ability->effects[e].duration,
                                             caster_id);
                }
                player_release(p);
            }
        }
        cast->is_active = 0;
        return;
    }

    /* --- Abilities aimed at allies ---
     *
     * Gated on having something to deliver, not on healing specifically. A support
     * ability whose whole payload is an effect — Clear Mind's cleanse, Bounty's
     * resource restore — carries no healing at all, and gating on healing alone made
     * both of them do nothing.
     */
    if (ability->target_type == ABILITY_TARGET_ALLY &&
        (ability->healing > 0 || ability->heal_percent > 0 || ability->effect_count > 0)) {
        if (cast->target_id != 0) {
            ActivePlayer* target = player_acquire(cast->target_id);
            if (target) {
                float d = dist2d(origin_x, origin_y, target->pos_x, target->pos_y);
                if (d <= ability->range || ability->range == 0.0f) {
                    /* Healing scales with Focus and crits on Precision. */
                    int total_heal = (int)((float)ability->healing *
                                           combat_healing_multiplier(c_stats[STAT_FOCUS]) *
                                           c_form_power);
                    total_heal += percent_of_max(target->max_health,
                                                 effect_permille_to_fraction(ability->heal_percent));
                    uint8_t heal_crit = (uint8_t)combat_check_crit(c_stats[STAT_PRECISION]);
                    if (heal_crit) {
                        total_heal = (int)((float)total_heal *
                                           combat_crit_multiplier(c_stats[STAT_FEROCITY]));
                    }

                    if (total_heal > 0) {
                        target->health += total_heal;
                        if (target->health > target->max_health)
                            target->health = target->max_health;
                    }

                    send_ability_effect(client_fd, caster_id, cast->target_id,
                                        cast->ability_id, 0, total_heal,
                                        target->health, 0, heal_crit);

                    for (int e = 0; e < ability->effect_count; e++) {
                        apply_effect_to_player(target, &ability->effects[e], caster_id);
                        send_status_effect_apply(client_fd, cast->target_id,
                                                 (uint8_t)ability->effects[e].type,
                                                 ability->effects[e].value,
                                                 ability->effects[e].duration,
                                                 caster_id);
                    }
                }
                player_release(target);
            }
        } else if (ability->aoe.shape != ABILITY_AOE_NONE) {
            /* Healing scales with Focus and crits on Precision. */
            int base_heal = (int)((float)ability->healing *
                                  combat_healing_multiplier(c_stats[STAT_FOCUS]) *
                                  c_form_power);
            // Roll crit once for the whole AoE pulse — same result for all targets
            uint8_t heal_crit = (uint8_t)combat_check_crit(c_stats[STAT_PRECISION]);
            int total_heal = heal_crit
                ? (int)((float)base_heal * combat_crit_multiplier(c_stats[STAT_FEROCITY]))
                : base_heal;

            player_registry_rdlock();
            int online_count = 0;
            const int* online = player_active_list_locked(&online_count);
            for (int n = 0; n < online_count; n++) {
                int i = online[n];
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
                                            active_players[i].health, 0, heal_crit);
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
            player_registry_unlock();
        }
        cast->is_active = 0;
        return;
    }

    /* --- Enemy-targeted resolution — snapshot under lock, send after unlock ---
     *
     * Gated on reaching enemies at all, not on damage. A pure debuff like Growl deals
     * nothing and exists entirely to put an effect on what it hits; gating on damage
     * meant it never touched an enemy.
     */
    if (ability->damage > 0 ||
        (ability->effect_count > 0 && ability->target_type == ABILITY_TARGET_ENEMY)) {
        float aim_dx = aim_x - origin_x;
        float aim_dy = aim_y - origin_y;

        // Snapshot results under lock
        typedef struct {
            uint32_t npc_id;
            int      damage;
            int      new_health;
            uint8_t  is_kill;
            uint8_t  is_crit;
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

            /* There is no dodge roll: an ability that reaches an NPC connects. */
            uint8_t condition_held = 0;
            uint8_t is_crit = 0;
            int damage = 0;

            /* An ability with no damage of its own deals none: weapon damage rides
             * along with a strike, not with a shout. */
            if (ability->damage > 0) {
                damage = calc_ability_damage(
                    (int)((float)(ability->damage + c_wpn) * c_form_power),
                    &ability->bonus_damage,
                    npc->health, npc->max_health,
                    npc->armor, c_stats, &c_mods,
                    (int)ability->damage_stat, &condition_held, &is_crit);
                if (condition_held) any_condition_held = 1;

                npc->health -= damage;
                if (npc->health < 0) npc->health = 0;
            }

            uint8_t is_kill = (npc->health == 0) ? 1 : 0;
            if (is_kill) {
                npc->is_alive = 0;
                struct timespec _ts;
                clock_gettime(CLOCK_MONOTONIC, &_ts);
                npc->death_time = _ts.tv_sec + _ts.tv_nsec / 1e9;
            }

            for (int e = 0; e < ability->effect_count; e++) {
                /* Effects gated on the execute branch fire only when it did, and
                 * self-directed effects belong to the caster, not the target. */
                if (ability->effects[e].on_condition && !condition_held) continue;
                if (ability->effects[e].self) continue;
                apply_effect_to_npc(npc, &ability->effects[e], caster_id);
            }

            hits[hit_count].npc_id     = npc->id;
            hits[hit_count].damage     = damage;
            hits[hit_count].new_health = npc->health;
            hits[hit_count].is_kill    = is_kill;
            hits[hit_count].is_crit    = is_crit;
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
            /* Every hit connects: there is no dodge roll left to report. */
            send_ability_effect(client_fd, caster_id, hits[h].npc_id,
                                cast->ability_id, hits[h].damage, 0,
                                hits[h].new_health, hits[h].is_kill, hits[h].is_crit);
            printf("[ABILITY] '%s' hit NPC %u for %d dmg%s (hp=%d)%s\n",
                   ability->name, hits[h].npc_id, hits[h].damage,
                   hits[h].is_crit ? " (CRIT)" : "",
                   hits[h].new_health, hits[h].is_kill ? " — KILLED" : "");

            if (hits[h].is_kill) {
                if (hits[h].xp_reward > 0) {
                    party_award_xp(caster_id, hits[h].xp_reward);
                }
                if (hits[h].xp_reward > 0) {
                    ActivePlayer* killer = player_acquire(caster_id);
                    if (killer) {
                        player_send_kill_reward_locked(client_fd, killer,
                                                       (uint32_t)hits[h].xp_reward);
                        player_release(killer);
                    }
                }
                loot_roll(hits[h].npc_type_id, hits[h].npc_x, hits[h].npc_y, caster_id);
            }
        }
    }

    /* --- Effects the caster applies to itself ---
     *
     * This covers Guard and Tough Hide, whose whole payload is a self modifier; the
     * self-heal on Bite; and Rend's execute branch, whose self damage and resistance
     * window fire once for the cast rather than once per enemy struck.
     */
    {
        ActivePlayer* self = player_acquire(caster_id);
        if (self) {
            if (ability->heal_self && ability->heal_percent > 0) {
                int healed = percent_of_max(self->max_health,
                                            effect_permille_to_fraction(ability->heal_percent));
                self->health += healed;
                if (self->health > self->max_health) self->health = self->max_health;

                send_ability_effect(client_fd, caster_id, caster_id, cast->ability_id,
                                    0, healed, self->health, 0, 0);
            }

            for (int e = 0; e < ability->effect_count; e++) {
                const AbilityEffectDef* effect = &ability->effects[e];

                /* An effect goes where the ability points unless it says otherwise.
                 * There used to be a heuristic here sending any buff on an enemy AoE
                 * to the caster, which predates the `self` flag and sent Growl's
                 * armour shred to the wolf instead of to what it growled at. */
                int is_self_directed = effect->self ||
                                       ability->target_type == ABILITY_TARGET_SELF;

                if (!is_self_directed) continue;
                if (effect->on_condition && !any_condition_held) continue;

                apply_effect_to_player(self, effect, caster_id);
                send_status_effect_apply(client_fd, caster_id, (uint8_t)effect->type,
                                         effect->value, effect->duration, caster_id);
            }
            player_release(self);
        }
    }

    cast->is_active = 0;
}

/**
 * Advance casts, cooldowns, status effects, zones, and mana regeneration.
 *
 * @param world       NPC world affected by completed casts and active zones.
 * @param delta_time  Elapsed tick time in seconds.
 */
void ability_tick(NPCWorld* world, double delta_time) {
    float dt = (float)delta_time;
    double now = get_time();

    // resolve completed casts
    pthread_mutex_lock(&g_ability_casts_lock);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        PendingAbilityCast* cast = &g_ability_casts[i];
        if (!cast->is_active) continue;
        double elapsed = now - cast->cast_start_time;
        if (elapsed < cast->cast_duration) continue;
        resolve_cast(cast, world);
    }
    pthread_mutex_unlock(&g_ability_casts_lock);

    /* Cooldowns are not ticked. They are absolute expiry instants on the monotonic
     * clock, so both forms' bars advance on their own with no work per player per
     * frame — which is also exactly why a form swap cannot skip one. */

    // tick status effects
    player_registry_rdlock();
    int online_count = 0;
    const int* online = player_active_list_locked(&online_count);
    for (int n = 0; n < online_count; n++) {
        int i = online[n];
        if (!active_players[i].is_loaded) continue;
        pthread_mutex_lock(&active_players[i].lock);

        for (int e = 0; e < MAX_ACTIVE_EFFECTS; e++) {
            if (!active_players[i].active_effects[e].active) continue;

            active_players[i].active_effects[e].duration_remaining -= dt;

            if (active_players[i].active_effects[e].duration_remaining <= 0.0f) {
                uint8_t was_buff = (active_players[i].active_effects[e].effect_type == EFFECT_BUFF);
                active_players[i].active_effects[e].active = 0;
                StatusEffectRemovePacket pkt = {0};
                pkt.header.type      = PACKET_STATUS_EFFECT_REMOVE;
                pkt.header.player_id = htonl(active_players[i].character_id);
                pkt.header.payload_size = htons(sizeof(StatusEffectRemovePacket) - sizeof(PacketHeader));
                pkt.target_id        = htonl(active_players[i].character_id);
                pkt.effect_type      = active_players[i].active_effects[e].effect_type;
                server_send(active_players[i].client_fd, &pkt, sizeof(pkt));
                /* Rebuild from the race curve so the expired buff's contribution is
                 * gone and every surviving one is still counted. */
                if (was_buff) {
                    player_recompute_stats(&active_players[i]);
                }
                continue;
            }

            uint8_t etype = active_players[i].active_effects[e].effect_type;
            int ticks = (etype == EFFECT_DOT || etype == EFFECT_HOT ||
                         etype == EFFECT_HOT_PERCENT);

            if (ticks && active_players[i].active_effects[e].tick_rate > 0.0f) {
                active_players[i].active_effects[e].tick_remaining -= dt;
                if (active_players[i].active_effects[e].tick_remaining <= 0.0f) {
                    active_players[i].active_effects[e].tick_remaining =
                        active_players[i].active_effects[e].tick_rate;

                    ActivePlayer* player = &active_players[i];
                    int value = player->active_effects[e].value;

                    if (etype == EFFECT_HOT) {
                        player->health += value;
                    } else if (etype == EFFECT_HOT_PERCENT) {
                        /* Percent-of-max healing: the shape Second Wind and Hibernate
                         * both need, and the one a flat integer cannot express. */
                        player->health += percent_of_max(player->max_health,
                                                         effect_permille_to_fraction(value));
                    } else {
                        player->health -= value;
                        // DOTs count as combat for HP regen suppression
                        player->last_combat_time = now;
                        player_add_rage(player, 0, value);
                    }

                    if (player->health > player->max_health) player->health = player->max_health;
                    if (player->health < 0) player->health = 0;
                }
            }
        }

        pthread_mutex_unlock(&active_players[i].lock);
    }
    player_registry_unlock();

    // tick zones
    pthread_mutex_lock(&g_zones_lock);
    for (int z = 0; z < MAX_ZONES; z++) {
        if (!g_zones[z].is_active) continue;

        g_zones[z].duration_remaining -= dt;
        if (g_zones[z].duration_remaining <= 0.0f) {
            player_registry_rdlock();
            for (int p = 0; p < MAX_PLAYERS; p++) {
                if (active_players[p].is_loaded) {
                    send_remove_zone(active_players[p].client_fd, g_zones[z].zone_id);
                }
            }
            player_registry_unlock();
            g_zones[z].is_active = 0;
            continue;
        }

        g_zones[z].tick_timer -= dt;
        if (g_zones[z].tick_timer <= 0.0f) {
            g_zones[z].tick_timer = g_zones[z].tick_rate;

            player_registry_rdlock();
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
            player_registry_unlock();

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

    /* Regenerate the resource pool. Mana and stamina refill toward their maximum;
     * rage runs the other way, building through combat and decaying out of it, so it
     * is handled by the same loop with the sign reversed rather than by a second one. */
    player_registry_rdlock();
    online = player_active_list_locked(&online_count);
    for (int n = 0; n < online_count; n++) {
        int i = online[n];
        if (!active_players[i].is_loaded) continue;

        pthread_mutex_lock(&active_players[i].lock);

        ActivePlayer* player = &active_players[i];
        int changed = 0;

        if (player->resource_type == RESOURCE_RAGE) {
            const ProgressionConfig* cfg = progression_config();
            double idle = get_time() - player->last_combat_time;

            if (idle >= cfg->rage_decay_delay && player->resource > 0) {
                g_mana_accum[i] -= (float)(cfg->rage_decay_per_second * dt);
                if (g_mana_accum[i] <= -1.0f) {
                    int decay = (int)(-g_mana_accum[i]);
                    g_mana_accum[i] += (float)decay;

                    player->resource -= decay;
                    if (player->resource < 0) player->resource = 0;
                    changed = 1;
                }
            }
        } else if (player->max_resource > 0 && player->resource < player->max_resource) {
            StatId governing = (player->resource_type == RESOURCE_STAMINA)
                ? STAT_STAMINA_CAPACITY : STAT_FOCUS;

            g_mana_accum[i] += combat_resource_regen_rate(player->resource_type,
                                                          player->stats[governing]) * dt;
            if (g_mana_accum[i] >= 1.0f) {
                int regen = (int)g_mana_accum[i];
                g_mana_accum[i] -= (float)regen;

                player->resource += regen;
                if (player->resource > player->max_resource) {
                    player->resource = player->max_resource;
                }
                changed = 1;
            }
        }

        if (changed) {
            send_mana_update(player->client_fd, player->character_id,
                             player->resource, player->max_resource);
        }

        pthread_mutex_unlock(&active_players[i].lock);
    }
    player_registry_unlock();
}
