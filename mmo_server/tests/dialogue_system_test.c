/**
 * @file
 * Exercise the dialogue registry, option conditions, and session map.
 *
 * The registry and the session map are both grown at runtime, and both used to
 * be fixed arrays, so the cases that matter here are the ones the arrays could
 * not express: a dialogue numbered past any compiled ceiling, a page carrying
 * more options than a packet does, and a conversation held by a character whose
 * identifier is far larger than the old sessions[MAX_PLAYERS] index.
 *
 * player_acquire()/player_release() are stubbed rather than linked: the real
 * ones drag in the database, and what is under test is which options survive a
 * given player state, not how that state is stored.
 */

#include "dialogue_system.h"
#include "server_types.h"
#include "quest_system.h"
#include "race_registry.h"
#include "item_instance.h"
#include "items_database.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Player stub --------------------------------------------------------- */

static ActivePlayer g_stub_player;
static int          g_stub_loaded = 1;

ActivePlayer* player_acquire(uint32_t character_id) {
    if (!g_stub_loaded) return NULL;
    g_stub_player.character_id = character_id;
    return &g_stub_player;
}

void player_release(ActivePlayer* player) { (void)player; }

/* quest_system.c is linked in, because a dialogue condition now asks the quest
 * system whether a character qualifies rather than restating its rules. These
 * are the calls it makes that this test has no interest in. */
void player_send_slot_updates(int fd, uint32_t c, const uint16_t* s, int n) {
    (void)fd; (void)c; (void)s; (void)n;
}
const ItemDefinition* item_get(uint32_t item_id) { (void)item_id; return NULL; }
/** The bag is full here: rewards are refused, which is not this file's subject. */
uint16_t inventory_add_tracked(ItemInstance* slots, uint32_t item_id,
                               uint16_t quantity, uint16_t max_stack,
                               uint8_t bind_on_pickup,
                               uint16_t* changed, int max_changed,
                               int* changed_count) {
    (void)slots; (void)item_id; (void)max_stack; (void)bind_on_pickup;
    (void)changed; (void)max_changed;
    if (changed_count) *changed_count = 0;
    return quantity;
}
uint16_t inventory_add(ItemInstance* slots, uint32_t item_id, uint16_t quantity,
                       uint16_t max_stack, uint8_t bind_on_pickup) {
    return inventory_add_tracked(slots, item_id, quantity, max_stack,
                                 bind_on_pickup, NULL, 0, NULL);
}
int inventory_first_free(const ItemInstance* slots) { (void)slots; return -1; }
ssize_t server_send(int fd, void* buf, size_t len) { (void)fd; (void)buf; return (ssize_t)len; }

typedef struct { int unused; } NPCWorldStub;
NPCWorldStub g_npc_world;
int npc_world_nearest_of_type(void* world, uint16_t type_id, float fx, float fy,
                              float* ox, float* oy) {
    (void)world; (void)type_id; (void)fx; (void)fy; (void)ox; (void)oy; return 0;
}

/** Reset the stub to a bare level-one character of the given race. */
static void stub_reset(uint32_t race_id, int level) {
    quest_state_release(&g_stub_player.quests);
    memset(&g_stub_player, 0, sizeof(g_stub_player));
    g_stub_player.race_id = race_id;
    g_stub_player.level   = level;
    g_stub_loaded         = 1;
}

/** Put a quest into the stub character's active log. */
static void stub_add_quest(uint32_t quest_id, int is_active, int is_complete) {
    PlayerQuestLog* log = &g_stub_player.quests.log;
    log->slots = realloc(log->slots, (size_t)(log->count + 1) * sizeof(*log->slots));
    if (!log->slots) exit(1);
    log->capacity = log->count + 1;

    struct PlayerQuestSlot* slot = &log->slots[log->count++];
    memset(slot, 0, sizeof(*slot));
    slot->quest_id    = quest_id;
    slot->is_active   = (uint8_t)is_active;
    slot->is_complete = (uint8_t)is_complete;
}

/** Mark a quest finished for the stub character. */
static void stub_finish_quest(uint32_t quest_id) {
    quest_history_add(&g_stub_player.quests.history, quest_id);
}

/* --- Helpers ------------------------------------------------------------- */

static int passed = 0;

static void check(int condition, const char* what) {
    if (!condition) {
        printf("  FAIL: %s\n", what);
        exit(1);
    }
    passed++;
}

/** Build a document with one page whose options carry the given conditions. */
static const char* CONDITION_DOC =
"{ \"id\": 4096, \"name\": \"Gatekeeper\", \"pages\": ["
"  { \"page_num\": 0, \"text\": \"well?\", \"options\": ["
"    { \"option_id\": 1, \"text\": \"wolf only\",  \"next_page\": -1,"
"      \"conditions\": [ { \"type\": \"race\", \"value\": \"wolf\" } ] },"
"    { \"option_id\": 2, \"text\": \"bear only\",  \"next_page\": -1,"
"      \"conditions\": [ { \"type\": \"race\", \"value\": \"bear\" } ] },"
"    { \"option_id\": 3, \"text\": \"level five\", \"next_page\": -1,"
"      \"conditions\": [ { \"type\": \"min_level\", \"value\": 5 } ] },"
"    { \"option_id\": 4, \"text\": \"unstarted\",  \"next_page\": -1,"
"      \"conditions\": [ { \"type\": \"quest_not_started\", \"value\": 77 } ] },"
"    { \"option_id\": 5, \"text\": \"finished\",   \"next_page\": -1,"
"      \"conditions\": [ { \"type\": \"quest_complete\", \"value\": 77 } ] },"
"    { \"option_id\": 6, \"text\": \"handed in\",  \"next_page\": -1,"
"      \"conditions\": [ { \"type\": \"quest_turned_in\", \"value\": 77 } ] },"
"    { \"option_id\": 7, \"text\": \"anyone\",     \"next_page\": -1 }"
"  ] } ] }";

/** Count how many of a page's options a character may currently be offered. */
static int visible_count(const DialoguePageDef* page, uint32_t character_id,
                         uint8_t* ids, int max_ids) {
    return dialogue_page_visible_options(page, character_id, ids, max_ids);
}

static int has_id(const uint8_t* ids, int count, uint8_t wanted) {
    for (int i = 0; i < count; i++) if (ids[i] == wanted) return 1;
    return 0;
}

/* --- Tests --------------------------------------------------------------- */

static void test_registry_has_no_ceiling(void) {
    printf("registry accepts an identifier past any fixed table size\n");

    const char* doc = "{ \"id\": 100000, \"name\": \"Far\", \"pages\": ["
                      "{ \"page_num\": 0, \"text\": \"hello\", \"options\": [] } ] }";
    check(dialogue_load_document(doc, "far.json") == 1, "far dialogue installed");

    const DialogueDef* d = dialogue_get(100000);
    check(d != NULL, "dialogue 100000 is retrievable");
    check(strcmp(d->name, "Far") == 0, "name survived");
    check(d->page_count == 1, "one page");
}

static void test_pages_are_found_by_number(void) {
    printf("pages are addressed by their authored number, not their index\n");

    const char* doc = "{ \"id\": 7, \"name\": \"Sparse\", \"pages\": ["
                      "{ \"page_num\": 0,  \"text\": \"a\", \"options\": [] },"
                      "{ \"page_num\": 40, \"text\": \"b\", \"options\": [] } ] }";
    check(dialogue_load_document(doc, "sparse.json") == 1, "sparse dialogue installed");

    const DialogueDef* d = dialogue_get(7);
    check(dialogue_find_page(d, 40) != NULL, "page 40 resolves");
    check(strcmp(dialogue_find_page(d, 40)->text, "b") == 0, "page 40 is the right page");
    check(dialogue_find_page(d, 1) == NULL, "an unauthored number resolves to nothing");
}

static void test_conditions_select_options(void) {
    printf("conditions decide which options a character is offered\n");

    check(dialogue_load_document(CONDITION_DOC, "gatekeeper.json") == 1,
          "conditional dialogue installed");
    const DialoguePageDef* page = dialogue_find_page(dialogue_get(4096), 0);
    check(page != NULL, "page 0 resolves");
    check(page->option_count == 7, "all seven options were authored");

    uint8_t ids[8];

    /* A level-one wolf who has never heard of quest 77. */
    stub_reset(1 /* wolf */, 1);
    int n = visible_count(page, 900001, ids, 8);
    check(n == 3, "wolf sees exactly three options");
    check(has_id(ids, n, 1), "wolf sees the wolf option");
    check(!has_id(ids, n, 2), "wolf does not see the bear option");
    check(!has_id(ids, n, 3), "level one does not see the level-five option");
    check(has_id(ids, n, 4), "an unstarted quest shows the unstarted option");
    check(has_id(ids, n, 7), "the unconditional option is always offered");

    /* Same character, now level five with quest 77 finished but not handed in. */
    stub_reset(1, 5);
    stub_add_quest(77, 1, 1);
    n = visible_count(page, 900001, ids, 8);
    check(has_id(ids, n, 3), "level five sees the level-five option");
    check(!has_id(ids, n, 4), "an accepted quest is no longer unstarted");
    check(has_id(ids, n, 5), "a finished quest shows the hand-in option");
    check(!has_id(ids, n, 6), "a finished quest is not yet handed in");

    /* Handed in: the log slot is gone and the identifier is in the history. */
    stub_reset(1, 5);
    stub_finish_quest(77);
    n = visible_count(page, 900001, ids, 8);
    check(!has_id(ids, n, 5), "a handed-in quest no longer offers the hand-in");
    check(has_id(ids, n, 6), "a handed-in quest shows the handed-in option");

    /* A bear sees the bear branch and nothing of the wolf's. */
    stub_reset(2 /* bear */, 1);
    n = visible_count(page, 900001, ids, 8);
    check(has_id(ids, n, 2), "bear sees the bear option");
    check(!has_id(ids, n, 1), "bear does not see the wolf option");
}

static void test_visible_options_respect_the_wire_cap(void) {
    printf("more passing options than a packet carries are dropped, not sent\n");

    stub_reset(1, 99);
    const DialoguePageDef* page = dialogue_find_page(dialogue_get(4096), 0);

    uint8_t ids[2];
    int n = visible_count(page, 900001, ids, 2);
    check(n == 2, "only as many options as the caller has room for");
}

static void test_option_lookup_is_by_identifier(void) {
    printf("options are found by identifier, not by row\n");

    const DialoguePageDef* page = dialogue_find_page(dialogue_get(4096), 0);
    const DialogueOptionDef* option = dialogue_page_find_option(page, 5);
    check(option != NULL, "option 5 resolves");
    check(strcmp(option->text, "finished") == 0, "option 5 is the right option");
    check(dialogue_page_find_option(page, 99) == NULL, "an unknown id resolves to nothing");
}

static void test_sessions_key_on_character_id(void) {
    printf("conversations are keyed by character id, however large\n");

    /* Far past the old sessions[MAX_PLAYERS] index, which is the point. */
    const uint32_t big = 4000000000u;

    check(dialogue_session_open(big, 5, 4096), "session opened");

    DialogueSession snapshot;
    check(dialogue_session_snapshot(big, &snapshot) == 1, "session found");
    check(snapshot.npc_id == 5, "session remembers its NPC");
    check(snapshot.dialogue_id == 4096, "session remembers its dialogue");

    dialogue_session_update_page(big, 40);
    check(dialogue_session_snapshot(big, &snapshot) == 1, "session still found");
    check(snapshot.current_page == 40, "session page moved");

    dialogue_session_close(big);
    check(dialogue_session_snapshot(big, NULL) == 0, "session closed");
}

static void test_many_sessions_survive_growth(void) {
    printf("the session map grows and every conversation survives\n");

    const int count = 500;
    for (int i = 0; i < count; i++)
        check(dialogue_session_open(1000000u + (uint32_t)i * 7919u, 1, 4096),
              "session opened during growth");

    for (int i = 0; i < count; i++) {
        DialogueSession snapshot;
        check(dialogue_session_snapshot(1000000u + (uint32_t)i * 7919u, &snapshot) == 1,
              "every session is still reachable after growth");
    }

    /* Erasing repairs the probe chain; the rest must remain reachable. */
    for (int i = 0; i < count; i += 2)
        dialogue_session_close(1000000u + (uint32_t)i * 7919u);

    for (int i = 1; i < count; i += 2) {
        DialogueSession snapshot;
        check(dialogue_session_snapshot(1000000u + (uint32_t)i * 7919u, &snapshot) == 1,
              "surviving sessions are still reachable after erasures");
    }
}

static void test_shipped_dialogue_directory_loads(void) {
    printf("the shipped dialogue directory loads and its pages resolve\n");

    dialogue_system_cleanup();
    check(dialogue_system_init("world_server/data/dialogues") == 1, "dialogue directory read");
    check(dialogues_get_count() > 0, "at least one dialogue loaded");

    /* Every navigation target must exist, or a conversation dead-ends in play. */
    int checked_targets = 0;
    for (uint32_t id = 1; id < 4096; id++) {
        const DialogueDef* d = dialogue_get(id);
        if (!d) continue;
        for (int p = 0; p < d->page_count; p++) {
            const DialoguePageDef* page = &d->pages[p];
            check(page->page_num >= 0 && page->page_num <= 255,
                  "every page number fits the packet's one-byte field");
            for (int o = 0; o < page->option_count; o++) {
                int32_t next = page->options[o].next_page;
                if (next == DIALOGUE_PAGE_CLOSE) continue;
                check(dialogue_find_page(d, next) != NULL,
                      "every option leads to a page that exists");
                checked_targets++;
            }
        }
    }
    check(checked_targets > 0, "navigation targets were actually checked");
}

int main(void) {
    printf("=== dialogue system ===\n");

    /* Race conditions are resolved to identifiers at load, so the registry has
     * to exist before any dialogue is parsed -- the same ordering main() keeps. */
    check(race_registry_init("world_server/data/races.json") > 0,
          "races load before dialogues");

    test_registry_has_no_ceiling();
    test_pages_are_found_by_number();
    test_conditions_select_options();
    test_visible_options_respect_the_wire_cap();
    test_option_lookup_is_by_identifier();
    test_sessions_key_on_character_id();
    test_many_sessions_survive_growth();
    test_shipped_dialogue_directory_loads();

    dialogue_system_cleanup();
    race_registry_cleanup();

    printf("\n%d checks passed\n", passed);
    return 0;
}
