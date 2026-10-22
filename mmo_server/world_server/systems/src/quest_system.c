/**
 * @file
 * Decide who may take a quest, grant it, advance it, and hand it in.
 *
 * This is the behaviour layer: what a quest *is* lives in quest_registry.h, and
 * what a character has done with one lives in quest_state.h.
 */

#include "quest_system.h"
#include "outbox.h"
#include "str_fixed.h"
#include "log.h"
#include "player_data.h"
#include "player_level.h"
#include "item_registry.h"
#include "npc_world.h"
#include "utils.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/socket.h>
#include <arpa/inet.h>

extern NPCWorld g_npc_world;

static quest_change_hook g_change_hook;

void quest_set_change_hook(quest_change_hook hook) {
    g_change_hook = hook;
}

/** Tell whoever is listening that this character's quests changed. */
static void notify_changed(uint32_t character_id, int client_fd) {
    if (g_change_hook)
        g_change_hook(character_id, client_fd);
}

void quest_offers_changed(uint32_t character_id, int client_fd) {
	notify_changed(character_id, client_fd);
}

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

    /* The NPC to point at: the one named for the purpose, or else the
     * objective's own target when that target is an NPC. */
    uint32_t pointed_at = def->marker_npc;
    if (pointed_at == 0 &&
        (def->type == QUEST_OBJECTIVE_TALK || def->type == QUEST_OBJECTIVE_KILL))
        pointed_at = def->target_id;

    if (!located && pointed_at > 0 && pointed_at <= UINT16_MAX) {
        located = npc_world_nearest_of_type(&g_npc_world, (uint16_t)pointed_at,
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
    outbox_send(client_fd, &pkt, sizeof(pkt));
}

// Clamp a character's step to one the quest still has, as content can shrink.
static int slot_step(const QuestDef* q, const struct PlayerQuestSlot* pq) {
	return pq->step < q->step_count ? pq->step : q->step_count - 1;
}

/**
 * Send a quest and the step the character is on, then that step's progress.
 *
 * Sent again for the same quest each time it moves to its next step; the client
 * refreshes the entry in place.
 *
 * @param from_x, from_y  The character's position, used to place markers.
 */
static void send_quest_accept_packet(int client_fd, uint32_t character_id,
                                     const QuestDef* q, const PlayerQuestEntry* pq,
                                     float from_x, float from_y) {
	int index = slot_step(q, pq);
	const QuestStepDef* step = quest_step_get(q, index);
	QuestAcceptPacket pkt;

	if(!step)
		return;
	memset(&pkt, 0, sizeof(pkt));
	pkt.header.type         = PACKET_QUEST_ACCEPT;
	pkt.header.player_id    = htonl(character_id);
	pkt.header.payload_size = htons(sizeof(pkt) - sizeof(PacketHeader));
	pkt.quest_id   = htonl(q->quest_id);
	STR_COPY_FIELD(pkt.title, q->title);
	STR_COPY_FIELD(pkt.step_title, step->title);
	pkt.obj_count  = step->obj_count;
	pkt.step       = (uint8_t)(index + 1);
	pkt.step_count = q->step_count;

	for(int i = 0; i < step->obj_count && i < MAX_QUEST_OBJECTIVES; i++)
		fill_objective_info(&pkt.objectives[i], &step->objectives[i], from_x, from_y);

	outbox_send(client_fd, &pkt, sizeof(pkt));

	for(int i = 0; i < step->obj_count && i < MAX_QUEST_OBJECTIVES; i++)
		send_progress(client_fd, character_id, q->quest_id, (uint8_t)i,
		              pq->progress[i], step->objectives[i].required_count);
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
    notify_changed(character_id, client_fd);
    return 1;
}

// What one step paid, in the form the packets that report it carry.
typedef struct {
	uint32_t        xp;
	uint32_t        currency;
	uint8_t         currency_id;
	uint8_t         item_count;
	QuestRewardItem items[MAX_QUEST_OBJECTIVES];
	uint16_t        changed[MAX_SLOT_UPDATES];   // inventory slots the items touched
	int             changed_count;
} quest_payout;

/**
 * Pay one step's rewards to a locked character and record what was paid.
 *
 * XP goes through the same path a kill's does, so a step that crosses a level
 * threshold levels the player now, and the new total reaches the client with
 * the reward. It goes out as a kill reward of zero, which the client reads as
 * "update the bar only".
 */
static void pay_step_locked(ActivePlayer* p, int client_fd, uint32_t character_id,
                            const QuestDef* q, const QuestStepDef* step,
                            quest_payout* out) {
	memset(out, 0, sizeof(*out));
	out->xp          = step->xp_reward;
	out->currency    = step->currency_reward;
	out->currency_id = q->currency_id;

	if(step->xp_reward > 0) {
		player_award_xp_locked(p, step->xp_reward);
		player_send_kill_reward_locked(client_fd, p, 0);
	}
	if(step->currency_reward > 0)
		world_currency_credit(p->currency, q->currency_id, step->currency_reward);

	// slots come from inventory_add_tracked(), as a stacked reward opens none
	for(int i = 0; i < step->item_reward_count && i < MAX_QUEST_OBJECTIVES; i++) {
		uint32_t reward_id = step->item_rewards[i].item_id;
		uint16_t want      = step->item_rewards[i].quantity ? step->item_rewards[i].quantity : 1;
		const ItemDefinition* def = item_get(reward_id);
		uint16_t touched[MAX_SLOT_UPDATES];
		int      touched_count = 0;

		// retain unplaced reward quantities for warning output
		uint16_t left   = inventory_add_tracked(p->inventory, reward_id, want,
		                                        def ? def->max_stack : 1,
		                                        def ? def->bind_on_pickup : 0,
		                                        touched, MAX_SLOT_UPDATES,
		                                        &touched_count);
		uint16_t stored = (uint16_t)(want - left);

		if(stored > 0) {
			out->items[out->item_count].item_id        = htonl(reward_id);
			out->items[out->item_count].quantity       = (uint8_t)stored;
			out->items[out->item_count].inventory_slot =
				touched_count > 0 ? (uint8_t)touched[0] : 0;
			out->item_count++;
		}

		for(int t = 0; t < touched_count && out->changed_count < MAX_SLOT_UPDATES; t++) {
			// two rewards of one item share a stack, so a slot can come back twice
			int already = 0;

			for(int c = 0; c < out->changed_count; c++)
				if(out->changed[c] == touched[t]) { already = 1; break; }
			if(!already)
				out->changed[out->changed_count++] = touched[t];
		}

		if(left > 0)
			LOG_WARN("[QUEST] character %u had no room for %u x item %u",
			         character_id, left, reward_id);
	}
}

// Report the stacks the rewards actually landed in.
static void send_payout_slots(int client_fd, uint32_t character_id,
                              const quest_payout* pay) {
	if(pay->changed_count > 0)
		player_send_slot_updates(client_fd, character_id, pay->changed, pay->changed_count);
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

	// what the quest pays at the hand-in is the step it finished on
	const QuestStepDef* last = quest_step_get(q, slot_step(q, pq));

	if(!last) {
		player_release(p);
		return 0;
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
    /* A turn-in is the shape of loss a player writes in about: the quest is
     * gone from the log, the rewards are paid, and a crash before the next
     * sweep takes both while the NPC considers it done. */
    player_mark_critical(p);

	quest_payout pay;

	pay_step_locked(p, client_fd, character_id, q, last, &pay);
    player_release(p);

    QuestCompletePacket cpkt;
    memset(&cpkt, 0, sizeof(cpkt));
    cpkt.header.type         = PACKET_QUEST_COMPLETE;
    cpkt.header.player_id    = htonl(character_id);
    cpkt.header.payload_size = htons(sizeof(cpkt) - sizeof(PacketHeader));
    cpkt.quest_id        = htonl(quest_id);
    cpkt.xp_reward       = htonl(pay.xp);
    cpkt.currency_reward = htonl(pay.currency);
    cpkt.currency_id     = pay.currency_id;
	cpkt.item_count      = pay.item_count;
	memcpy(cpkt.items, pay.items, sizeof(cpkt.items));
    outbox_send(client_fd, &cpkt, sizeof(cpkt));
	send_payout_slots(client_fd, character_id, &pay);

    LOG_DEBUG("[QUEST] Character %u completed quest %u '%s'", character_id, quest_id, q->title);
    notify_changed(character_id, client_fd);
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
    outbox_send(client_fd, &pkt, sizeof(pkt));

    LOG_DEBUG("[QUEST] character %u abandoned quest %u", character_id, quest_id);
    notify_changed(character_id, client_fd);
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

// Carry one finished step out of the locked section so it can be reported.
typedef struct {
	uint32_t        quest_id;
	uint8_t         step;      // the step that finished, from 0
	quest_payout    payout;
	PlayerQuestEntry now;      // the quest as it stands on its next step
	float           from_x;
	float           from_y;
} QuestStepAdvance;

// Everything one event changed, gathered under the player lock.
typedef struct {
	QuestAdvance      advances[MAX_ADVANCES_PER_EVENT];
	int               advance_count;
	QuestStepAdvance* steps;           // grows; freed by the caller
	int               step_count;
	int               step_capacity;
} QuestEventResult;

// Make room to record one more finished step, or NULL when memory ran out.
static QuestStepAdvance* step_advance_reserve(QuestEventResult* result) {
	if(result->step_count == result->step_capacity) {
		int grown = result->step_capacity ? result->step_capacity * 2 : 4;
		QuestStepAdvance* bigger = realloc(result->steps, (size_t)grown * sizeof(*bigger));

		if(!bigger)
			return NULL;
		result->steps = bigger;
		result->step_capacity = grown;
	}

	return &result->steps[result->step_count++];
}

/**
 * Pay a finished step and move the quest on to the next one.
 *
 * The next step starts from nothing: progress belongs to a step, and the old
 * step's counters mean nothing to the new one.
 */
static void move_to_next_step_locked(ActivePlayer* p, int client_fd, uint32_t character_id,
                                     const QuestDef* q, struct PlayerQuestSlot* pq,
                                     const QuestStepDef* done, QuestEventResult* result) {
	QuestStepAdvance unreported;
	QuestStepAdvance* rec = step_advance_reserve(result);

	if(!rec) {
		// the step still moves on; the player is told at the next resend
		LOG_ERROR("[QUEST] out of memory reporting quest %u's step to character %u",
		          pq->quest_id, character_id);
		rec = &unreported;
	}
	rec->quest_id = pq->quest_id;
	rec->step     = pq->step;
	pay_step_locked(p, client_fd, character_id, q, done, &rec->payout);

	pq->step++;
	memset(pq->progress, 0, sizeof(pq->progress));
	rec->now    = *pq;
	rec->from_x = p->pos_x;
	rec->from_y = p->pos_y;

	// the rewards are paid, and a crash before the next sweep would take them back
	player_mark_critical(p);
}

/**
 * Advance every objective of a matching type and target, in one locked pass.
 *
 * One acquisition, not one per objective: the loop this replaced released and
 * re-acquired the player around each packet, so a second thread could change
 * the log underneath an index the loop was still walking.
 *
 * Only a quest's current step is looked at. Finishing a step that is not the
 * last pays it and moves on; finishing the last one leaves the quest waiting to
 * be handed in.
 *
 * @param result  Receives the advances and finished steps.
 */
static void advance_objectives(uint32_t character_id, int client_fd,
                               QuestObjectiveType type, uint32_t target_id,
                               QuestEventResult* result) {
	ActivePlayer* p = player_acquire(character_id);

	if(!p)
		return;

	for(int qi = 0; qi < p->quests.log.count; qi++) {
		struct PlayerQuestSlot* pq = &p->quests.log.slots[qi];

		if(!pq->is_active || pq->is_complete)
			continue;

		const QuestDef* q = quest_get(pq->quest_id);

		if(!q)
			continue;

		// content can shrink under a saved step, so land on the last one
		pq->step = (uint8_t)slot_step(q, pq);

		const QuestStepDef* step = quest_step_get(q, pq->step);

		if(!step)
			continue;

		int changed = 0;

		for(int oi = 0; oi < step->obj_count && oi < MAX_QUEST_OBJECTIVES; oi++) {
			const QuestObjectiveDef* def = &step->objectives[oi];

			if(def->type != (uint8_t)type)
				continue;
			if(def->target_id != target_id)
				continue;
			if(pq->progress[oi] >= def->required_count)
				continue;

			pq->progress[oi]++;
			changed = 1;

			if(result->advance_count < MAX_ADVANCES_PER_EVENT) {
				QuestAdvance* out = &result->advances[result->advance_count++];

				out->quest_id = pq->quest_id;
				out->obj_index = (uint8_t)oi;
				out->current = pq->progress[oi];
				out->required = def->required_count;
			} else {
				LOG_WARN_RL(5, 60, "[QUEST] character %u advanced more than %d "
				            "objectives at once; the rest were not reported",
				            character_id, MAX_ADVANCES_PER_EVENT);
			}
		}

		if(!changed)
			continue;

		int all_done = 1;

		for(int k = 0; k < step->obj_count && k < MAX_QUEST_OBJECTIVES; k++) {
			if(pq->progress[k] < step->objectives[k].required_count) {
				all_done = 0;
				break;
			}
		}

		// ordinary progress rides the sweep; losing "3 of 10 wolves" costs minutes
		if(!all_done) {
			p->is_dirty = 1;
			continue;
		}
		if(!quest_step_is_last(q, pq->step)) {
			move_to_next_step_locked(p, client_fd, character_id, q, pq, step, result);
			continue;
		}

		// the last step does not ride the sweep: the player is on the way to the NPC
		pq->is_complete = 1;
		player_mark_critical(p);
	}

	player_release(p);
}

// Tell the client a step finished and what it paid, then what comes next.
static void report_step(int client_fd, uint32_t character_id, const QuestStepAdvance* rec) {
	const QuestDef* q = quest_get(rec->quest_id);
	QuestStepDonePacket pkt;

	if(!q)
		return;
	memset(&pkt, 0, sizeof(pkt));
	pkt.header.type         = PACKET_QUEST_STEP_DONE;
	pkt.header.player_id    = htonl(character_id);
	pkt.header.payload_size = htons(sizeof(pkt) - sizeof(PacketHeader));
	pkt.quest_id        = htonl(rec->quest_id);
	pkt.xp_reward       = htonl(rec->payout.xp);
	pkt.currency_reward = htonl(rec->payout.currency);
	pkt.currency_id     = rec->payout.currency_id;
	pkt.item_count      = rec->payout.item_count;
	pkt.step            = (uint8_t)(rec->step + 1);
	pkt.step_count      = q->step_count;
	memcpy(pkt.items, rec->payout.items, sizeof(pkt.items));
	outbox_send(client_fd, &pkt, sizeof(pkt));
	send_payout_slots(client_fd, character_id, &rec->payout);

	send_quest_accept_packet(client_fd, character_id, q, &rec->now, rec->from_x, rec->from_y);
	LOG_DEBUG("[QUEST] Character %u finished step %u of quest %u '%s'", character_id,
	          (unsigned)pkt.step, rec->quest_id, q->title);
}

/**
 * Advance objectives of one type and send every resulting packet.
 */
static void quest_on_event(uint32_t character_id, int client_fd,
                           QuestObjectiveType type, uint32_t target_id) {
	QuestEventResult result;

	memset(&result, 0, sizeof(result));
	advance_objectives(character_id, client_fd, type, target_id, &result);

	for(int i = 0; i < result.advance_count; i++)
		send_progress(client_fd, character_id, result.advances[i].quest_id,
		              result.advances[i].obj_index, result.advances[i].current,
		              result.advances[i].required);
	for(int i = 0; i < result.step_count; i++)
		report_step(client_fd, character_id, &result.steps[i]);
	free(result.steps);

	/* A finished objective can finish a step or the quest, and either is what
	 * an NPC's mark changes for. */
	if(result.advance_count > 0 || result.step_count > 0)
		notify_changed(character_id, client_fd);
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

void quest_on_item_bought(uint32_t character_id, int client_fd, uint32_t item_id) {
    quest_on_event(character_id, client_fd, QUEST_OBJECTIVE_BUY, item_id);
}

void quest_on_item_sold(uint32_t character_id, int client_fd, uint32_t item_id) {
    quest_on_event(character_id, client_fd, QUEST_OBJECTIVE_SELL, item_id);
}

// Order quest ids largest first, so the newest quests lead the finished list.
static int compare_ids_descending(const void* a, const void* b) {
	uint32_t x = *(const uint32_t*)a;
	uint32_t y = *(const uint32_t*)b;

	return (x < y) - (x > y);
}

// Copy the ids of finished quests that are not in the log again, largest first.
static int copy_finished_ids(const PlayerQuestState* quests, uint32_t** out) {
	int count = 0;

	*out = NULL;
	if(!quests->history.ids || quests->history.count <= 0)
		return 0;

	uint32_t* ids = malloc((size_t)quests->history.count * sizeof(*ids));

	if(!ids)
		return -1;

	for(uint32_t i = 0; i <= quests->history.mask; i++) {
		uint32_t id = quests->history.ids[i];

		// a repeatable being done again is shown as in progress, not finished
		if(id != 0 && !quest_log_find((PlayerQuestLog*)&quests->log, id))
			ids[count++] = id;
	}
	qsort(ids, (size_t)count, sizeof(*ids), compare_ids_descending);
	*out = ids;

	return count;
}

// Tell the client which quests this character finished in earlier sessions.
static void send_finished(int client_fd, uint32_t character_id, const uint32_t* ids,
                          int count) {
	for(int i = 0; i < count; i++) {
		const QuestDef* q = quest_get(ids[i]);
		QuestHistoryPacket pkt;

		if(!q)
			continue;
		memset(&pkt, 0, sizeof(pkt));
		pkt.header.type = PACKET_QUEST_HISTORY;
		pkt.header.player_id = htonl(character_id);
		pkt.header.payload_size = htons(sizeof(pkt) - sizeof(PacketHeader));
		pkt.quest_id = htonl(q->quest_id);
		STR_COPY_FIELD(pkt.title, q->title);
		outbox_send(client_fd, &pkt, sizeof(pkt));
	}
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
    uint32_t* finished = NULL;
    int finished_count = copy_finished_ids(&p->quests, &finished);
    if (finished_count < 0) {
        LOG_ERROR("[QUEST] out of memory listing character %u's finished quests", character_id);
        finished_count = 0;
    }
    float from_x = p->pos_x;
    float from_y = p->pos_y;
    player_release(p);

    send_finished(client_fd, character_id, finished, finished_count);
    free(finished);

    for (int i = 0; i < count; i++) {
        if (!local_quests[i].is_active) continue;
        const QuestDef* q = quest_get(local_quests[i].quest_id);
        if (!q) continue;
        send_quest_accept_packet(client_fd, character_id, q, &local_quests[i],
                                 from_x, from_y);
    }

    free(local_quests);
    notify_changed(character_id, client_fd);
}
