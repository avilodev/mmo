#define _GNU_SOURCE

/**
 * @file
 * Walk "Welcome to Ennara", one quest of seven steps, from first conversation
 * to last hand-in.
 *
 * opening_sequence_test proves the data connects: every identifier resolves,
 * every quest is offered and taken back. This proves it plays. A character with
 * a real quest log is taken through the quest by the same calls the server
 * makes -- accept, a talk/buy/sell event per step, turn in -- and at every step
 * the test asks what the client would be told: which step it is on, what that
 * step paid, which NPC has the quest to offer or collect, and where the
 * objective's marker points.
 *
 * The player and socket are stubbed; everything else is the real quest system,
 * dialogue system, shop data and NPC pool.
 */

#include "quest_system.h"
#include "quest_marks.h"
#include "dialogue_system.h"
#include "npc_world.h"
#include "npc_spawns.h"
#include "race_registry.h"
#include "item_instance.h"
#include "item_registry.h"
#include "shop.h"

#include <arpa/inet.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* --- Stubs --------------------------------------------------------------- */

NPCWorld g_npc_world;

static ActivePlayer g_player;

ActivePlayer* player_acquire(uint32_t character_id) {
    g_player.character_id = character_id;
    return &g_player;
}
void player_release(ActivePlayer* player) { (void)player; }
void player_send_slot_updates(int fd, uint32_t c, const uint16_t* s, int n) {
    (void)fd; (void)c; (void)s; (void)n;
}
// The bag always has room, and every reward lands in its first slot.
uint16_t inventory_add_tracked(ItemInstance* slots, uint32_t item_id,
                               uint16_t quantity, uint16_t max_stack,
                               uint8_t bind_on_pickup,
                               uint16_t* changed, int max_changed,
                               int* changed_count) {
	(void)slots; (void)item_id; (void)quantity; (void)max_stack; (void)bind_on_pickup;
	if(changed && max_changed > 0)
		changed[0] = 0;
	if(changed_count)
		*changed_count = max_changed > 0 ? 1 : 0;

	return 0;
}
uint16_t inventory_add(ItemInstance* slots, uint32_t item_id, uint16_t quantity,
                       uint16_t max_stack, uint8_t bind_on_pickup) {
    return inventory_add_tracked(slots, item_id, quantity, max_stack,
                                 bind_on_pickup, NULL, 0, NULL);
}
int inventory_first_free(const ItemInstance* slots) { (void)slots; return -1; }

/** The last packets of the kinds the test reads back. */
static QuestAcceptPacket   g_last_accept;
static NPCQuestMarksPacket g_last_marks;

// Step and hand-in rewards as the client was told them, and how often.
static QuestStepDonePacket g_last_step_done;
static int                 g_step_done_count;
static QuestCompletePacket g_last_complete;
static int                 g_accept_count;

// What the stubbed XP path was asked to pay; see quest_reward_stub.c.
extern uint64_t g_stub_xp_total;

// The available-quest list as the last send left it.
#define MAX_TEST_AVAILABLE 32
static AvailableQuestPacket g_available[MAX_TEST_AVAILABLE];
static int                  g_finished_count;
static uint32_t             g_finished[8];
static int                  g_available_count;
static uint32_t             g_available_promised;

ssize_t outbox_send(int fd, void* buf, size_t len) {
    (void)fd;
    const PacketHeader* header = (const PacketHeader*)buf;

	if(header->type == PACKET_AVAILABLE_QUESTS_RESET) {
		g_available_count = 0;
		g_available_promised = ntohl(((const AvailableQuestsResetPacket*)buf)->count);
	}
	if(header->type == PACKET_QUEST_HISTORY && len >= sizeof(QuestHistoryPacket) &&
	   g_finished_count < 8)
		g_finished[g_finished_count++] = ntohl(((const QuestHistoryPacket*)buf)->quest_id);
	if(header->type == PACKET_AVAILABLE_QUEST && len >= sizeof(AvailableQuestPacket) &&
	   g_available_count < MAX_TEST_AVAILABLE)
		memcpy(&g_available[g_available_count++], buf, sizeof(AvailableQuestPacket));

    if (header->type == PACKET_QUEST_ACCEPT && len >= sizeof(g_last_accept)) {
        memcpy(&g_last_accept, buf, sizeof(g_last_accept));
        g_accept_count++;
    }
	if(header->type == PACKET_QUEST_STEP_DONE && len >= sizeof(g_last_step_done)) {
		memcpy(&g_last_step_done, buf, sizeof(g_last_step_done));
		g_step_done_count++;
	}
	if(header->type == PACKET_QUEST_COMPLETE && len >= sizeof(g_last_complete))
		memcpy(&g_last_complete, buf, sizeof(g_last_complete));
    if (header->type == PACKET_NPC_QUEST_MARKS && len >= sizeof(g_last_marks))
        memcpy(&g_last_marks, buf, sizeof(g_last_marks));
    return (ssize_t)len;
}

/* --- Helpers ------------------------------------------------------------- */

static int passed = 0;

static void check(int condition, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

static void check(int condition, const char* fmt, ...) {
    if (!condition) {
        va_list args;
        printf("  FAIL: ");
        va_start(args, fmt);
        vprintf(fmt, args);
        va_end(args);
        printf("\n");
        exit(1);
    }
    passed++;
}

#define CHARACTER 77
#define FD        5

/** The NPC types and items the line names; the data says the same thing. */
#define WARDEN   20
#define MIRELA   50
#define DORAN    51
#define TONIC    50000
#define KNUCKLES 10000
#define CAP      1001

/** Refresh and read back the marks the character would see. */
static const NPCQuestMarksPacket* marks(void) {
    memset(&g_last_marks, 0, sizeof(g_last_marks));
    quest_marks_send(CHARACTER, FD);
    return &g_last_marks;
}

/** What the marks say about one NPC type. */
static const NPCQuestMarkEntry* mark_for(uint8_t npc_type) {
    const NPCQuestMarksPacket* m = marks();

    for (int i = 0; i < m->count; i++)
        if (m->entries[i].npc_type_id == npc_type)
            return &m->entries[i];

    return NULL;
}

static void expect_mark(uint8_t npc_type, NPCQuestMark want, const char* title) {
    const NPCQuestMarkEntry* e = mark_for(npc_type);

    if (want == NPC_QUEST_MARK_NONE) {
        check(e == NULL, "NPC type %u should have nothing for the character but says '%s'",
              npc_type, e ? e->title : "");
        return;
    }
    check(e != NULL, "NPC type %u should have a quest ('%s') but has no mark", npc_type, title);
    check(e->mark == want, "NPC type %u mark is %u, wanted %u", npc_type, e->mark, want);
    check(strcmp(e->title, title) == 0, "NPC type %u offers '%s', wanted '%s'",
          npc_type, e->title, title);
}

/** Every NPC type that speaks has nothing for the character. */
static void expect_no_marks(void) {
    const NPCQuestMarksPacket* m = marks();

    check(m->count == 0, "%d NPCs still carry a mark", m->count);
}

/** No page may offer more choices at once than a packet can carry. */
static void expect_pages_fit(void) {
    uint8_t ids[64];

    for (uint32_t d = 1; d < 256; d++) {
        const DialogueDef* dialogue = dialogue_get(d);

        for (int p = 0; dialogue && p < dialogue->page_count; p++) {
            int shown = dialogue_page_visible_options(&dialogue->pages[p], CHARACTER,
                                                      ids, (int)sizeof(ids));

            check(shown <= MAX_DIALOGUE_OPTIONS,
                  "dialogue %u page %d shows %d options at once; a packet carries %d",
                  d, dialogue->pages[p].page_num, shown, MAX_DIALOGUE_OPTIONS);
        }
    }
}

/** Whether an option with this action and quest is visible to the character. */
static int option_visible(uint32_t npc_type, uint8_t action, uint32_t quest_id) {
    uint32_t dialogue_id = 0;

    npc_world_read_begin(&g_npc_world);
    for (int slot = 0; slot < npc_world_capacity(&g_npc_world) && !dialogue_id; slot++) {
        NPCEntity* npc = npc_world_slot(&g_npc_world, slot);

        if (npc && npc->id != 0 && npc->npc_type_id == npc_type)
            dialogue_id = npc->dialogue_id;
    }
    npc_world_read_end(&g_npc_world);

    const DialogueDef* d = dialogue_get(dialogue_id);

    for (int p = 0; d && p < d->page_count; p++)
        for (int o = 0; o < d->pages[p].option_count; o++) {
            const DialogueOptionDef* option = &d->pages[p].options[o];

            if (option->action == action && option->action_value == quest_id &&
                dialogue_option_available(option, CHARACTER))
                return 1;
        }

    return 0;
}

static int progress_of(uint32_t quest_id, int objective) {
    struct PlayerQuestSlot* slot = quest_log_find(&g_player.quests.log, quest_id);

    return slot ? slot->progress[objective] : -1;
}

static int holds(uint32_t quest_id) {
    return quest_log_find(&g_player.quests.log, quest_id) != NULL;
}

// The step the character is on, counted from 1, or 0 when the quest is not held.
static int step_of(uint32_t quest_id) {
	struct PlayerQuestSlot* slot = quest_log_find(&g_player.quests.log, quest_id);

	return slot ? slot->step + 1 : 0;
}

// Whether the first page of an NPC type's conversation shows an option with this text.
static int says(uint32_t npc_type, const char* text) {
	uint32_t dialogue_id = 0;

	npc_world_read_begin(&g_npc_world);
	for(int slot = 0; slot < npc_world_capacity(&g_npc_world) && !dialogue_id; slot++) {
		NPCEntity* npc = npc_world_slot(&g_npc_world, slot);

		if(npc && npc->id != 0 && npc->npc_type_id == npc_type)
			dialogue_id = npc->dialogue_id;
	}
	npc_world_read_end(&g_npc_world);

	const DialogueDef* d = dialogue_get(dialogue_id);

	if(!d || d->page_count == 0)
		return 0;
	for(int o = 0; o < d->pages[0].option_count; o++) {
		const DialogueOptionDef* option = &d->pages[0].options[o];

		if(strcmp(option->text, text) == 0 && dialogue_option_available(option, CHARACTER))
			return 1;
	}

	return 0;
}

/* --- The walk ------------------------------------------------------------ */

#define QUEST     301
#define STEPS     7
#define STEP_ONE  1
#define LAST_STEP STEPS

// What each step is called, what it asks, and what finishing it pays.
static const struct {
	const char* title;
	uint8_t     objective_type;
	uint32_t    xp;
	uint32_t    coin;
} STEP_FACTS[STEPS] = {
	{"A Coin to Start",            QUEST_OBJECTIVE_TALK, 40,  40},
	{"Honest Goods",               QUEST_OBJECTIVE_BUY,  40,  25},
	{"A Fair Price",               QUEST_OBJECTIVE_SELL, 40,  25},
	{"The Armsmonger",             QUEST_OBJECTIVE_TALK, 50,  40},
	{"Something Sharp",            QUEST_OBJECTIVE_BUY,  60,  40},
	{"Second Thoughts",            QUEST_OBJECTIVE_SELL, 60,  40},
	{"The Measure of the Market",  QUEST_OBJECTIVE_TALK, 150, 100},
};

// Take the quest from the Warden and check what the client is told.
static void take(void) {
	expect_mark(WARDEN, NPC_QUEST_MARK_AVAILABLE, "Welcome to Ennara");
	check(option_visible(WARDEN, DIALOGUE_ACTION_QUEST_ACCEPT, QUEST),
	      "the Warden does not offer the quest in conversation");
	check(quest_player_accept(CHARACTER, FD, QUEST) == 1, "the quest could not be accepted");
	check(holds(QUEST), "the quest is not in the log after accepting");
	expect_pages_fit();
}

// Check what the client was last told about the step the character is on.
static void expect_on_step(int step) {
	check(step_of(QUEST) == step, "the character is on step %d, wanted %d", step_of(QUEST), step);
	check(g_last_accept.step == step, "the client was told step %u, wanted %d",
	      g_last_accept.step, step);
	check(g_last_accept.step_count == STEPS, "the client was told %u steps, wanted %d",
	      g_last_accept.step_count, STEPS);
	check(strcmp(g_last_accept.title, "Welcome to Ennara") == 0, "quest titled '%s'",
	      g_last_accept.title);
	check(strcmp(g_last_accept.step_title, STEP_FACTS[step - 1].title) == 0,
	      "step %d is called '%s', wanted '%s'", step, g_last_accept.step_title,
	      STEP_FACTS[step - 1].title);
	check(g_last_accept.obj_count == 1, "step %d sent %u objectives", step, g_last_accept.obj_count);
	check(g_last_accept.objectives[0].objective_type == STEP_FACTS[step - 1].objective_type,
	      "step %d sent objective type %u, wanted %u", step,
	      g_last_accept.objectives[0].objective_type, STEP_FACTS[step - 1].objective_type);
	check(g_last_accept.objectives[0].has_marker == 1,
	      "step %d sent no map marker for its objective", step);
	check(progress_of(QUEST, 0) == 0, "step %d did not start from nothing", step);
}

// Check that finishing a step paid what it should and said so.
static void expect_step_paid(int step, int steps_done_so_far) {
	uint64_t xp_owed = 0;

	for(int i = 0; i < step; i++)
		xp_owed += STEP_FACTS[i].xp;
	check(g_step_done_count == steps_done_so_far, "%d step reports, wanted %d",
	      g_step_done_count, steps_done_so_far);
	check(g_last_step_done.step == step, "the report named step %u, wanted %d",
	      g_last_step_done.step, step);
	check(g_last_step_done.step_count == STEPS, "the report said %u steps",
	      g_last_step_done.step_count);
	check(ntohl(g_last_step_done.xp_reward) == STEP_FACTS[step - 1].xp,
	      "step %d paid %u XP, wanted %u", step, ntohl(g_last_step_done.xp_reward),
	      STEP_FACTS[step - 1].xp);
	check(ntohl(g_last_step_done.currency_reward) == STEP_FACTS[step - 1].coin,
	      "step %d paid %u coin, wanted %u", step, ntohl(g_last_step_done.currency_reward),
	      STEP_FACTS[step - 1].coin);
	check(g_stub_xp_total == xp_owed, "%llu XP paid in all, wanted %llu",
	      (unsigned long long)g_stub_xp_total, (unsigned long long)xp_owed);
}

// Hand the finished quest in at the Warden, and check what it leaves behind.
static void hand_in(void) {
	expect_mark(WARDEN, NPC_QUEST_MARK_READY, "Welcome to Ennara");
	check(option_visible(WARDEN, DIALOGUE_ACTION_QUEST_TURNIN, QUEST),
	      "the Warden does not take the quest back in conversation");
	check(quest_player_turnin(CHARACTER, FD, QUEST) == 1, "the quest could not be handed in");
	check(!holds(QUEST), "the quest is still in the log after handing it in");
	expect_pages_fit();
}

// A fresh character, with every counter the stubs keep back at zero.
static void new_character(void) {
	quest_state_release(&g_player.quests);
	memset(&g_player, 0, sizeof(g_player));
	g_player.level = 1;
	g_player.race_id = 1;
	g_stub_xp_total = 0;
	g_step_done_count = 0;
	g_accept_count = 0;
}

// Do what each step asks, up to and including the given step.
static void do_steps_through(int last) {
	for(int step = 1; step <= last; step++) {
		switch(step) {
		case 1: quest_on_npc_talk(CHARACTER, FD, MIRELA); break;
		case 2:
			quest_on_item_bought(CHARACTER, FD, TONIC);
			quest_on_item_bought(CHARACTER, FD, TONIC);
			break;
		case 3: quest_on_item_sold(CHARACTER, FD, TONIC); break;
		case 4: quest_on_npc_talk(CHARACTER, FD, DORAN); break;
		case 5: quest_on_item_bought(CHARACTER, FD, KNUCKLES); break;
		case 6: quest_on_item_sold(CHARACTER, FD, KNUCKLES); break;
		case 7: quest_on_npc_talk(CHARACTER, FD, WARDEN); break;
		}
	}
}

static void test_the_quest_walks_seven_steps(void) {
	printf("one quest takes a character through seven steps, and the client is told at each\n");

	new_character();

	// Nobody has anything for a stranger except the Warden.
	expect_mark(MIRELA, NPC_QUEST_MARK_NONE, "");
	expect_mark(DORAN, NPC_QUEST_MARK_NONE, "");

	// 1. Talk: speak with Mirela.
	take();
	expect_on_step(1);
	expect_no_marks();
	quest_on_npc_talk(CHARACTER, FD, DORAN);
	check(step_of(QUEST) == 1 && progress_of(QUEST, 0) == 0,
	      "talking to the wrong NPC advanced the quest");
	check(g_step_done_count == 0, "a step was reported before it finished");
	quest_on_npc_talk(CHARACTER, FD, MIRELA);
	expect_step_paid(1, 1);

	// 2. Buy two tonics.
	expect_on_step(2);
	check(says(MIRELA, "Do you have any work?") && !says(MIRELA, "What else needs doing?"),
	      "Mirela does not speak to step 2 alone");
	quest_on_item_bought(CHARACTER, FD, CAP);
	check(step_of(QUEST) == 2 && progress_of(QUEST, 0) == 0, "buying the wrong item advanced the quest");
	quest_on_item_sold(CHARACTER, FD, TONIC);
	check(step_of(QUEST) == 2 && progress_of(QUEST, 0) == 0, "selling counted as buying");
	quest_on_item_bought(CHARACTER, FD, TONIC);
	check(step_of(QUEST) == 2 && progress_of(QUEST, 0) == 1 && g_step_done_count == 1,
	      "one tonic of two should not finish the step");
	quest_on_item_bought(CHARACTER, FD, TONIC);
	expect_step_paid(2, 2);

	// 3. Sell one back.
	expect_on_step(3);
	check(says(MIRELA, "What else needs doing?") && !says(MIRELA, "Do you have any work?"),
	      "Mirela does not speak to step 3 alone");
	quest_on_item_bought(CHARACTER, FD, TONIC);
	check(step_of(QUEST) == 3 && progress_of(QUEST, 0) == 0, "buying counted as selling");
	quest_on_item_sold(CHARACTER, FD, TONIC);
	expect_step_paid(3, 3);

	// 4. Talk: Doran.
	expect_on_step(4);
	check(says(MIRELA, "Is there anything more?"), "Mirela does not point to Doran at step 4");
	quest_on_npc_talk(CHARACTER, FD, MIRELA);
	check(step_of(QUEST) == 4 && progress_of(QUEST, 0) == 0,
	      "talking to Mirela again advanced the Doran step");
	quest_on_npc_talk(CHARACTER, FD, DORAN);
	expect_step_paid(4, 4);

	// 5. Buy the knuckles.
	expect_on_step(5);
	check(says(DORAN, "Any work for me?"), "Doran does not speak to step 5");
	check(!says(MIRELA, "Is there anything more?"), "Mirela still speaks to step 4");
	quest_on_item_bought(CHARACTER, FD, KNUCKLES);
	expect_step_paid(5, 5);

	// 6. Sell them back.
	expect_on_step(6);
	quest_on_item_sold(CHARACTER, FD, KNUCKLES);
	expect_step_paid(6, 6);

	// 7. Talk: report to the Warden.
	expect_on_step(7);
	check(says(DORAN, "Is that all?"), "Doran does not send the player to the Warden at step 7");
	expect_no_marks();
	quest_on_npc_talk(CHARACTER, FD, WARDEN);

	// The last step is handed in, not paid on the spot.
	check(holds(QUEST) && step_of(QUEST) == LAST_STEP, "the last step moved on by itself");
	check(g_step_done_count == 6, "the last step was reported as a step, not a hand-in");
	check(g_stub_xp_total == 290, "%llu XP paid before the hand-in, wanted 290",
	      (unsigned long long)g_stub_xp_total);
	check(!says(DORAN, "Is that all?"), "Doran still speaks to a finished quest");
	hand_in();
	check(g_stub_xp_total == 440, "%llu XP paid in all, wanted 440", (unsigned long long)g_stub_xp_total);
	check(ntohl(g_last_complete.xp_reward) == 150 && ntohl(g_last_complete.currency_reward) == 100,
	      "the hand-in paid %u XP and %u coin", ntohl(g_last_complete.xp_reward),
	      ntohl(g_last_complete.currency_reward));
	check(g_last_complete.item_count == 1 && ntohl(g_last_complete.items[0].item_id) == CAP,
	      "the cap was not part of the hand-in");

	// Finished: nobody has anything left, and it cannot be taken twice.
	expect_no_marks();
	check(quest_player_accept(CHARACTER, FD, QUEST) == 0, "the quest could be taken twice");
	check(!holds(QUEST), "a finished quest came back into the log");
}

static void test_steps_cannot_be_skipped(void) {
	printf("a step cannot be skipped, and the quest cannot be handed in early\n");

	new_character();
	check(quest_player_accept(CHARACTER, FD, QUEST) == 1, "the quest accepts");

	// everything but the first step's own objective changes nothing
	quest_on_item_bought(CHARACTER, FD, TONIC);
	quest_on_item_sold(CHARACTER, FD, TONIC);
	quest_on_npc_talk(CHARACTER, FD, DORAN);
	quest_on_item_bought(CHARACTER, FD, KNUCKLES);
	quest_on_npc_talk(CHARACTER, FD, WARDEN);
	check(step_of(QUEST) == 1 && progress_of(QUEST, 0) == 0, "a later step's work advanced step 1");
	check(g_step_done_count == 0, "a step was reported with nothing done");

	// the quest is not complete until its last step is, so there is nothing to hand in
	check(quest_player_turnin(CHARACTER, FD, QUEST) == 0, "the quest was handed in at step 1");
	do_steps_through(5);
	check(step_of(QUEST) == 6, "five steps done left the character on step %d", step_of(QUEST));
	check(quest_player_turnin(CHARACTER, FD, QUEST) == 0, "the quest was handed in at step 6");
	expect_no_marks();

	// finishing the last step's objective is not handing it in
	quest_on_item_sold(CHARACTER, FD, KNUCKLES);
	check(step_of(QUEST) == 7, "step 6 did not move the character to step 7");
	quest_on_npc_talk(CHARACTER, FD, WARDEN);
	check(holds(QUEST), "the quest left the log before it was handed in");
	check(quest_player_accept(CHARACTER, FD, QUEST) == 0, "the quest opened again before the hand-in");
	expect_mark(WARDEN, NPC_QUEST_MARK_READY, "Welcome to Ennara");
	hand_in();
}

static void test_a_retaken_quest_starts_over(void) {
	printf("giving the quest up and taking it again starts from the first step\n");

	new_character();
	check(quest_player_accept(CHARACTER, FD, QUEST) == 1, "the quest accepts");
	do_steps_through(2);
	check(step_of(QUEST) == 3, "two steps done left the character on step %d", step_of(QUEST));
	check(quest_player_abandon(CHARACTER, FD, QUEST) == 1, "the quest abandons");
	check(quest_player_accept(CHARACTER, FD, QUEST) == 1, "the quest accepts again");
	check(step_of(QUEST) == 1 && progress_of(QUEST, 0) == 0,
	      "a retaken quest started on step %d", step_of(QUEST));
	check(g_last_accept.step == 1, "the client was told step %u", g_last_accept.step);
}

static void test_the_step_survives_a_save(void) {
	printf("the step and its progress are what a save copies\n");

	new_character();
	check(quest_player_accept(CHARACTER, FD, QUEST) == 1, "the quest accepts");
	do_steps_through(1);
	quest_on_item_bought(CHARACTER, FD, TONIC);

	PlayerQuestState copy;

	memset(&copy, 0, sizeof(copy));
	check(quest_state_copy(&copy, &g_player.quests) == 1, "the state copies");

	struct PlayerQuestSlot* slot = quest_log_find(&copy.log, QUEST);

	check(slot != NULL, "the copy lost the quest");
	check(slot->step == 1, "the copy is on step %d, wanted 2", slot ? slot->step + 1 : 0);
	check(slot->progress[0] == 1, "the copy lost the tonic already bought");
	quest_state_release(&copy);
}

// Refresh and read back the quests the character may take.
static void refresh_available(void) {
	g_available_count = 0;
	g_available_promised = 0xFFFFFFFFu;
	quest_marks_send(CHARACTER, FD);
	check(g_available_promised == (uint32_t)g_available_count,
	      "the reset promised %u quests but %d followed", g_available_promised,
	      g_available_count);
}

// Find one quest in the list the last send produced.
static const AvailableQuestPacket* find_available(uint32_t quest_id) {
	for(int i = 0; i < g_available_count; i++)
		if(ntohl(g_available[i].quest_id) == quest_id)
			return &g_available[i];

	return NULL;
}

static void test_available_quests_are_listed(void) {
	printf("the server lists what the character may take, with what it asks and pays\n");

	new_character();

	// A stranger may take the first quest and nothing else.
	refresh_available();
	check(g_available_count == 1, "a stranger may take %d quests, wanted 1", g_available_count);

	const AvailableQuestPacket* first = find_available(QUEST);

	check(first != NULL, "the quest is not listed");
	if(!first)
		return;

	check(strcmp(first->title, "Welcome to Ennara") == 0, "title is '%s'", first->title);
	check(first->step_count == STEPS, "%u steps listed, wanted %d", first->step_count, STEPS);
	check(first->giver_npc_type == WARDEN, "giver is %u, wanted %u", first->giver_npc_type, WARDEN);
	check(first->obj_count == 1, "%u objectives, wanted 1", first->obj_count);
	check(first->has_giver_pos == 1, "the giver has no position, so the map has nowhere to mark it");
	check(first->objectives[0].objective_type == QUEST_OBJECTIVE_TALK, "objective type");
	check(strcmp(first->objectives[0].description, "Speak with Mirela the Provisioner") == 0,
	      "objective reads '%s'", first->objectives[0].description);
	check(first->objectives[0].has_marker == 0, "an unaccepted quest carries a map position");

	// what the player is promised is the whole quest's worth
	check(ntohl(first->xp_reward) == 440, "xp reward is %u", ntohl(first->xp_reward));
	check(ntohl(first->currency_reward) == 310, "coin reward is %u", ntohl(first->currency_reward));
	check(first->item_count == 1 && ntohl(first->items[0].item_id) == CAP &&
	      first->items[0].quantity == 1, "the cap is not listed as a reward");

	// Taking it moves it out of the list, and it stays out through every step.
	check(quest_player_accept(CHARACTER, FD, QUEST) == 1, "the quest accepts");
	refresh_available();
	check(g_available_count == 0, "an accepted quest is still listed");
	do_steps_through(LAST_STEP);
	refresh_available();
	check(g_available_count == 0, "the quest listed again before the hand-in");
	check(quest_player_turnin(CHARACTER, FD, QUEST) == 1, "the quest hands in");
	refresh_available();
	check(g_available_count == 0, "a finished quest is listed again");

	// Giving it up lists it again.
	new_character();
	check(quest_player_accept(CHARACTER, FD, QUEST) == 1, "the quest accepts");
	check(quest_player_abandon(CHARACTER, FD, QUEST) == 1, "the quest abandons");
	refresh_available();
	check(find_available(QUEST) != NULL, "an abandoned quest is not offered again");
}

// Replace the registry with two plain quests, for tests that need more than one.
static void load_two_plain_quests(void) {
	static const char TEXT[] =
		"{\"quests\": ["
		"{\"quest_id\": 11, \"title\": \"Eleven\", \"objectives\": "
		"[{\"type\": \"talk\", \"target_id\": 50, \"required\": 1, \"description\": \"a\"}]},"
		"{\"quest_id\": 12, \"title\": \"Twelve\", \"objectives\": "
		"[{\"type\": \"talk\", \"target_id\": 50, \"required\": 1, \"description\": \"b\"}]}"
		"]}";
	char path[] = "/tmp/quest_line_test_XXXXXX";
	int fd = mkstemp(path);

	check(fd >= 0, "a scratch quest file opens");
	check(write(fd, TEXT, sizeof(TEXT) - 1) == (ssize_t)sizeof(TEXT) - 1, "the scratch file writes");
	close(fd);
	check(quest_registry_load(path) == 1, "the scratch quests load");
	unlink(path);
}

static void test_finished_quests_are_named_on_entry(void) {
	printf("entering the world names the quests already finished\n");

	load_two_plain_quests();
	quest_state_release(&g_player.quests);
	memset(&g_player, 0, sizeof(g_player));
	g_finished_count = 0;
	check(quest_history_add(&g_player.quests.history, 11), "recording quest 11");
	check(quest_history_add(&g_player.quests.history, 12), "recording quest 12");
	check(quest_history_add(&g_player.quests.history, 4000), "recording a removed quest");
	quest_send_all(CHARACTER, FD);

	check(g_finished_count == 2, "%d finished quests named, wanted 2", g_finished_count);
	check(g_finished[0] == 12 && g_finished[1] == 11, "largest id comes first");

	// a repeatable under way again is in progress, not finished
	check(quest_player_accept(CHARACTER, FD, 11) == 0, "a finished quest is not offered again");
	quest_log_reserve_one(&g_player.quests.log);
	g_player.quests.log.slots[g_player.quests.log.count].quest_id = 11;
	g_player.quests.log.slots[g_player.quests.log.count++].is_active = 1;
	g_finished_count = 0;
	quest_send_all(CHARACTER, FD);
	check(g_finished_count == 1 && g_finished[0] == 12, "a quest in the log was called finished");

	check(quest_registry_load("world_server/data/quests.json") == 1, "the real quests load again");
}

static void test_available_quests_follow_the_level(void) {
	printf("a quest joins the list only once the level it asks for is reached\n");

	memset(&g_player, 0, sizeof(g_player));
	g_player.level = 1;
	g_player.race_id = 1;

	QuestDef* quest = (QuestDef*)quest_get(301);
	int was = quest->require_level;

	quest->require_level = 5;
	refresh_available();
	check(find_available(301) == NULL, "a level 1 character is offered a level 5 quest");

	g_player.level = 5;
	refresh_available();
	check(find_available(301) != NULL, "the quest did not open at the level it asks for");

	quest->require_level = was;
}

int main(void) {
    printf("=== Welcome to Ennara ===\n");

    check(race_registry_init("world_server/data/races.json") > 0, "races load");
    check(dialogue_system_init("world_server/data/dialogues") == 1, "dialogues load");
    check(quest_registry_load("world_server/data/quests.json") == 1, "quests load");
    items_init("world_server/data/items.json");
    check(shop_init("world_server/data/shops.json") == 1, "shops load");
    check(npc_world_init(&g_npc_world, NPC_CAPACITY_DEFAULT), "NPC pool ready");
    check(npc_spawns_load("world_server/data/spawns.json", &g_npc_world) > 0, "spawns load");

    /* What the server does at startup: the marks follow every change. */
    quest_set_change_hook(quest_marks_send);

	test_the_quest_walks_seven_steps();
	test_steps_cannot_be_skipped();
	test_a_retaken_quest_starts_over();
	test_the_step_survives_a_save();
	test_available_quests_are_listed();
	test_available_quests_follow_the_level();
	test_finished_quests_are_named_on_entry();

    quest_state_release(&g_player.quests);
    npc_world_shutdown(&g_npc_world);
    shop_cleanup();
    items_cleanup();
    quest_registry_clear();
    dialogue_system_cleanup();
    race_registry_cleanup();

    printf("\n%d checks passed\n", passed);
    return 0;
}
