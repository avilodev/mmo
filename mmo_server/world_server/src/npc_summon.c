/**
 * @file
 * Spawn registry types, parent summons to their summoner, and reap the orphans.
 *
 * The interesting part is the reaping, and it is interesting because of the
 * pool's lock order. A summon has to disappear when its summoner does, and the
 * obvious implementation -- each summon checks its parent while holding its own
 * slot -- is exactly the two-slots-at-once the pool forbids. So the AI tick
 * collects (child, parent) pairs while it scans, and this runs afterwards, with
 * no lock held, taking the write lock the removal needs.
 */

#include "npc_summon.h"
#include "npc_registry.h"
#include "log.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_per_parent = SUMMON_PER_PARENT_DEFAULT;

void npc_summon_configure(int per_parent) {
    if (per_parent <= 0)                     per_parent = SUMMON_PER_PARENT_DEFAULT;
    if (per_parent > SUMMON_PER_PARENT_MAX)  per_parent = SUMMON_PER_PARENT_MAX;
    g_per_parent = per_parent;
    LOG_INFO("[SUMMON] budget: %d live summons per parent", g_per_parent);
}

int npc_summon_per_parent(void) { return g_per_parent; }

uint32_t npc_spawn_from_type(NPCWorld* world, int type_index,
                             float x, float y, uint32_t parent_id,
                             int affix_index) {
    const NPCTypeDef* t = npc_type_at(type_index);
    if (!t) return 0;

    const NPCArchetypeDef* arch = npc_archetype_at(t->archetype_index);
    float hitbox = t->hitbox_radius > 0.0f ? t->hitbox_radius
                 : (arch ? arch->hitbox_radius : 16.0f);

    /* An affix renames what it composes onto, so a player can tell a Duelist
     * Rusher from a Rusher before it starts casting differently at them. */
    const NPCAffixDef* affix = (affix_index >= 0) ? npc_affix_at(affix_index) : NULL;

    /* Composed in a buffer wide enough for the registry's fields, then copied
     * into the entity's narrower one. The entity's name is what goes on the
     * wire and is 32 bytes; a composed name longer than that is truncated, which
     * is a display consequence and not a correctness one -- but it is done in
     * one explicit step rather than left to a format-string surprise. */
    char composed[NPC_NAME_MAX * 3];
    if (affix && (affix->name_prefix[0] || affix->name_suffix[0])) {
        int n = snprintf(composed, sizeof(composed), "%s%s%s",
                         affix->name_prefix, t->name, affix->name_suffix);
        (void)n;
    } else {
        snprintf(composed, sizeof(composed), "%s", t->name);
    }

    char name[32];
    strncpy(name, composed, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';

    uint32_t id = npc_world_spawn(world, name, x, y, t->health, hitbox,
                                  0, 0, t->id, 0.0f, NPC_CATEGORY_HOSTILE);
    if (!id) return 0;

    NPCEntity* npc = npc_world_acquire(world, id);
    if (!npc) return id;

    npc->armor         = t->armor;
    npc->xp_reward     = (uint32_t)t->xp_reward;
    npc->parent_npc_id = parent_id;
    npc->affix_index   = (int16_t)affix_index;
    npc->stealth_active = t->stealth;

    /* A summon pays out nothing. A summoner whose adds granted XP is an
     * infinite faucet, and it is the first thing a player finds. */
    if (t->summon_only || parent_id != 0) {
        npc->no_reward = 1;
        npc->xp_reward = 0;
    }

    if (affix) {
        npc->mod_cast_time_pct    += affix->cast_time_pct;
        npc->mod_damage_pct       += affix->damage_pct;
        npc->mod_move_speed_pct   += affix->move_speed_pct;
        npc->mod_attack_speed_pct += affix->attack_speed_pct;
        npc->mod_damage_taken_pct += affix->damage_taken_pct;
    }

    npc_world_release(world, npc);
    return id;
}

int npc_summon_group(NPCWorld* world, uint32_t parent_id,
                     float x, float y,
                     const int* type_index, int type_count,
                     int count, float spread) {
    if (!type_index || type_count <= 0 || count <= 0) return 0;

    /* Read and reserve the parent's budget in one acquisition, so two summons
     * resolving in the same flush cannot both see room for the last slot. */
    int room = g_per_parent;
    if (parent_id) {
        NPCEntity* parent = npc_world_acquire(world, parent_id);
        if (!parent) return 0;
        room = g_per_parent - (int)parent->summon_children;
        if (room > count) room = count;
        if (room < 0) room = 0;
        parent->summon_children = (uint16_t)(parent->summon_children + room);
        npc_world_release(world, parent);
    } else if (room > count) {
        room = count;
    }

    if (room <= 0) {
        LOG_DEBUG("[SUMMON] NPC %u is at its summon budget of %d", parent_id, g_per_parent);
        return 0;
    }

    int spawned = 0;
    for (int i = 0; i < room; i++) {
        float angle = (float)(2.0 * M_PI) * (float)i / (float)room;
        float r = spread > 0.0f ? spread * (0.5f + 0.5f * ((float)(rand() % 100) / 100.0f))
                                : 0.0f;
        float sx = x + cosf(angle) * r;
        float sy = y + sinf(angle) * r;

        if (npc_spawn_from_type(world, type_index[i % type_count], sx, sy,
                                parent_id, -1))
            spawned++;
    }

    /* Hand back what did not spawn, so a failed allocation does not permanently
     * shrink this summoner's budget. */
    if (parent_id && spawned < room) {
        NPCEntity* parent = npc_world_acquire(world, parent_id);
        if (parent) {
            int give_back = room - spawned;
            parent->summon_children = (uint16_t)(parent->summon_children >= give_back
                                     ? parent->summon_children - give_back : 0);
            npc_world_release(world, parent);
        }
    }

    return spawned;
}

int npc_summon_reap(NPCWorld* world, const uint32_t* children,
                    const uint32_t* parents, const uint8_t* alive, int count) {
    int reaped = 0;

    for (int i = 0; i < count; i++) {
        int gone = 1;

        if (alive[i]) {
            NPCEntity* parent = npc_world_acquire(world, parents[i]);
            if (parent) {
                gone = !parent->is_alive;
                npc_world_release(world, parent);
            }
        }
        /* A summon that died stays dead: it has no respawn timer, and leaving the
         * corpse would hold a pool slot for the rest of the world's life. */

        if (!gone) continue;
        npc_world_remove(world, children[i]);
        reaped++;
    }

    /* Recount each summoner's live children from what actually survived. The
     * list is one entry per summon in the world, which is small enough that a
     * quadratic tally costs less than the map it would otherwise need. */
    for (int i = 0; i < count; i++) {
        if (!alive[i]) continue;

        int seen_before = 0;
        for (int j = 0; j < i; j++)
            if (alive[j] && parents[j] == parents[i]) { seen_before = 1; break; }
        if (seen_before) continue;

        int live = 0;
        for (int j = i; j < count; j++)
            if (alive[j] && parents[j] == parents[i]) live++;

        NPCEntity* parent = npc_world_acquire(world, parents[i]);
        if (!parent) continue;
        parent->summon_children = (uint16_t)live;
        npc_world_release(world, parent);
    }

    if (reaped)
        LOG_DEBUG("[SUMMON] despawned %d summon(s): dead, or their summoner is gone",
                  reaped);
    return reaped;
}
