/**
 * @file
 * The list of quests the server says a character may take.
 *
 * The packet is the server's word and the client keeps what it is told, so what
 * matters here is that it is read faithfully and that a hostile or garbled one
 * cannot overrun a field.
 */

#include "ui/quest_log.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int passed = 0;

static void check(int condition, const char* what) {
	if(!condition) {
		printf("  FAIL: %s\n", what);
		exit(1);
	}
	passed++;
}

// Start from an empty log; quest_log_init lives with the drawing code.
static void fresh_log(QuestLogState* ql) {
	memset(ql, 0, sizeof(*ql));
	quest_log_available_reset(ql);
}

// Build the packet the server would send for one quest.
static AvailableQuestPacket make_packet(uint32_t id, const char* title, uint8_t giver) {
	AvailableQuestPacket pkt;

	memset(&pkt, 0, sizeof(pkt));
	pkt.header.type = PACKET_AVAILABLE_QUEST;
	pkt.quest_id = htonl(id);
	snprintf(pkt.title, sizeof(pkt.title), "%s", title);
	pkt.giver_npc_type = giver;
	pkt.step_count = 7;
	pkt.obj_count = 1;
	snprintf(pkt.objectives[0].description, sizeof(pkt.objectives[0].description),
	         "Buy 2 Health Tonics");
	pkt.objectives[0].required = (int32_t)htonl(2);
	pkt.objectives[0].objective_type = QUEST_OBJECTIVE_BUY;
	pkt.objectives[0].target_id = htonl(50000);
	pkt.xp_reward = htonl(40);
	pkt.currency_reward = htonl(25);
	pkt.currency_id = 1;
	pkt.item_count = 1;
	pkt.items[0].item_id = htonl(1001);
	pkt.items[0].quantity = 3;

	return pkt;
}

static void test_a_quest_is_read_faithfully(void) {
	printf("a quest is read the way it was sent\n");

	QuestLogState ql;
	AvailableQuestPacket pkt = make_packet(302, "Honest Goods", 50);

	fresh_log(&ql);
	pkt.has_giver_pos = 1;
	pkt.giver_x_centi = (int32_t)htonl((uint32_t)-123456);
	pkt.giver_y_centi = (int32_t)htonl(7850);
	quest_log_available_add(&ql, &pkt);

	check(ql.available_count == 1, "one quest listed");

	const available_quest_entry* e = &ql.available[0];

	check(e->id == 302, "id");
	check(strcmp(e->title, "Honest Goods") == 0, "title");
	check(e->giver_npc_type == 50, "giver");
	check(e->has_giver_pos == 1, "giver position flag");
	check(e->giver_x == -1234.56f && e->giver_y == 78.5f, "giver position");
	check(e->step_count == 7, "step count");
	check(e->obj_count == 1, "objective count");
	check(strcmp(e->objectives[0].description, "Buy 2 Health Tonics") == 0, "objective text");
	check(e->objectives[0].required == 2, "objective required count");
	check(e->objectives[0].current == 0, "nothing is done on a quest not yet taken");
	check(e->objectives[0].objective_type == QUEST_OBJECTIVE_BUY, "objective type");
	check(e->xp_reward == 40 && e->currency_reward == 25 && e->currency_id == 1, "rewards");
	check(e->item_count == 1 && e->items[0].item_id == 1001 && e->items[0].quantity == 3,
	      "item reward");
}

static void test_reset_replaces_the_list(void) {
	printf("a reset empties the list and clears the selection\n");

	QuestLogState ql;
	AvailableQuestPacket a = make_packet(1, "One", 10);
	AvailableQuestPacket b = make_packet(2, "Two", 10);

	fresh_log(&ql);
	quest_log_available_add(&ql, &a);
	quest_log_available_add(&ql, &b);
	ql.available_selected = 1;
	check(ql.available_count == 2, "two listed");

	quest_log_available_reset(&ql);
	check(ql.available_count == 0, "list is empty after a reset");
	check(ql.available_selected == -1, "selection is cleared by a reset");
}

static void test_a_hostile_packet_cannot_overrun(void) {
	printf("counts and text from the network are not trusted\n");

	QuestLogState ql;
	AvailableQuestPacket pkt = make_packet(9, "x", 1);

	fresh_log(&ql);
	memset(pkt.title, 'A', sizeof(pkt.title));
	memset(pkt.objectives[0].description, 'B', sizeof(pkt.objectives[0].description));
	pkt.obj_count = 255;
	pkt.item_count = 255;
	quest_log_available_add(&ql, &pkt);

	const available_quest_entry* e = &ql.available[0];

	check(strlen(e->title) == sizeof(e->title) - 1, "an unterminated title is ended");
	check(e->obj_count == MAX_QUEST_OBJ, "objective count is clamped");
	check(e->item_count == MAX_QUEST_OBJ, "item count is clamped");
	check(strlen(e->objectives[0].description) == sizeof(e->objectives[0].description) - 1,
	      "an unterminated objective is ended");
}

static void test_the_display_limit_holds(void) {
	printf("quests past the display limit are dropped, not written out of bounds\n");

	QuestLogState ql;
	AvailableQuestPacket pkt = make_packet(5, "Many", 3);

	fresh_log(&ql);
	for(int i = 0; i < MAX_AVAILABLE_QUESTS + 10; i++)
		quest_log_available_add(&ql, &pkt);
	check(ql.available_count == MAX_AVAILABLE_QUESTS, "list stops at the limit");
}

static void test_quests_are_found_by_giver(void) {
	printf("the quests one NPC type gives can be picked out\n");

	QuestLogState ql;
	AvailableQuestPacket a = make_packet(1, "One", 10);
	AvailableQuestPacket b = make_packet(2, "Two", 20);
	AvailableQuestPacket c = make_packet(3, "Three", 10);
	const available_quest_entry* out[4];

	fresh_log(&ql);
	quest_log_available_add(&ql, &a);
	quest_log_available_add(&ql, &b);
	quest_log_available_add(&ql, &c);

	check(quest_log_available_from(&ql, 10, out, 4) == 2, "two from the first giver");
	check(out[0]->id == 1 && out[1]->id == 3, "in the order they were sent");
	check(quest_log_available_from(&ql, 20, out, 4) == 1, "one from the second giver");
	check(quest_log_available_from(&ql, 99, out, 4) == 0, "none from a stranger");
	check(quest_log_available_from(&ql, 10, out, 1) == 1, "never more than asked for");
}

int main(void) {
	printf("=== available quests on the client ===\n");
	test_a_quest_is_read_faithfully();
	test_reset_replaces_the_list();
	test_a_hostile_packet_cannot_overrun();
	test_the_display_limit_holds();
	test_quests_are_found_by_giver();
	printf("\n%d checks passed\n", passed);

	return 0;
}
