/**
 * @file
 * Perform the work an NPC's think phase queued, once every lock is released.
 *
 * The other half of npc_deferred.h. Everything here touches something the acting
 * NPC's slot lock does not cover -- a player's health and effect slots, an ally's
 * buff slots, the zone pool, the NPC pool's write lock for a summon -- which is
 * the whole reason the queue exists rather than the think phase doing it inline.
 *
 * Player positions come from the tick snapshot rather than from live slots. An
 * earlier version read pos_x, pos_y, is_dead and client_fd straight out of
 * active_players[] holding only the registry read lock, which is not the lock
 * that protects those fields -- and this is the code that decides whether an NPC
 * ability hits you. Survivors are resolved through player_acquire_slot(), which
 * re-checks that the slot still holds the character the snapshot named.
 */

#include "npc_deferred.h"
#include "npc_effects.h"
#include "npc_geometry.h"
#include "npc_registry.h"
#include "npc_summon.h"
#include "npc_triggers.h"
#include "projectile.h"
#include "player_data.h"
#include "player_effects.h"
#include "damage_model.h"
#include "combat_stats.h"
#include "zone_owner.h"
#include "log.h"
#include "utils.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <time.h>

extern ActivePlayer active_players[];

/** Radius within which a telegraph start or resolve packet is broadcast. */
#define TELEGRAPH_BROADCAST_RADIUS 500.0f

/** Bound one ally-directed action's recipient list. */
#define NPC_ALLY_TARGETS 32

static double get_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static float dist2d(float ax, float ay, float bx, float by) {
    float dx = bx - ax, dy = by - ay;
    return sqrtf(dx * dx + dy * dy);
}

void npc_deferred_push(NPCDeferredQueue* q, const NPCDeferredAction* item) {
    if (!q || !item) return;
    if (q->count < q->capacity) {
        q->items[q->count++] = *item;
        return;
    }
    /* Only reachable when the sizing allocation failed, and never silently: a
     * dropped resolve is a telegraph that warns and then never lands. */
    LOG_WARN_RL(5, 60, "[NPC_AI] deferred queue full at %d actions -- "
                       "an NPC ability was dropped this tick", q->capacity);
}

/* --- Packets -------------------------------------------------------------- */

/** Send one status-effect application to a client. */
static void send_effect_apply(int client_fd, uint32_t target_id,
                              const AbilityEffectDef* e, uint32_t source_id) {
    StatusEffectApplyPacket pkt = {0};
    pkt.header.type         = PACKET_STATUS_EFFECT_APPLY;
    pkt.header.player_id    = htonl(target_id);
    pkt.header.payload_size = htons(sizeof(pkt) - sizeof(PacketHeader));
    pkt.target_id   = htonl(target_id);
    pkt.effect_type = (uint8_t)e->type;
    pkt.value       = htonl((uint32_t)e->value);
    pkt.duration    = e->duration;
    pkt.source_id   = htonl(source_id);
    server_send(client_fd, &pkt, sizeof(pkt));
}

/** Send one NPC-dealt hit to the player who took it. */
static void send_ability_effect(int client_fd, uint32_t npc_id, uint32_t char_id,
                                uint16_t ability_id, int damage, int new_hp,
                                uint8_t is_kill) {
    AbilityEffectPacket pkt = {0};
    pkt.header.type         = PACKET_ABILITY_EFFECT;
    pkt.header.player_id    = htonl(npc_id);
    pkt.header.payload_size = htons(sizeof(pkt) - sizeof(PacketHeader));
    pkt.caster_id         = htonl(npc_id);
    pkt.target_id         = htonl(char_id);
    pkt.ability_id        = htons(ability_id);
    pkt.damage            = htonl((uint32_t)damage);
    pkt.healing           = 0;
    pkt.target_new_health = htonl((uint32_t)new_hp);
    pkt.is_kill           = is_kill;
    server_send(client_fd, &pkt, sizeof(pkt));
}

/* --- Damage against players ----------------------------------------------- */

/** Apply one NPC hit to one player, with its effects, and report a kill.
 *
 * @return 1 when the hit killed the player.
 */
static int hit_player(TickSnapshot* snap, int dense, uint32_t npc_id,
                      uint16_t ability_id, int base_damage,
                      int effect_first, int effect_count) {
    /* Takes the slot lock to mutate, and lets it reject a slot recycled to
     * another character since the snapshot was built. */
    ActivePlayer* target = player_acquire_slot(snap->slot[dense],
                                               snap->character_id[dense]);
    if (!target) return 0;

    int damage = base_damage;
    if (damage > 0) {
        int variance = (rand() % 21) - 10;
        damage += (damage * variance) / 100;

        /* The player's own mitigation: armour, the race passive if they are in
         * Animal Form, and any active percentage reducers. */
        DamageModifiers mods;
        player_collect_modifiers(target, 0, &mods);
        damage = damage_resolve(damage, NULL, &mods);

        target->health -= damage;
        if (target->health < 0) target->health = 0;
        target->last_combat_time = get_time();
        player_add_rage(target, 0, damage);
    } else {
        damage = 0;
    }

    for (int e = 0; e < effect_count; e++) {
        const AbilityEffectDef* def = npc_registry_effect(effect_first + e);
        if (!def || def->self) continue;
        player_effect_apply(target, def, npc_id);
    }

    int new_hp = target->health;
    uint8_t is_kill = (new_hp == 0) ? 1 : 0;
    int client_fd = target->client_fd;
    uint32_t char_id = target->character_id;
    player_release(target);

    send_ability_effect(client_fd, npc_id, char_id, ability_id, damage, new_hp, is_kill);
    for (int e = 0; e < effect_count; e++) {
        const AbilityEffectDef* def = npc_registry_effect(effect_first + e);
        if (!def || def->self) continue;
        send_effect_apply(client_fd, char_id, def, npc_id);
    }
    return is_kill;
}

/** Record that an ability of this NPC's touched nobody, for `ability_missed`.
 *
 * Wagon Breaker's Armor Break is "two charges without a hit", and every
 * committed attack in the roster is something a player can make miss -- so the
 * whiff is worth counting generally rather than for that one enemy.
 */
static void note_miss(NPCWorld* world, uint32_t npc_id) {
    NPCEntity* npc = npc_world_acquire(world, npc_id);
    if (!npc) return;
    npc->miss_events++;
    npc_world_release(world, npc);
}

/** Record that an NPC killed someone, for its `on_kill` triggers. */
static void note_kill(NPCWorld* world, uint32_t npc_id) {
    NPCEntity* npc = npc_world_acquire(world, npc_id);
    if (!npc) return;
    npc_trigger_note_kill(npc);
    npc_world_release(world, npc);
}

/* --- Action handlers ------------------------------------------------------ */

static void do_telegraph_start(const NPCDeferredAction* d, TickSnapshot* snap,
                               int* nearby) {
    NPCTelegraphStartPacket pkt = {0};
    pkt.header.type         = PACKET_NPC_TELEGRAPH_START;
    pkt.header.player_id    = 0;
    pkt.header.payload_size = htons(sizeof(pkt) - sizeof(PacketHeader));
    pkt.npc_id     = htonl(d->npc_id);
    pkt.ability_id = htons(d->tstart.ability_id);
    pkt.shape      = d->tstart.shape;
    pkt.pos_x      = d->tstart.pos_x;
    pkt.pos_y      = d->tstart.pos_y;
    pkt.dir_x      = d->tstart.dir_x;
    pkt.dir_y      = d->tstart.dir_y;
    pkt.radius     = d->tstart.radius;
    pkt.angle      = d->tstart.angle;
    pkt.width      = d->tstart.width;
    pkt.length     = d->tstart.length;
    pkt.cast_time  = d->tstart.cast_time;

    int n = tick_snapshot_query(snap, d->npc_x, d->npc_y,
                                TELEGRAPH_BROADCAST_RADIUS, nearby, MAX_PLAYERS);
    for (int k = 0; k < n; k++)
        server_send(snap->client_fd[nearby[k]], &pkt, sizeof(pkt));
}

static void do_telegraph_resolve(const NPCDeferredAction* d, NPCWorld* world,
                                 TickSnapshot* snap, int* nearby) {
    NPCTelegraphResolvePacket rpkt = {0};
    rpkt.header.type         = PACKET_NPC_TELEGRAPH_RESOLVE;
    rpkt.header.player_id    = 0;
    rpkt.header.payload_size = htons(sizeof(rpkt) - sizeof(PacketHeader));
    rpkt.npc_id     = htonl(d->npc_id);
    rpkt.ability_id = htons(d->tresolve.ability_id);

    int n = tick_snapshot_query(snap, d->npc_x, d->npc_y,
                                TELEGRAPH_BROADCAST_RADIUS, nearby, MAX_PLAYERS);
    for (int k = 0; k < n; k++)
        server_send(snap->client_fd[nearby[k]], &rpkt, sizeof(rpkt));

    /* A feint resolves into nothing but still shows both packets, which is what
     * makes it a feint rather than a bug: the player sees the wind-up and the
     * release and has already dodged. */
    if (!d->tresolve.deals_damage) return;

    /* The query radius is the shape's farthest reach from its own origin, so it
     * is a superset of what point_in_telegraph can accept -- the broadcast radius
     * above is a different centre and cannot stand in for it. */
    float rect_reach = sqrtf(d->tresolve.length * d->tresolve.length +
                             (d->tresolve.width * 0.5f) * (d->tresolve.width * 0.5f));
    float extent = d->tresolve.radius > rect_reach ? d->tresolve.radius : rect_reach;

    int hits = tick_snapshot_query(snap, d->tresolve.pos_x, d->tresolve.pos_y,
                                   extent, nearby, MAX_PLAYERS);
    int kills = 0, landed = 0;

    for (int k = 0; k < hits; k++) {
        int di = nearby[k];
        if (snap->is_dead[di]) continue;

        if (!point_in_telegraph(snap->pos_x[di], snap->pos_y[di],
                                d->tresolve.shape,
                                d->tresolve.pos_x, d->tresolve.pos_y,
                                d->tresolve.dir_x, d->tresolve.dir_y,
                                d->tresolve.radius, d->tresolve.angle,
                                d->tresolve.width, d->tresolve.length))
            continue;

        landed++;
        kills += hit_player(snap, di, d->npc_id, d->tresolve.ability_id,
                            d->tresolve.damage,
                            d->tresolve.effect_first, d->tresolve.effect_count);
    }

    if (kills)   note_kill(world, d->npc_id);
    if (!landed) note_miss(world, d->npc_id);
}

static void do_projectile(const NPCDeferredAction* d) {
    ProjectileSpawnInfo info = {0};
    info.owner_type  = PROJECTILE_OWNER_NPC;
    info.owner_id    = d->npc_id;
    info.ability_id  = d->proj.ability_id;
    info.owner_fd    = -1;
    info.origin_x    = d->proj.origin_x;
    info.origin_y    = d->proj.origin_y;
    info.aim_x       = d->proj.target_x;
    info.aim_y       = d->proj.target_y;
    info.speed       = d->proj.speed;
    info.width       = d->proj.width;
    info.max_range   = d->proj.range;
    info.damage      = d->proj.damage;
    info.damage_type = (AbilityDamageType)d->proj.damage_type;

    /* V3: effects travel with the projectile and land on impact. The old code
     * left this array empty and hardcoded physical damage, so every effect an
     * NPC projectile was authored to carry did nothing. */
    info.effect_count = 0;
    for (int e = 0; e < d->proj.effect_count && info.effect_count < MAX_ABILITY_EFFECTS; e++) {
        const AbilityEffectDef* def = npc_registry_effect(d->proj.effect_first + e);
        if (def) info.effects[info.effect_count++] = *def;
    }

    projectile_spawn(&info);
}

static void do_contact(const NPCDeferredAction* d, NPCWorld* world,
                       TickSnapshot* snap, int* nearby) {
    /* V1. No telegraph and no projectile: a contact attack lands on whoever is
     * touching this NPC when its cooldown comes up, which is what makes a swarm
     * dangerous to stand in rather than dangerous to read. */
    int n = tick_snapshot_query(snap, d->npc_x, d->npc_y, d->contact.reach,
                                nearby, MAX_PLAYERS);
    int kills = 0, landed = 0;

    for (int k = 0; k < n; k++) {
        int di = nearby[k];
        if (snap->is_dead[di]) continue;
        if (dist2d(d->npc_x, d->npc_y, snap->pos_x[di], snap->pos_y[di]) > d->contact.reach)
            continue;

        landed++;
        kills += hit_player(snap, di, d->npc_id, d->contact.ability_id,
                            d->contact.damage,
                            d->contact.effect_first, d->contact.effect_count);
    }

    if (kills)   note_kill(world, d->npc_id);
    if (!landed) note_miss(world, d->npc_id);
}

/** Apply one ally-directed action to one NPC, resolved through the snapshot. */
static void buff_one(NPCWorld* world, int slot, uint32_t expect_id,
                     const NPCDeferredAction* d) {
    NPCEntity* npc = npc_world_acquire_slot(world, slot, expect_id);
    if (!npc) return;
    if (!npc->is_alive) { npc_world_release(world, npc); return; }

    for (int e = 0; e < d->buff.effect_count; e++) {
        const AbilityEffectDef* def = npc_registry_effect(d->buff.effect_first + e);
        if (!def) continue;
        AbilityEffectDef copy = *def;
        /* The buff block's duration overrides the effect's own when it states
         * one, so an aura's length is written once where a reader looks for it
         * rather than repeated on each of its effects. */
        if (d->buff.duration > 0.0f) copy.duration = d->buff.duration;
        npc_effect_apply(world, slot, npc, &copy, d->npc_id);
    }

    /* V12: forcing an ally's action rather than modifying its stats. Handler's
     * Command and Packmaster's Direct are the whole reason these are flags -- a
     * wolf that lunges *now* is a different threat from a wolf that lunges 15%
     * faster on its own schedule. */
    if (d->buff.ready_allies) {
        double* cds = npc_world_cooldowns(world, slot);
        int n = npc_world_max_abilities(world);
        /* Zero is "never used", which every readiness test reads as ready. */
        for (int i = 0; i < n && cds; i++) cds[i] = 0.0;
        npc->recover_until = 0.0;
    }
    if (d->buff.retarget_allies && d->buff.target_id)
        npc->ai_target_id = d->buff.target_id;

    npc_world_release(world, npc);
}

static void do_buff(const NPCDeferredAction* d, NPCWorld* world, NPCTickSnapshot* npcs) {
    /* An aura reaches its caster as well as its allies -- Rally Howl buffs the
     * wolf that howled. A self-only buff stops there. */
    if (d->buff.target_self || d->buff.radius <= 0.0f) {
        buff_one(world, d->npc_slot, d->npc_id, d);
        if (d->buff.target_self) return;
    }
    if (!npcs || d->buff.radius <= 0.0f) return;

    int idx[NPC_ALLY_TARGETS];
    int n = npc_allies_near(npcs, d->npc_x, d->npc_y, d->buff.radius, d->npc_id,
                            d->buff.faction_only ? d->faction_index : -1,
                            idx, NPC_ALLY_TARGETS);
    /* Tether links one ally, not the pack; the query is nearest-first, so the
     * first result is the one it means. */
    if (d->buff.single_ally && n > 1) n = 1;

    for (int k = 0; k < n; k++)
        buff_one(world, npcs->slot[idx[k]], npcs->id[idx[k]], d);
}

/** Heal one NPC by a flat amount and a percentage of its maximum. */
static void heal_one(NPCWorld* world, int slot, uint32_t expect_id,
                     int amount, int percent) {
    NPCEntity* npc = npc_world_acquire_slot(world, slot, expect_id);
    if (!npc) return;
    if (npc->is_alive) {
        npc->health += amount;
        if (percent > 0)
            npc->health += percent_of_max(npc->max_health,
                                          effect_permille_to_fraction(percent));
        if (npc->health > npc->max_health) npc->health = npc->max_health;
    }
    npc_world_release(world, npc);
}

static void do_heal(const NPCDeferredAction* d, NPCWorld* world, NPCTickSnapshot* npcs) {
    if (d->heal.target_self || d->heal.radius <= 0.0f) {
        heal_one(world, d->npc_slot, d->npc_id, d->heal.amount, d->heal.percent);
        return;
    }
    if (!npcs) return;

    if (d->heal.lowest_ally_only) {
        int c = npc_lowest_health_ally(npcs, d->npc_x, d->npc_y, d->heal.radius,
                                       d->npc_id, d->faction_index);
        if (c >= 0)
            heal_one(world, npcs->slot[c], npcs->id[c], d->heal.amount, d->heal.percent);
        return;
    }

    int idx[NPC_ALLY_TARGETS];
    int n = npc_allies_near(npcs, d->npc_x, d->npc_y, d->heal.radius, d->npc_id,
                            d->faction_index, idx, NPC_ALLY_TARGETS);
    for (int k = 0; k < n; k++)
        heal_one(world, npcs->slot[idx[k]], npcs->id[idx[k]],
                 d->heal.amount, d->heal.percent);
}

static void do_zone(const NPCDeferredAction* d) {
    AbilityEffectDef effects[MAX_ABILITY_EFFECTS];
    int count = 0;
    for (int e = 0; e < d->zone.effect_count && count < MAX_ABILITY_EFFECTS; e++) {
        const AbilityEffectDef* def = npc_registry_effect(d->zone.effect_first + e);
        if (def) effects[count++] = *def;
    }

    int n = d->zone.count > 0 ? d->zone.count : 1;
    for (int i = 0; i < n; i++) {
        ZoneSpawnInfo info = {0};
        info.owner_type   = ZONE_OWNER_NPC;
        info.caster_id    = d->npc_id;
        info.ability_id   = d->zone.ability_id;
        info.radius       = d->zone.radius;
        info.duration     = d->zone.duration;
        info.tick_rate    = d->zone.tick_rate;
        info.effects      = effects;
        info.effect_count = count;

        if (n == 1 || d->zone.spread <= 0.0f) {
            info.pos_x = d->zone.pos_x;
            info.pos_y = d->zone.pos_y;
        } else {
            /* A multi-zone cast scatters rather than stacking: five puddles in
             * one spot are one puddle with five times the bookkeeping. */
            float angle = (float)(2.0 * M_PI) * (float)i / (float)n;
            info.pos_x = d->zone.pos_x + cosf(angle) * d->zone.spread;
            info.pos_y = d->zone.pos_y + sinf(angle) * d->zone.spread;
        }
        zone_create(&info);
    }
}

static void do_summon(const NPCDeferredAction* d, NPCWorld* world) {
    int lo = d->summon.count_min > 0 ? d->summon.count_min : 1;
    int hi = d->summon.count_max > lo ? d->summon.count_max : lo;
    int count = lo + (hi > lo ? rand() % (hi - lo + 1) : 0);

    npc_summon_group(world, d->npc_id, d->npc_x, d->npc_y,
                     d->summon.type_index, d->summon.type_count,
                     count, d->summon.spread);
}

/* --- Entry point ---------------------------------------------------------- */

void npc_deferred_flush(NPCDeferredQueue* q, NPCWorld* world,
                        TickSnapshot* players, NPCTickSnapshot* npcs) {
    if (!q || !players) { if (q) q->count = 0; return; }

    /* Retained rather than a stack array: MAX_PLAYERS ints is kilobytes, and
     * only the gameplay thread reaches this. */
    static int nearby[MAX_PLAYERS];

    for (int i = 0; i < q->count; i++) {
        const NPCDeferredAction* d = &q->items[i];
        switch (d->type) {
            case NPC_ACT_TELEGRAPH_START:   do_telegraph_start(d, players, nearby); break;
            case NPC_ACT_TELEGRAPH_RESOLVE: do_telegraph_resolve(d, world, players, nearby); break;
            case NPC_ACT_PROJECTILE:   do_projectile(d); break;
            case NPC_ACT_CONTACT:            do_contact(d, world, players, nearby); break;
            case NPC_ACT_BUFF:           do_buff(d, world, npcs); break;
            case NPC_ACT_HEAL:           do_heal(d, world, npcs); break;
            case NPC_ACT_ZONE:               do_zone(d); break;
            case NPC_ACT_SUMMON:             do_summon(d, world); break;
            default: break;
        }
    }

    q->count = 0;
}
