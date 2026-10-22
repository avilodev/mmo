/**
 * @file
 * Hold the quests the server says the character may take.
 */

#include "ui/quest_log.h"

#include <winsock2.h>   // ntohl for the counters the packet carries

#include <string.h>

// Copy a wire string into a field and end it, whatever the sender left there.
static void copy_text(char* out, size_t size, const char* in) {
	memcpy(out, in, size - 1);
	out[size - 1] = '\0';
}

void quest_log_available_reset(QuestLogState* ql) {
	ql->available_count = 0;
	ql->available_selected = -1;
}

// Read one objective's text and count; where it happens is not sent.
static void adopt_available_objective(QuestObjEntry* out, const QuestObjectiveInfo* in) {
	memset(out, 0, sizeof(*out));
	copy_text(out->description, sizeof(out->description), in->description);
	out->required = (int32_t)ntohl((uint32_t)in->required);
	out->objective_type = in->objective_type;
	out->target_id = ntohl(in->target_id);
}

void quest_log_available_add(QuestLogState* ql, const AvailableQuestPacket* pkt) {
	if(ql->available_count >= MAX_AVAILABLE_QUESTS)
		return;

	available_quest_entry* e = &ql->available[ql->available_count++];

	memset(e, 0, sizeof(*e));
	e->id = ntohl(pkt->quest_id);
	copy_text(e->title, sizeof(e->title), pkt->title);
	e->giver_npc_type = pkt->giver_npc_type;
	e->has_giver_pos = pkt->has_giver_pos != 0;
	e->giver_x = (float)(int32_t)ntohl((uint32_t)pkt->giver_x_centi) / 100.0f;
	e->giver_y = (float)(int32_t)ntohl((uint32_t)pkt->giver_y_centi) / 100.0f;
	e->step_count = pkt->step_count;
	e->obj_count = pkt->obj_count > MAX_QUEST_OBJ ? MAX_QUEST_OBJ : pkt->obj_count;
	for(int i = 0; i < e->obj_count; i++)
		adopt_available_objective(&e->objectives[i], &pkt->objectives[i]);
	e->xp_reward = ntohl(pkt->xp_reward);
	e->currency_reward = ntohl(pkt->currency_reward);
	e->currency_id = pkt->currency_id;
	e->item_count = pkt->item_count > MAX_QUEST_OBJ ? MAX_QUEST_OBJ : pkt->item_count;
	for(int i = 0; i < e->item_count; i++) {
		e->items[i].item_id = ntohl(pkt->items[i].item_id);
		e->items[i].quantity = pkt->items[i].quantity;
	}
}

int quest_log_available_from(const QuestLogState* ql, uint8_t giver_npc_type,
                             const available_quest_entry** out, int max_out) {
	int found = 0;

	for(int i = 0; i < ql->available_count && found < max_out; i++)
		if(ql->available[i].giver_npc_type == giver_npc_type)
			out[found++] = &ql->available[i];

	return found;
}
