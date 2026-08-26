/**
 * @file
 * Decide who may take a quest, grant it, advance it, and hand it in.
 *
 * This is the behaviour layer: what a quest *is* lives in quest_registry.h, and
 * what a character has done with one lives in quest_storage.h.
 */

#include "quest_system.h"
#include "str_fixed.h"
#include "log.h"
#include "player_data.h"
#include "items_database.h"
#include "npc_world.h"
#include "utils.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/socket.h>
#include <arpa/inet.h>

extern NPCWorld g_npc_world;

/* --- Eligibility --------------------------------------------------------- */

/**
 * Report whether a character may take a quest right now.
 */
int quest_is_available_for(const QuestDef* q, const ActivePlayer* p) {
    return quest_is_available_at(q, p, time(NULL));
}

int quest_is_available_at(const QuestDef* q, const ActivePlayer* p, time_t now) {
    if (!q || !p) return 0;

    /* Holding it always blocks it. Repeatable is not the same as duplicable:
     * two copies of one quest would share one progress record. */
    if (quest_log_find((PlayerQuestLog*)&p->quests.log, q->quest_id)) return 0;

    /* Having finished it normally ends it, and for a repeatable is where the
     * reset gate takes over. A character with history but no counter -- every
     * character saved before counters existed -- reads as due rather than
     * stuck, which is the safe direction for a migration to fail in. */
    if (quest_history_contains(&p->quests.history, q->quest_id)) {
        const QuestCounter* c = quest_counters_find(&p->quests.counters, q->quest_id);
        if (!quest_repeat_allows(q->repeat_mode,
                                 c ? c->count  : 0,
                                 c ? c->window : 0,
                                 q->max_count, now))
            return 0;
    }

    if (q->require_race_id != 0 && p->race_id != q->require_race_id) return 0;
    if (q->require_level > 0 && p->level < q->require_level) return 0;

    for (int i = 0; i < q->require_all_count; i++)
        if (!quest_history_contains(&p->quests.history, q->require_all[i])) return 0;

    if (q->require_any_count > 0) {
        int any = 0;
        for (int i = 0; i < q->require_any_count && !any; i++)
            any = quest_history_contains(&p->quests.history, q->require_any[i]);
        if (!any) return 0;
    }

    return 1;
}

/* --- Packets ------------------------------------------------------------- */

/**
 * Fill one objective's wire record, including where the player should go.
 *
 * The marker is resolved from the live NPC pool unless the quest file pinned
 * one, which is what keeps quest data free of copied coordinates.
 *
 * @param from_x, from_y  The character's position; the nearest target wins.
 */
static void fill_objective_info(QuestObjectiveInfo* out, const QuestObjectiveDef* def,
                                float from_x, float from_y) {
    memset(out, 0, sizeof(*out));

    strncpy(out->description, def->description, sizeof(out->description) - 1);
    out->required       = htonl(def->required_count);
    out->objective_type = def->type;
    out->target_id      = htonl(def->target_id);

    float marker_x = def->marker_x;
    float marker_y = def->marker_y;
    int   located  = def->has_marker;

    if (!located &&
        (def->type == QUEST_OBJECTIVE_TALK || def->type == QUEST_OBJECTIVE_KILL) &&
        def->target_id > 0 && def->target_id <= UINT16_MAX) {
        located = npc_world_nearest_of_type(&g_npc_world, (uint16_t)def->target_id,
                                            from_x, from_y, &marker_x, &marker_y);
    }

    if (located) {
        out->has_marker = 1;
        out->marker_x   = marker_x;
        out->marker_y   = marker_y;
    }
}

/**
 * Send one objective's progress.
 */
static void send_progress(int client_fd, uint32_t character_id, uint32_t quest_id,
                          uint8_t obj_index, int32_t current, int32_t required) {
    QuestProgressPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type         = PACKET_QUEST_PROGRESS;
    pkt.header.player_id    = htonl(character_id);
    pkt.header.payload_size = htons(sizeof(pkt) - sizeof(PacketHeader));
    pkt.quest_id  = htonl(quest_id);
    pkt.obj_index = obj_index;
    pkt.current   = htonl(current);
    pkt.required  = htonl(required);
    server_send(client_fd, &pkt, sizeof(pkt));
}

/**
 * Send a quest definition and its current objective progress.
 *
 * @param from_x, from_y  The character's position, used to place markers.
 */
static void send_quest_accept_packet(int client_fd, uint32_t character_id,
                                     const QuestDef* q, const PlayerQuestEntry* pq,
                                     float from_x, float from_y) {
    QuestAcceptPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type         = PACKET_QUEST_ACCEPT;
    pkt.header.player_id    = htonl(character_id);
    pkt.header.payload_size = htons(sizeof(pkt) - sizeof(PacketHeader));
    pkt.quest_id  = htonl(q->quest_id);
    STR_COPY_FIELD(pkt.title, q->title);
    pkt.obj_count = q->obj_count;

    for (int i = 0; i < q->obj_count && i < MAX_QUEST_OBJECTIVES; i++)
        fill_objective_info(&pkt.objectives[i], &q->objectives[i], from_x, from_y);

    server_send(client_fd, &pkt, sizeof(pkt));

    for (int i = 0; i < q->obj_count && i < MAX_QUEST_OBJECTIVES; i++)
        send_progress(client_fd, character_id, q->quest_id, (uint8_t)i,
                      pq->progress[i], q->objectives[i].required_count);
}

/* --- Accepting and turning in -------------------------------------------- */

int quest_player_accept(uint32_t character_id, int client_fd, uint32_t quest_id) {
    const QuestDef* q = quest_get(quest_id);
    if (!q) {
        LOG_DEBUG("[QUEST] quest_player_accept: unknown quest %u", quest_id);
        return 0;
    }

    ActivePlayer* p = player_acquire(character_id);
    if (!p) return 0;

    /* The same rule the dialogue asked before offering it. Checked again here
     * because an offer is not the only way in: a fabricated accept packet
     * reaches this function without passing a conversation at all. */
    if (!quest_is_available_for(q, p)) {
        int already = quest_log_find(&p->quests.log, quest_id) != NULL ||
                      quest_history_contains(&p->quests.history, quest_id);
        player_release(p);
        if (already)
            LOG_DEBUG("[QUEST] character %u already has or has finished quest %u",
                      character_id, quest_id);
        else
            LOG_WARN("[QUEST] character %u does not qualify for quest %u",
                     character_id, quest_id);
        return 0;
    }

    if (!quest_log_reserve_one(&p->quests.log)) {
        player_release(p);
        LOG_ERROR("[QUEST] out of memory growing character %u's quest log", character_id);
        return 0;
    }

    struct PlayerQuestSlot* pq = &p->quests.log.slots[p->quests.log.count++];
    memset(pq, 0, sizeof(*pq));
    pq->quest_id  = quest_id;
    pq->is_active = 1;
    p->is_dirty   = 1;

    struct PlayerQuestSlot pq_copy = *pq;
    float from_x = p->pos_x;
    float from_y = p->pos_y;
    player_release(p);

    send_quest_accept_packet(client_fd, character_id, q, &pq_copy, from_x, from_y);
    LOG_DEBUG("[QUEST] Character %u accepted quest %u '%s'", character_id, quest_id, q->title);
    return 1;
}

int quest_player_turnin(uint32_t character_id, int client_fd, uint32_t quest_id) {
    const QuestDef* q = quest_get(quest_id);
    if (!q) return 0;

    ActivePlayer* p = player_acquire(character_id);
    if (!p) return 0;

    struct PlayerQuestSlot* pq = quest_log_find(&p->quests.log, quest_id);
    if (!pq || !pq->is_active || !pq->is_complete) {
        player_release(p);
        return 0;  // caller uses fail_page
    }

    /* The slot is released and the completion is recorded. The version before
     * this left the finished quest sitting in the log forever, because that
     * leftover record was the only record that it had been done -- which is
     * what made a fixed log a limit on quests taken in a lifetime.
     *
     * Freeing the slot is also what makes a repeatable work: the quest becomes
     * acceptable again the moment its record says so, and the NPC re-offers it
     * with no further help from anything. */
    quest_log_remove(&p->quests.log, quest_id);
    if (!quest_state_record_turnin(&p->quests, quest_id, q->repeat_mode, time(NULL))) {
        player_release(p);
        LOG_ERROR("[QUEST] out of memory recording quest %u for character %u",
                  quest_id, character_id);
        return 0;
    }
    p->is_dirty = 1;

    if (q->xp_reward > 0) p->experience += q->xp_reward;
    if (q->currency_reward > 0)
        world_currency_credit(p->currency, q->currency_id, q->currency_reward);

    QuestCompletePacket cpkt;
    memset(&cpkt, 0, sizeof(cpkt));
    cpkt.header.type         = PACKET_QUEST_COMPLETE;
    cpkt.header.player_id    = htonl(character_id);
    cpkt.header.payload_size = htons(sizeof(cpkt) - sizeof(PacketHeader));
    cpkt.quest_id        = htonl(quest_id);
    cpkt.xp_reward       = htonl(q->xp_reward);
    cpkt.currency_reward = htonl(q->currency_reward);
    cpkt.currency_id     = q->currency_id;

    for (int i = 0; i < q->item_reward_count && i < MAX_QUEST_OBJECTIVES; i++) {
        uint32_t reward_id = q->item_rewards[i].item_id;
        uint16_t want      = q->item_rewards[i].quantity ? q->item_rewards[i].quantity : 1;

        const ItemDefinition* def = item_get(reward_id);
        int slot = inventory_first_free(p->inventory);

        // retain unplaced reward quantities for warning output
        uint16_t left   = inventory_add(p->inventory, reward_id, want,
                                        def ? def->max_stack : 1,
                                        def ? def->bind_on_pickup : 0);
        uint16_t stored = (uint16_t)(want - left);

        if (stored > 0) {
            cpkt.items[cpkt.item_count].item_id        = htonl(reward_id);
            cpkt.items[cpkt.item_count].quantity       = (uint8_t)stored;
            cpkt.items[cpkt.item_count].inventory_slot = (uint8_t)(slot < 0 ? 0 : slot);
            cpkt.item_count++;
        }
        if (left > 0)
            LOG_WARN("[QUEST] character %u had no room for %u x item %u",
                     character_id, left, reward_id);
    }

    player_release(p);

    server_send(client_fd, &cpkt, sizeof(cpkt));

    // report current slots after stack merges
    if (cpkt.item_count > 0) {
        uint16_t changed[MAX_SLOT_UPDATES];
        int n = 0;
        for (int i = 0; i < cpkt.item_count && n < MAX_SLOT_UPDATES; i++)
            changed[n++] = cpkt.items[i].inventory_slot;
        player_send_slot_updates(client_fd, character_id, changed, n);
    }

    LOG_DEBUG("[QUEST] Character %u completed quest %u '%s'", character_id, quest_id, q->title);
    return 1;
}

int quest_player_abandon(uint32_t character_id, int client_fd, uint32_t quest_id) {
    ActivePlayer* p = player_acquire(character_id);
    if (!p) return 0;

    /* The whole of the rule: the quest has to be in this character's own log.
     * The packet said which quest, and nothing else it said is believed. */
    if (!quest_log_remove(&p->quests.log, quest_id)) {
        player_release(p);
        LOG_DEBUG("[QUEST] character %u asked to abandon quest %u, which is not "
                  "in their log", character_id, quest_id);
        return 0;
    }
    p->is_dirty = 1;
    player_release(p);

    /* Deliberately nothing written to the history or the counters: giving up is
     * not finishing, and both of those are read as evidence that it was. */

    QuestAbandonPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type         = PACKET_QUEST_ABANDONED;
    pkt.header.player_id    = htonl(character_id);
    pkt.header.payload_size = htons(sizeof(pkt) - sizeof(PacketHeader));
    pkt.quest_id            = htonl(quest_id);
    server_send(client_fd, &pkt, sizeof(pkt));

    LOG_DEBUG("[QUEST] character %u abandoned quest %u", character_id, quest_id);
    return 1;
}

/* --- Progress ------------------------------------------------------------ */

/** Bound one event's progress packets.
 *
 * Not a limit on the quest log, which has none: it is how many objectives a
 * single kill or conversation can plausibly advance at once, and a buffer that
 * has to live on the stack under the player lock. An event advancing more than
 * this is reported rather than silently dropped.
 */
#define MAX_ADVANCES_PER_EVENT 64

/** Carry one objective advance out of the locked section so it can be sent. */
typedef struct {
    uint32_t quest_id;
    uint8_t  obj_index;
    int32_t  current;
    int32_t  required;
} QuestAdvance;

/**
 * Advance every objective of a matching type and target, in one locked pass.
 *
 * One acquisition, not one per objective: the loop this replaced released and
 * re-acquired the player around each packet, so a second thread could change
 * the log underneath an index the loop was still walking.
 *
 * @param out       Receives the advances made.
 * @param max_out   Capacity of out.
 * @return          How many advances were recorded.
 */
static int advance_objectives(uint32_t character_id, QuestObjectiveType type,
                              uint32_t target_id, QuestAdvance* out, int max_out) {
    ActivePlayer* p = player_acquire(character_id);
    if (!p) return 0;

    int written = 0;

    for (int qi = 0; qi < p->quests.log.count; qi++) {
        struct PlayerQuestSlot* pq = &p->quests.log.slots[qi];
        if (!pq->is_active || pq->is_complete) continue;

        const QuestDef* q = quest_get(pq->quest_id);
        if (!q) continue;

        int changed = 0;
        for (int oi = 0; oi < q->obj_count && oi < MAX_QUEST_OBJECTIVES; oi++) {
            const QuestObjectiveDef* def = &q->objectives[oi];
            if (def->type != (uint8_t)type) continue;
            if (def->target_id != target_id) continue;
            if (pq->progress[oi] >= def->required_count) continue;

            pq->progress[oi]++;
            changed = 1;

            if (written < max_out) {
                out[written].quest_id  = pq->quest_id;
                out[written].obj_index = (uint8_t)oi;
                out[written].current   = pq->progress[oi];
                out[written].required  = def->required_count;
                written++;
            } else {
                LOG_WARN_RL(5, 60, "[QUEST] character %u advanced more than %d "
                            "objectives at once; the rest were not reported",
                            character_id, max_out);
            }
        }

        if (!changed) continue;

        int all_done = 1;
        for (int k = 0; k < q->obj_count && k < MAX_QUEST_OBJECTIVES; k++) {
            if (pq->progress[k] < q->objectives[k].required_count) { all_done = 0; break; }
        }
        if (all_done) pq->is_complete = 1;

        p->is_dirty = 1;
    }

    player_release(p);
    return written;
}

/**
 * Advance objectives of one type and send every resulting progress packet.
 */
static void quest_on_event(uint32_t character_id, int client_fd,
                           QuestObjectiveType type, uint32_t target_id) {
    QuestAdvance advances[MAX_ADVANCES_PER_EVENT];
    int count = advance_objectives(character_id, type, target_id,
                                   advances, MAX_ADVANCES_PER_EVENT);

    for (int i = 0; i < count; i++)
        send_progress(client_fd, character_id, advances[i].quest_id,
                      advances[i].obj_index, advances[i].current, advances[i].required);
}

void quest_on_npc_kill(uint32_t character_id, int client_fd, uint16_t npc_type_id) {
    quest_on_event(character_id, client_fd, QUEST_OBJECTIVE_KILL, npc_type_id);
}

void quest_on_item_collect(uint32_t character_id, int client_fd, uint32_t item_id) {
    quest_on_event(character_id, client_fd, QUEST_OBJECTIVE_COLLECT, item_id);
}

void quest_on_npc_talk(uint32_t character_id, int client_fd, uint16_t npc_type_id) {
    quest_on_event(character_id, client_fd, QUEST_OBJECTIVE_TALK, npc_type_id);
}

void quest_send_all(uint32_t character_id, int client_fd) {
    ActivePlayer* p = player_acquire(character_id);
    if (!p) return;

    /* Copied out so the packets go out with the player lock released; the log
     * is heap-allocated and could be grown by another accept in the meantime. */
    int count = p->quests.log.count;
    PlayerQuestEntry* local_quests = NULL;
    if (count > 0) {
        local_quests = malloc((size_t)count * sizeof(*local_quests));
        if (!local_quests) {
            player_release(p);
            LOG_ERROR("[QUEST] out of memory resending character %u's quests", character_id);
            return;
        }
        memcpy(local_quests, p->quests.log.slots, (size_t)count * sizeof(*local_quests));
    }
    float from_x = p->pos_x;
    float from_y = p->pos_y;
    player_release(p);

    for (int i = 0; i < count; i++) {
        if (!local_quests[i].is_active) continue;
        const QuestDef* q = quest_get(local_quests[i].quest_id);
        if (!q) continue;
        send_quest_accept_packet(client_fd, character_id, q, &local_quests[i],
                                 from_x, from_y);
    }

    free(local_quests);
}
