/**
 * @file
 * Tell each character which NPCs have a quest for them.
 */

#include "quest_marks.h"
#include "dialogue_system.h"
#include "quest_system.h"
#include "outbox.h"
#include "str_fixed.h"
#include "player_data.h"
#include "npc_world.h"
#include "log.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

extern NPCWorld g_npc_world;

/** Bound the distinct NPC types gathered from the world in one pass. */
#define MAX_SOURCES 64

/** Whether the quest could be taken by this character this instant. */
static int quest_open_to(const QuestDef* quest, uint32_t character_id) {
    ActivePlayer* p = player_acquire(character_id);
    if (!p) return 0;

    int open = quest_is_available_for(quest, p);

    player_release(p);
    return open;
}

/** Whether the character holds the quest with every objective met. */
static int quest_waiting_to_hand_in(uint32_t quest_id, uint32_t character_id) {
    ActivePlayer* p = player_acquire(character_id);
    if (!p) return 0;

    const struct PlayerQuestSlot* slot = quest_log_find(&p->quests.log, quest_id);
    int ready = slot && slot->is_active && slot->is_complete;

    player_release(p);
    return ready;
}

/**
 * Read one dialogue option as a mark for this character.
 *
 * @param title  Receives the quest's title when the option yields a mark.
 */
static NPCQuestMark option_mark(const DialogueOptionDef* option, uint32_t character_id,
                                const char** title) {
    if (option->action != DIALOGUE_ACTION_QUEST_ACCEPT &&
        option->action != DIALOGUE_ACTION_QUEST_TURNIN)
        return NPC_QUEST_MARK_NONE;

    const QuestDef* quest = quest_get(option->action_value);
    if (!quest || !dialogue_option_available(option, character_id))
        return NPC_QUEST_MARK_NONE;

    *title = quest->title;
    if (option->action == DIALOGUE_ACTION_QUEST_TURNIN)
        return quest_waiting_to_hand_in(quest->quest_id, character_id)
                   ? NPC_QUEST_MARK_READY : NPC_QUEST_MARK_NONE;

    return quest_open_to(quest, character_id) ? NPC_QUEST_MARK_AVAILABLE
                                              : NPC_QUEST_MARK_NONE;
}

/** The strongest mark anything in one dialogue gives this character. */
static NPCQuestMark dialogue_mark(const DialogueDef* dialogue, uint32_t character_id,
                                  const char** title) {
    NPCQuestMark best = NPC_QUEST_MARK_NONE;

    for (int pg = 0; pg < dialogue->page_count; pg++) {
        const DialoguePageDef* page = &dialogue->pages[pg];

        for (int op = 0; op < page->option_count; op++) {
            const char* option_title = NULL;
            NPCQuestMark mark = option_mark(&page->options[op], character_id, &option_title);

            if (mark > best) {
                best = mark;
                *title = option_title;
            }
        }
    }

    return best;
}

/** Find the entry already written for a type, or NULL. */
static NPCQuestMarkEntry* find_entry(NPCQuestMarkEntry* out, int count, uint8_t type) {
    for (int i = 0; i < count; i++)
        if (out[i].npc_type_id == type)
            return &out[i];

    return NULL;
}

int quest_marks_compute(uint32_t character_id, const QuestMarkSource* sources,
                        int source_count, NPCQuestMarkEntry* out, int max_out) {
    int count = 0;

    for (int i = 0; i < source_count; i++) {
        const DialogueDef* dialogue = dialogue_get(sources[i].dialogue_id);
        const char* title = NULL;

        if (!dialogue)
            continue;

        NPCQuestMark mark = dialogue_mark(dialogue, character_id, &title);
        NPCQuestMarkEntry* entry = find_entry(out, count, sources[i].npc_type_id);

        if (mark == NPC_QUEST_MARK_NONE || (entry && entry->mark >= mark))
            continue;
        if (!entry) {
            if (count >= max_out)
                break;
            entry = &out[count++];
            memset(entry, 0, sizeof(*entry));
            entry->npc_type_id = sources[i].npc_type_id;
        }
        entry->mark = (uint8_t)mark;
        STR_COPY_FIELD(entry->title, title);
    }

    return count;
}

/** Collect each distinct (type, dialogue) pair standing in the world. */
static int gather_sources(QuestMarkSource* out, int max_out) {
    int count = 0;

    npc_world_read_begin(&g_npc_world);

    int capacity = npc_world_capacity(&g_npc_world);

    for (int slot = 0; slot < capacity && count < max_out; slot++) {
        NPCEntity* npc = npc_world_slot(&g_npc_world, slot);

        if (!npc || npc->id == 0 || npc->dialogue_id == 0 || npc->npc_type_id > 255)
            continue;

        int known = 0;

        for (int i = 0; i < count && !known; i++)
            known = out[i].npc_type_id == npc->npc_type_id &&
                    out[i].dialogue_id == npc->dialogue_id;
        if (!known) {
            out[count].npc_type_id = (uint8_t)npc->npc_type_id;
            out[count].dialogue_id = npc->dialogue_id;
            count++;
        }
    }

    npc_world_read_end(&g_npc_world);

    return count;
}

// Whether this option offers a quest the character may take right now.
static const QuestDef* option_offer(const DialogueOptionDef* option, uint32_t character_id) {
	if(option->action != DIALOGUE_ACTION_QUEST_ACCEPT)
		return NULL;

	const QuestDef* quest = quest_get(option->action_value);

	if(!quest || !dialogue_option_available(option, character_id))
		return NULL;

	return quest_open_to(quest, character_id) ? quest : NULL;
}

// Add a quest and its giver to the list unless it is already there.
static int list_add(available_quest** list, int* count, int* capacity,
                    uint32_t quest_id, uint8_t giver) {
	for(int i = 0; i < *count; i++)
		if((*list)[i].quest_id == quest_id && (*list)[i].giver_npc_type == giver)
			return 1;
	if(*count == *capacity) {
		int grown = *capacity ? *capacity * 2 : 8;
		available_quest* bigger = realloc(*list, (size_t)grown * sizeof(**list));

		if(!bigger)
			return 0;
		*list = bigger;
		*capacity = grown;
	}
	(*list)[*count].quest_id = quest_id;
	(*list)[*count].giver_npc_type = giver;
	(*count)++;

	return 1;
}

// Add every quest one page of a dialogue offers.
static int list_page(const DialoguePageDef* page, uint32_t character_id, uint8_t giver,
                     available_quest** list, int* count, int* capacity) {
	for(int op = 0; op < page->option_count; op++) {
		const QuestDef* quest = option_offer(&page->options[op], character_id);

		if(quest && !list_add(list, count, capacity, quest->quest_id, giver))
			return 0;
	}

	return 1;
}

int quest_available_compute(uint32_t character_id, const QuestMarkSource* sources,
                            int source_count, available_quest** out) {
	available_quest* list = NULL;
	int count = 0;
	int capacity = 0;

	*out = NULL;
	for(int i = 0; i < source_count; i++) {
		const DialogueDef* dialogue = dialogue_get(sources[i].dialogue_id);

		for(int pg = 0; dialogue && pg < dialogue->page_count; pg++) {
			if(list_page(&dialogue->pages[pg], character_id, sources[i].npc_type_id,
			             &list, &count, &capacity))
				continue;
			free(list);

			return -1;
		}
	}
	*out = list;

	return count;
}

// Describe one objective the way the log shows it, without a map position.
static void fill_available_objective(QuestObjectiveInfo* out, const QuestObjectiveDef* def) {
	memset(out, 0, sizeof(*out));
	STR_COPY_FIELD(out->description, def->description);
	out->required = htonl(def->required_count);
	out->objective_type = def->type;
	out->target_id = htonl(def->target_id);
}

// Add one step's item rewards to the packet, merging a repeated item into one row.
static void add_available_items(AvailableQuestPacket* pkt, const QuestStepDef* step) {
	for(int i = 0; i < step->item_reward_count && i < MAX_QUEST_OBJECTIVES; i++) {
		uint32_t item_id = step->item_rewards[i].item_id;
		uint8_t quantity = step->item_rewards[i].quantity ? step->item_rewards[i].quantity : 1;
		int row = 0;

		while(row < pkt->item_count && ntohl(pkt->items[row].item_id) != item_id)
			row++;
		if(row == pkt->item_count) {
			if(row >= MAX_QUEST_OBJECTIVES)
				continue;
			pkt->items[row].item_id = htonl(item_id);
			pkt->item_count++;
		}
		pkt->items[row].quantity = (uint8_t)(pkt->items[row].quantity + quantity);
	}
}

// Build the packet that tells a client one quest it may take.
static void build_available_packet(AvailableQuestPacket* pkt, uint32_t character_id,
                                   const QuestDef* quest, uint8_t giver,
                                   float from_x, float from_y) {
	const QuestStepDef* first = quest_step_get(quest, 0);
	uint32_t xp = 0;
	uint32_t coin = 0;
	float giver_x;
	float giver_y;

	memset(pkt, 0, sizeof(*pkt));
	pkt->header.type = PACKET_AVAILABLE_QUEST;
	pkt->header.player_id = htonl(character_id);
	pkt->header.payload_size = htons(sizeof(*pkt) - sizeof(PacketHeader));
	pkt->quest_id = htonl(quest->quest_id);
	STR_COPY_FIELD(pkt->title, quest->title);
	pkt->giver_npc_type = giver;
	if(npc_world_nearest_of_type(&g_npc_world, giver, from_x, from_y, &giver_x, &giver_y)) {
		pkt->has_giver_pos = 1;
		pkt->giver_x_centi = (int32_t)htonl((uint32_t)(int32_t)lroundf(giver_x * 100.0f));
		pkt->giver_y_centi = (int32_t)htonl((uint32_t)(int32_t)lroundf(giver_y * 100.0f));
	}
	pkt->step_count = quest->step_count;
	pkt->currency_id = quest->currency_id;
	if(!first)
		return;

	// what the player reads is the first step; the reward is the whole quest's
	pkt->obj_count = first->obj_count;
	for(int i = 0; i < first->obj_count && i < MAX_QUEST_OBJECTIVES; i++)
		fill_available_objective(&pkt->objectives[i], &first->objectives[i]);
	for(int s = 0; s < quest->step_count; s++) {
		const QuestStepDef* step = quest_step_get(quest, s);

		xp += step->xp_reward;
		coin += step->currency_reward;
		add_available_items(pkt, step);
	}
	pkt->xp_reward = htonl(xp);
	pkt->currency_reward = htonl(coin);
}

// Replace the client's list of quests it may take with the current one.
static void available_send(uint32_t character_id, int client_fd,
                           const QuestMarkSource* sources, int source_count) {
	available_quest* list = NULL;
	int count = quest_available_compute(character_id, sources, source_count, &list);

	if(count < 0) {
		LOG_ERROR("[QUEST] out of memory listing available quests for %u", character_id);
		return;
	}

	LOG_INFO("[QUEST] character %u may take %d quest(s) from %d NPC type(s)", character_id,
	         count, source_count);

	AvailableQuestsResetPacket reset;
	float from_x = 0.0f;
	float from_y = 0.0f;
	ActivePlayer* p = player_acquire(character_id);

	if(p) {
		from_x = p->pos_x;
		from_y = p->pos_y;
		player_release(p);
	}
	memset(&reset, 0, sizeof(reset));
	reset.header.type = PACKET_AVAILABLE_QUESTS_RESET;
	reset.header.player_id = htonl(character_id);
	reset.header.payload_size = htons(sizeof(reset) - sizeof(PacketHeader));
	reset.count = htonl((uint32_t)count);
	outbox_send(client_fd, &reset, sizeof(reset));

	for(int i = 0; i < count; i++) {
		const QuestDef* quest = quest_get(list[i].quest_id);
		AvailableQuestPacket pkt;

		if(!quest)
			continue;
		build_available_packet(&pkt, character_id, quest, list[i].giver_npc_type, from_x,
		                       from_y);
		outbox_send(client_fd, &pkt, sizeof(pkt));
	}
	free(list);
}

void quest_marks_send(uint32_t character_id, int client_fd) {
	QuestMarkSource sources[MAX_SOURCES];
	NPCQuestMarksPacket pkt;
	int found = gather_sources(sources, MAX_SOURCES);

	memset(&pkt, 0, sizeof(pkt));
	pkt.header.type         = PACKET_NPC_QUEST_MARKS;
	pkt.header.player_id    = htonl(character_id);
	pkt.header.payload_size = htons(sizeof(pkt) - sizeof(PacketHeader));
	pkt.count = (uint8_t)quest_marks_compute(character_id, sources, found, pkt.entries,
	                                         MAX_NPC_QUEST_MARKS);
	outbox_send(client_fd, &pkt, sizeof(pkt));
	available_send(character_id, client_fd, sources, found);
}
