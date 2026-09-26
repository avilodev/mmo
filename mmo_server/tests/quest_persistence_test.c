#define _GNU_SOURCE

/**
 * @file
 * Check the quest log, the completion history, and the on-disk format.
 *
 * The three things worth proving here are the ones that used to be impossible.
 * A character can hold more quests than any compiled number. Finishing a quest
 * frees its slot and is still remembered afterwards, forever, so a prerequisite
 * check has something to read. And a save file written by the version before
 * this one still loads, with its inactive records read as history -- which is
 * what they always meant.
 *
 * The counter table added in version 3 is checked the same way, and so is the
 * thing that matters more than the round trip: a version 2 file -- every file
 * written before repeatable quests existed -- still loads, and loads with an
 * empty counter table, which is exactly correct because nobody had repeatables.
 *
 * The player, item and socket calls quest_system.c makes are stubbed; nothing
 * here grants a reward or sends a packet.
 */

#include "quest_system.h"
#include "item_instance.h"
#include "items_database.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* --- Stubs --------------------------------------------------------------- */

ActivePlayer* player_acquire(uint32_t character_id) { (void)character_id; return NULL; }
void player_release(ActivePlayer* player) { (void)player; }
void player_send_slot_updates(int client_fd, uint32_t character_id,
                              const uint16_t* slot_ids, int count) {
    (void)client_fd; (void)character_id; (void)slot_ids; (void)count;
}
const ItemDefinition* item_get(uint32_t item_id) { (void)item_id; return NULL; }
/** The bag is full here: every reward is refused, so the quest paths under test
 *  are the ones that record a turn-in without placing anything. */
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

typedef struct NPCWorldStub { int unused; } NPCWorldStub;
NPCWorldStub g_npc_world;
int npc_world_nearest_of_type(void* world, uint16_t npc_type_id,
                              float from_x, float from_y, float* out_x, float* out_y) {
    (void)world; (void)npc_type_id; (void)from_x; (void)from_y; (void)out_x; (void)out_y;
    return 0;
}

/* --- Helpers ------------------------------------------------------------- */

static int passed = 0;

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

/** Put a quest into a log directly, the way an accept would. */
static void log_push(PlayerQuestLog* log, uint32_t quest_id, int32_t progress0) {
    /* Grown through the same public path the server uses, so the test cannot
     * quietly depend on a capacity the real code would not have. */
    PlayerQuestLog grown = *log;
    if (grown.count >= grown.capacity) {
        int wanted = grown.capacity ? grown.capacity * 2 : 8;
        grown.slots = realloc(grown.slots, (size_t)wanted * sizeof(*grown.slots));
        check(grown.slots != NULL, "log allocation");
        memset(grown.slots + grown.capacity, 0,
               (size_t)(wanted - grown.capacity) * sizeof(*grown.slots));
        grown.capacity = wanted;
    }
    memset(&grown.slots[grown.count], 0, sizeof(grown.slots[0]));
    grown.slots[grown.count].quest_id    = quest_id;
    grown.slots[grown.count].is_active   = 1;
    grown.slots[grown.count].progress[0] = progress0;
    grown.count++;
    *log = grown;
}

/* --- Tests --------------------------------------------------------------- */

static void test_history_has_no_ceiling(void) {
    printf("a character can finish more quests than any fixed table would hold\n");

    PlayerQuestState state = {0};
    QuestHistory* history = &state.history;

    /* Far past the 32 the old fixed log allowed in a whole lifetime. */
    const uint32_t total = 20000;
    for (uint32_t id = 1; id <= total; id++)
        check(quest_history_add(history, id), "recording quest %u", id);

    check(quest_history_count(history) == (int)total,
          "all %u are remembered (have %d)", total, quest_history_count(history));

    for (uint32_t id = 1; id <= total; id++)
        check(quest_history_contains(history, id), "quest %u is still remembered", id);

    check(!quest_history_contains(history, total + 1), "an unfinished quest is not");
    check(!quest_history_contains(history, 0), "quest id zero is never present");

    /* Sparse identifiers cost one entry each, not a range. */
    check(quest_history_add(history, 1000000), "a sparsely numbered quest records");
    check(quest_history_contains(history, 1000000), "and is found again");

    /* Recording twice is not two entries. */
    int before = quest_history_count(history);
    quest_history_add(history, 1000000);
    check(quest_history_count(history) == before, "recording a quest twice counts once");

    quest_state_release(&state);
    check(quest_history_count(&state.history) == 0, "release empties the set");
}

static void test_log_grows_without_a_ceiling(void) {
    printf("an active quest log grows to whatever the character takes on\n");

    PlayerQuestState state = {0};
    PlayerQuestLog* log = &state.log;
    const uint32_t total = 5000;

    for (uint32_t id = 1; id <= total; id++) log_push(log, id, (int32_t)id);
    check(log->count == (int)total, "the log holds all %u", total);

    for (uint32_t id = 1; id <= total; id++) {
        struct PlayerQuestSlot* slot = quest_log_find(log, id);
        check(slot != NULL, "quest %u is in the log", id);
        check(slot->progress[0] == (int32_t)id, "quest %u kept its progress", id);
    }

    check(quest_log_find(log, total + 1) == NULL, "an absent quest is not found");

    quest_state_release(&state);
    check(log->count == 0 && log->slots == NULL, "release empties the log");
}

static void test_abandoning_frees_the_slot_and_records_nothing(void) {
    printf("abandoning a quest gives the slot back and leaves no trace\n");

    PlayerQuestState state = {0};
    log_push(&state.log, 10, 4);
    log_push(&state.log, 20, 7);
    log_push(&state.log, 30, 1);

    check(quest_log_remove(&state.log, 20), "quest 20 is abandoned");
    check(state.log.count == 2, "the log is one shorter");
    check(quest_log_find(&state.log, 20) == NULL, "and no longer holds it");
    check(quest_log_find(&state.log, 10) != NULL, "the others are untouched");
    check(quest_log_find(&state.log, 30) != NULL, "including the last one");
    check(quest_log_find(&state.log, 30)->progress[0] == 1,
          "which kept its own progress when it moved into the gap");

    /* The one thing an abandon must never do. A quest that was given up was
     * never completed, and an identifier in the history set is permanent --
     * recording one here would make the quest unavailable forever. */
    check(quest_history_count(&state.history) == 0,
          "nothing was recorded as finished");
    check(!quest_history_contains(&state.history, 20),
          "least of all the quest that was given up");

    /* Progress lives in the slot, so dropping the slot drops the progress.
     * Taking the quest again therefore starts it over, which is the only
     * answer that does not mean storing progress for quests nobody is on. */
    log_push(&state.log, 20, 0);
    check(quest_log_find(&state.log, 20)->progress[0] == 0,
          "taking it again starts from nothing");

    quest_state_release(&state);
}

static void test_abandoning_a_quest_you_do_not_have_changes_nothing(void) {
    printf("an abandon naming someone else's quest is refused\n");

    /* The abandon packet is the first thing a client may send about a quest,
     * so the rule that the named quest must actually be in the caller's own
     * log is the whole of its security. */
    PlayerQuestState state = {0};
    log_push(&state.log, 10, 4);

    check(!quest_log_remove(&state.log, 999), "a quest not in the log is refused");
    check(state.log.count == 1, "and the log is unchanged");
    check(quest_log_find(&state.log, 10)->progress[0] == 4, "progress and all");

    check(!quest_log_remove(&state.log, 0), "quest id zero is refused too");
    check(state.log.count == 1, "still unchanged");

    quest_state_release(&state);
}

static void test_round_trip(void) {
    printf("a save and load returns the same quests and the same history\n");

    char directory[] = "/tmp/mmo-quest-test-XXXXXX";
    check(mkdtemp(directory) != NULL, "temporary directory");
    quest_storage_set_dir(directory);

    PlayerQuestState state = {0};

    log_push(&state.log, 10, 4);
    log_push(&state.log, 20, 7);
    for (uint32_t id = 100; id < 400; id++) quest_history_add(&state.history, id);

    check(quest_player_save(42, &state), "save");

    PlayerQuestState loaded = {0};
    check(quest_player_load(42, &loaded), "load");

    check(loaded.log.count == 2, "both active quests came back");
    check(quest_log_find(&loaded.log, 10)->progress[0] == 4, "quest 10 kept its progress");
    check(quest_log_find(&loaded.log, 20)->progress[0] == 7, "quest 20 kept its progress");

    check(quest_history_count(&loaded.history) == 300, "the whole history came back");
    for (uint32_t id = 100; id < 400; id++)
        check(quest_history_contains(&loaded.history, id), "quest %u is still finished", id);
    check(!quest_history_contains(&loaded.history, 99), "and nothing else is");

    quest_state_release(&state);
    quest_state_release(&loaded);

    check(quest_player_load(999, &loaded) == 0, "an absent file loads nothing");

    unlink("/tmp/mmo-quest-test-nonexistent");
}

static void test_counters_survive_the_round_trip(void) {
    printf("a repeatable's count and window come back with the rest\n");

    char directory[] = "/tmp/mmo-quest-counter-XXXXXX";
    check(mkdtemp(directory) != NULL, "temporary directory");
    quest_storage_set_dir(directory);

    PlayerQuestState state = {0};
    log_push(&state.log, 10, 4);
    quest_history_add(&state.history, 10);
    quest_history_add(&state.history, 11);
    for (int i = 0; i < 3; i++) quest_counters_record(&state.counters, 11, 700);
    quest_counters_record(&state.counters, 12, 42);

    check(quest_player_save(55, &state), "save");

    PlayerQuestState loaded = {0};
    check(quest_player_load(55, &loaded), "load");

    check(quest_counters_count(&loaded.counters) == 2, "both counters came back");

    const QuestCounter* three = quest_counters_find(&loaded.counters, 11);
    check(three != NULL, "the thrice-done quest has a counter");
    check(three->count == 3, "with its lifetime count, not a fresh one");
    check(three->window == 700, "and the window its last turn-in fell in");

    const QuestCounter* once = quest_counters_find(&loaded.counters, 12);
    check(once != NULL && once->count == 1 && once->window == 42,
          "and so does the once-done one");

    check(quest_counters_find(&loaded.counters, 10) == NULL,
          "a quest with no counter did not gain one");
    check(quest_history_count(&loaded.history) == 2, "the history is untouched by any of it");

    quest_state_release(&state);
    quest_state_release(&loaded);
}

static void test_version_2_file_loads_with_no_counters(void) {
    printf("a file written before repeatable quests still loads\n");

    char directory[] = "/tmp/mmo-quest-v2-XXXXXX";
    check(mkdtemp(directory) != NULL, "temporary directory");
    quest_storage_set_dir(directory);

    /* Version 2 on disk: magic, version, and the two counts, then the slots and
     * then the finished identifiers. No counter section, because there were no
     * repeatable quests to count. */
    char path[600];
    snprintf(path, sizeof(path), "%s/9.bin", directory);
    FILE* f = fopen(path, "wb");
    check(f != NULL, "writing a version 2 file");

    uint32_t magic = 0x5153414DU, version = 2, active = 1, done = 2;
    fwrite(&magic, 4, 1, f);
    fwrite(&version, 4, 1, f);
    fwrite(&active, 4, 1, f);
    fwrite(&done, 4, 1, f);

    struct PlayerQuestSlot slot;
    memset(&slot, 0, sizeof(slot));
    slot.quest_id = 60; slot.is_active = 1; slot.progress[0] = 3;
    fwrite(&slot, sizeof(slot), 1, f);

    uint32_t finished[2] = { 61, 62 };
    fwrite(finished, sizeof(finished[0]), 2, f);
    fclose(f);

    PlayerQuestState state = {0};
    check(quest_player_load(9, &state), "the version 2 file loaded");

    check(state.log.count == 1, "its active quest came back");
    check(quest_log_find(&state.log, 60)->progress[0] == 3, "with its progress");
    check(quest_history_count(&state.history) == 2, "and both finished quests");
    check(quest_history_contains(&state.history, 61), "quest 61 is finished");
    check(quest_history_contains(&state.history, 62), "quest 62 is finished");

    /* Not a loss: nobody who saved a version 2 file had a repeatable quest. */
    check(quest_counters_count(&state.counters) == 0,
          "and the counter table is empty, which is what it always was");

    quest_state_release(&state);
}

static void test_a_corrupt_counter_count_is_refused(void) {
    printf("an absurd counter count is refused before it becomes an allocation\n");

    char directory[] = "/tmp/mmo-quest-badc-XXXXXX";
    check(mkdtemp(directory) != NULL, "temporary directory");
    quest_storage_set_dir(directory);

    char path[600];
    snprintf(path, sizeof(path), "%s/12.bin", directory);
    FILE* f = fopen(path, "wb");
    check(f != NULL, "writing a corrupt file");

    /* Sane in every field the previous version checked, absurd in the one it
     * did not know about. */
    uint32_t magic = 0x5153414DU, version = 3, active = 0, done = 0,
             counters = 0xFFFFFFF0U;
    fwrite(&magic, 4, 1, f);
    fwrite(&version, 4, 1, f);
    fwrite(&active, 4, 1, f);
    fwrite(&done, 4, 1, f);
    fwrite(&counters, 4, 1, f);
    fclose(f);

    PlayerQuestState state = {0};
    check(quest_player_load(12, &state) == 0, "the corrupt file was refused");
    check(quest_counters_count(&state.counters) == 0, "and nothing was left behind");

    quest_state_release(&state);
}

static void test_legacy_file_migrates(void) {
    printf("a file from the fixed-array version still loads, as log plus history\n");

    char directory[] = "/tmp/mmo-quest-legacy-XXXXXX";
    check(mkdtemp(directory) != NULL, "temporary directory");
    quest_storage_set_dir(directory);

    /* Version 1 on disk: a native int count, then that many records, in which a
     * finished quest was left behind with is_active clear. */
    char path[600];
    snprintf(path, sizeof(path), "%s/7.bin", directory);
    FILE* f = fopen(path, "wb");
    check(f != NULL, "writing a legacy file");

    struct PlayerQuestSlot legacy[3];
    memset(legacy, 0, sizeof(legacy));
    legacy[0].quest_id = 11; legacy[0].is_active = 1; legacy[0].progress[0] = 2;
    legacy[1].quest_id = 22;                                   /* handed in */
    legacy[2].quest_id = 33;                                   /* handed in */

    int count = 3;
    fwrite(&count, sizeof(int), 1, f);
    fwrite(legacy, sizeof(legacy[0]), 3, f);
    fclose(f);

    PlayerQuestState state = {0};
    check(quest_player_load(7, &state), "the legacy file loaded");

    check(state.log.count == 1, "only the in-progress quest is active");
    check(quest_log_find(&state.log, 11) != NULL, "quest 11 is still in progress");
    check(quest_log_find(&state.log, 11)->progress[0] == 2, "with its progress");

    check(quest_history_count(&state.history) == 2, "both finished quests became history");
    check(quest_history_contains(&state.history, 22), "quest 22 counts as finished");
    check(quest_history_contains(&state.history, 33), "quest 33 counts as finished");
    check(!quest_history_contains(&state.history, 11), "the active one does not");

    quest_state_release(&state);
}

static void test_corrupt_file_is_refused(void) {
    printf("a file claiming an absurd number of quests is refused, not allocated\n");

    char directory[] = "/tmp/mmo-quest-bad-XXXXXX";
    check(mkdtemp(directory) != NULL, "temporary directory");
    quest_storage_set_dir(directory);

    char path[600];
    snprintf(path, sizeof(path), "%s/8.bin", directory);
    FILE* f = fopen(path, "wb");
    check(f != NULL, "writing a corrupt file");

    uint32_t magic = 0x5153414DU, version = 3, active = 0xFFFFFFF0U, done = 0;
    fwrite(&magic, 4, 1, f);
    fwrite(&version, 4, 1, f);
    fwrite(&active, 4, 1, f);
    fwrite(&done, 4, 1, f);
    fclose(f);

    PlayerQuestState state = {0};
    check(quest_player_load(8, &state) == 0, "the corrupt file was refused");
    check(state.log.count == 0 && state.history.count == 0,
          "and nothing was left behind");

    quest_state_release(&state);
}

int main(void) {
    printf("=== quest storage and persistence ===\n");

    test_history_has_no_ceiling();
    test_log_grows_without_a_ceiling();
    test_abandoning_frees_the_slot_and_records_nothing();
    test_abandoning_a_quest_you_do_not_have_changes_nothing();
    test_round_trip();
    test_counters_survive_the_round_trip();
    test_version_2_file_loads_with_no_counters();
    test_a_corrupt_counter_count_is_refused();
    test_legacy_file_migrates();
    test_corrupt_file_is_refused();

    printf("\n%d checks passed\n", passed);
    return 0;
}
