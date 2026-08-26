#define _GNU_SOURCE

/**
 * @file
 * Check repeatable quests: the reset gate, the counter table, and eligibility.
 *
 * Three separable things, tested separately on purpose.
 *
 * The gate is arithmetic over a reset window and a turn-in count, and it is the
 * part most likely to be retuned later, so it is exercised with no player and
 * no file anywhere near it.
 *
 * The counters are one table per character holding one entry per repeatable
 * they have actually turned in -- deliberately *not* a wider history entry,
 * because a character finishing two thousand story quests must not pay for the
 * thirty repeatables they also did.
 *
 * Eligibility is where the two meet: a finished quest is normally the end of it,
 * and a repeatable is the one case where completion history is not the whole
 * answer. That is checked through the same quest_is_available_at() the dialogue
 * layer asks, because an offer and a grant disagreeing is the bug this design
 * exists to prevent.
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
uint16_t inventory_add(ItemInstance* slots, uint32_t item_id, uint16_t quantity,
                       uint16_t max_stack, uint8_t bind_on_pickup) {
    (void)slots; (void)item_id; (void)max_stack; (void)bind_on_pickup;
    return quantity;
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

#define DAY  86400
#define WEEK (7 * DAY)

/** A fixed instant well clear of the epoch, so windows are large numbers. */
static const time_t NOON = 1750000000;   /* 2025-06-15 around midday UTC */

/* --- The reset gate ------------------------------------------------------ */

static void test_once_is_never_offered_again(void) {
    printf("a once-only quest stays finished\n");

    check(!quest_repeat_allows(QUEST_REPEAT_ONCE, 1, 0, 0, NOON),
          "a completed once-only quest is never available again");
    check(!quest_repeat_allows(QUEST_REPEAT_ONCE, 1, 0, 0, NOON + 10 * DAY),
          "and no amount of waiting changes that");
}

static void test_free_repeat_has_no_wait(void) {
    printf("a free repeatable comes back the moment it is handed in\n");

    check(quest_repeat_allows(QUEST_REPEAT_FREE, 1, 0, 0, NOON),
          "handing it in makes it available again immediately");
    check(quest_repeat_allows(QUEST_REPEAT_FREE, 5000, 0, 0, NOON),
          "and there is no implicit ceiling on how often");
}

static void test_a_cap_stops_a_free_repeatable(void) {
    printf("max_count is what stops a repeatable, and only when set\n");

    check(quest_repeat_allows(QUEST_REPEAT_FREE, 19, 0, 20, NOON),
          "the nineteenth turn-in leaves one left");
    check(!quest_repeat_allows(QUEST_REPEAT_FREE, 20, 0, 20, NOON),
          "the twentieth is the last");
    check(!quest_repeat_allows(QUEST_REPEAT_FREE, 21, 0, 20, NOON),
          "and a count past the cap stays refused");
}

static void test_a_daily_waits_for_the_next_window(void) {
    printf("a daily comes back when the reset passes, not 24 hours later\n");

    uint16_t today = quest_repeat_window_of(QUEST_REPEAT_DAILY, NOON);

    check(!quest_repeat_allows(QUEST_REPEAT_DAILY, 1, today, 0, NOON),
          "a daily done today is not available again today");
    check(!quest_repeat_allows(QUEST_REPEAT_DAILY, 1, today, 0, NOON + 6 * 3600),
          "nor six hours later, still inside the same day");
    check(quest_repeat_allows(QUEST_REPEAT_DAILY, 1, today, 0, NOON + DAY),
          "but it is once the day rolls over");

    /* The point of a wall-clock window rather than a rolling personal timer:
     * a turn-in just before reset is available again minutes later, and every
     * player's daily flips at the same instant rather than drifting later. */
    time_t just_before = (NOON / DAY) * DAY + DAY - 60;
    uint16_t late = quest_repeat_window_of(QUEST_REPEAT_DAILY, just_before);
    check(quest_repeat_allows(QUEST_REPEAT_DAILY, 1, late, 0, just_before + 120),
          "a turn-in just before reset is available two minutes later");
}

static void test_a_weekly_groups_seven_days(void) {
    printf("a weekly waits for its own, longer window\n");

    uint16_t this_week = quest_repeat_window_of(QUEST_REPEAT_WEEKLY, NOON);

    check(!quest_repeat_allows(QUEST_REPEAT_WEEKLY, 1, this_week, 0, NOON + 3 * DAY),
          "three days later is still the same week");
    check(quest_repeat_allows(QUEST_REPEAT_WEEKLY, 1, this_week, 0, NOON + WEEK),
          "a week later is not");

    check(quest_repeat_window_of(QUEST_REPEAT_WEEKLY, NOON) ==
          quest_repeat_window_of(QUEST_REPEAT_WEEKLY, NOON + 6 * DAY) ||
          quest_repeat_window_of(QUEST_REPEAT_WEEKLY, NOON) + 1 ==
          quest_repeat_window_of(QUEST_REPEAT_WEEKLY, NOON + 6 * DAY),
          "six days is either the same week or exactly the next one");
}

static void test_a_cap_outranks_a_reset(void) {
    printf("a capped daily stops for good once the cap is reached\n");

    uint16_t yesterday = (uint16_t)(quest_repeat_window_of(QUEST_REPEAT_DAILY, NOON) - 1);
    check(!quest_repeat_allows(QUEST_REPEAT_DAILY, 7, yesterday, 7, NOON),
          "the reset has passed, but the seventh of seven was the last");
}

static void test_windows_are_stable_and_ordered(void) {
    printf("a window index identifies a reset period and nothing else\n");

    check(quest_repeat_window_of(QUEST_REPEAT_DAILY, NOON) ==
          quest_repeat_window_of(QUEST_REPEAT_DAILY, NOON + 60),
          "a minute later is the same day");
    check(quest_repeat_window_of(QUEST_REPEAT_DAILY, NOON + DAY) ==
          quest_repeat_window_of(QUEST_REPEAT_DAILY, NOON) + 1,
          "a day later is the next window");

    /* ONCE and FREE have no reset, so their window is meaningless and must be
     * a constant rather than something a save file could disagree about. */
    check(quest_repeat_window_of(QUEST_REPEAT_ONCE, NOON) == 0, "once has no window");
    check(quest_repeat_window_of(QUEST_REPEAT_FREE, NOON) == 0, "free has no window");

    check(quest_repeat_window_of(QUEST_REPEAT_DAILY, 0) == 0,
          "the epoch itself is window zero, not an underflow");
}

static void test_the_window_counter_running_out_fails_open(void) {
    printf("a daily past the end of the window counter keeps working\n");

    /* A uint16 of days runs out in the twenty-second century. What matters is
     * not the date but the direction of the failure: once every instant maps to
     * the same clamped window, "is now a later window than last time" is false
     * forever, and every daily in the game would quietly stop resetting.
     *
     * Failing open costs a daily that resets imprecisely. Failing closed costs
     * a daily that never resets again, with nothing in any log to say why. */
    time_t past_the_end = (time_t)((long long)UINT16_MAX * DAY + DAY);

    check(quest_repeat_window_of(QUEST_REPEAT_DAILY, past_the_end) == UINT16_MAX,
          "the window saturates rather than wrapping to a stale value");

    check(quest_repeat_allows(QUEST_REPEAT_DAILY, 1, UINT16_MAX, 0, past_the_end),
          "and a daily last done in the saturated window is available again");
    check(quest_repeat_allows(QUEST_REPEAT_WEEKLY, 1, UINT16_MAX, 0, past_the_end * 7),
          "as is a weekly");

    /* A cap still means a cap: failing open is about the clock, not the rules. */
    check(!quest_repeat_allows(QUEST_REPEAT_DAILY, 5, UINT16_MAX, 5, past_the_end),
          "but a capped quest that has run out is still finished");
}

static void test_json_spellings(void) {
    printf("quests.json names a repeat mode by word, never by number\n");

    check(quest_repeat_mode_from_key("once")   == QUEST_REPEAT_ONCE,   "once");
    check(quest_repeat_mode_from_key("free")   == QUEST_REPEAT_FREE,   "free");
    check(quest_repeat_mode_from_key("daily")  == QUEST_REPEAT_DAILY,  "daily");
    check(quest_repeat_mode_from_key("weekly") == QUEST_REPEAT_WEEKLY, "weekly");
    check(quest_repeat_mode_from_key("nightly") == QUEST_REPEAT_MODE_COUNT,
          "an unknown spelling is reported rather than guessed at");
}

/* --- The counter table --------------------------------------------------- */

static void test_counters_hold_only_repeatables(void) {
    printf("a counter exists only for a quest that can repeat\n");

    QuestCounters counters = {0};

    check(quest_counters_find(&counters, 42) == NULL,
          "a quest never turned in has no counter");

    check(quest_counters_record(&counters, 42, 100), "recording a turn-in");
    const QuestCounter* c = quest_counters_find(&counters, 42);
    check(c != NULL, "which creates the counter");
    check(c->count == 1, "at one turn-in");
    check(c->window == 100, "in the window it happened in");

    check(quest_counters_record(&counters, 42, 101), "a second turn-in");
    c = quest_counters_find(&counters, 42);
    check(c->count == 2, "increments rather than replacing");
    check(c->window == 101, "and moves the window forward");

    check(quest_counters_count(&counters) == 1, "one quest, one entry");

    quest_counters_release(&counters);
    check(quest_counters_count(&counters) == 0, "release empties the table");
}

static void test_counters_have_no_ceiling(void) {
    printf("the counter table grows with what the character actually repeats\n");

    QuestCounters counters = {0};
    const uint32_t total = 4000;

    for (uint32_t id = 1; id <= total; id++)
        check(quest_counters_record(&counters, id, (uint16_t)(id % 1000)),
              "recording quest %u", id);

    check(quest_counters_count(&counters) == (int)total, "all %u have entries", total);
    for (uint32_t id = 1; id <= total; id++) {
        const QuestCounter* c = quest_counters_find(&counters, id);
        check(c != NULL && c->count == 1, "quest %u kept its count", id);
    }

    /* Sparse identifiers cost one entry each, exactly as the history set does. */
    check(quest_counters_record(&counters, 5000000, 1), "a sparsely numbered quest");
    check(quest_counters_find(&counters, 5000000) != NULL, "is found again");
    check(quest_counters_find(&counters, 5000001) == NULL, "and its neighbour is not");

    quest_counters_release(&counters);
}

static void test_a_count_saturates_rather_than_wrapping(void) {
    printf("a lifetime count stops at its maximum rather than wrapping to zero\n");

    QuestCounters counters = {0};
    check(quest_counters_record(&counters, 7, 0), "first turn-in");

    QuestCounter* c = (QuestCounter*)quest_counters_find(&counters, 7);
    c->count = UINT16_MAX;
    check(quest_counters_record(&counters, 7, 1), "a turn-in at the maximum");
    check(quest_counters_find(&counters, 7)->count == UINT16_MAX,
          "leaves the count at the maximum instead of wrapping to zero");

    quest_counters_release(&counters);
}

/* --- Recording a turn-in ------------------------------------------------- */

static void test_a_turnin_writes_every_table_it_belongs_in(void) {
    printf("a repeatable turn-in is recorded in history and in the counters\n");

    PlayerQuestState state = {0};

    check(quest_state_record_turnin(&state, 2, QUEST_REPEAT_FREE, NOON),
          "recording a repeatable turn-in");

    /* Both, not either. Prerequisites read the history set, so a repeatable
     * that only incremented its counter would leave anything gated behind it
     * permanently unopenable -- and the counter is what the reset gate reads,
     * so history alone could not answer when it comes back. */
    check(quest_history_contains(&state.history, 2), "history remembers it happened");
    check(quest_counters_find(&state.counters, 2) != NULL, "and the counter says how often");
    check(quest_counters_find(&state.counters, 2)->count == 1, "once so far");

    check(quest_state_record_turnin(&state, 2, QUEST_REPEAT_FREE, NOON), "a second turn-in");
    check(quest_counters_find(&state.counters, 2)->count == 2, "counts twice");
    check(quest_history_count(&state.history) == 1, "but is still one finished quest");

    quest_state_release(&state);
}

static void test_a_story_turnin_costs_no_counter(void) {
    printf("a once-only turn-in allocates no counter at all\n");

    PlayerQuestState state = {0};

    check(quest_state_record_turnin(&state, 1, QUEST_REPEAT_ONCE, NOON), "recording it");
    check(quest_history_contains(&state.history, 1), "history remembers it");

    /* The whole reason the counters are a second table: a character who
     * finishes two thousand story quests must not pay for a counter each. */
    check(quest_counters_count(&state.counters) == 0,
          "and nothing was written to the counter table");

    quest_state_release(&state);
}

static void test_a_turnin_records_the_window_it_happened_in(void) {
    printf("a daily turn-in records which reset window it fell in\n");

    PlayerQuestState state = {0};

    check(quest_state_record_turnin(&state, 3, QUEST_REPEAT_DAILY, NOON), "recording it");
    check(quest_counters_find(&state.counters, 3)->window ==
          quest_repeat_window_of(QUEST_REPEAT_DAILY, NOON),
          "the counter carries today's window");

    quest_state_release(&state);
}

/* --- Eligibility --------------------------------------------------------- */

/** Write a quest file the eligibility tests can load. */
static void write_quests(const char* path) {
    FILE* f = fopen(path, "w");
    check(f != NULL, "writing %s", path);
    fprintf(f,
        "{\"quests\":["
        " {\"quest_id\":1,\"title\":\"A Story Quest\"},"
        " {\"quest_id\":2,\"title\":\"Wolves Again\",\"repeat\":\"free\"},"
        " {\"quest_id\":3,\"title\":\"Daily Patrol\",\"repeat\":\"daily\"},"
        " {\"quest_id\":4,\"title\":\"Five Times Only\",\"repeat\":\"free\",\"max_count\":5},"
        " {\"quest_id\":5,\"title\":\"After the Story\",\"require_quest\":1}"
        "]}\n");
    fclose(f);
}

static void test_a_story_quest_is_offered_once(void) {
    printf("finishing a story quest is the end of it\n");

    ActivePlayer p;
    memset(&p, 0, sizeof(p));

    const QuestDef* q = quest_get(1);
    check(q != NULL, "the story quest loaded");
    check(quest_is_available_at(q, &p, NOON), "it is available before it is done");

    quest_history_add(&p.quests.history, 1);
    check(!quest_is_available_at(q, &p, NOON), "and not after");

    quest_state_release(&p.quests);
}

static void test_a_free_repeatable_returns_immediately(void) {
    printf("a free repeatable is offered again as soon as its slot is free\n");

    ActivePlayer p;
    memset(&p, 0, sizeof(p));

    const QuestDef* q = quest_get(2);
    check(q != NULL, "the repeatable loaded");

    quest_history_add(&p.quests.history, 2);
    quest_counters_record(&p.quests.counters, 2, 0);
    check(quest_is_available_at(q, &p, NOON), "having done it does not block it");

    /* Holding it still does, though: repeatable is not the same as duplicable. */
    check(quest_log_reserve_one(&p.quests.log), "growing the log");
    p.quests.log.slots[p.quests.log.count++].quest_id = 2;
    check(!quest_is_available_at(q, &p, NOON),
          "a repeatable already in the log is not offered a second time");

    quest_state_release(&p.quests);
}

static void test_a_repeatable_still_unlocks_what_follows_it(void) {
    printf("a repeatable turn-in counts as completion for anything gated behind it\n");

    ActivePlayer p;
    memset(&p, 0, sizeof(p));

    const QuestDef* follow_on = quest_get(5);
    check(follow_on != NULL, "the follow-on loaded");
    check(!quest_is_available_at(follow_on, &p, NOON), "it is gated to begin with");

    /* This is why a repeatable turn-in writes both tables: prerequisites read
     * the history set, so a counter alone would leave the chain unopenable. */
    quest_history_add(&p.quests.history, 1);
    check(quest_is_available_at(follow_on, &p, NOON), "and open once the gate is done");

    quest_state_release(&p.quests);
}

static void test_a_daily_returns_on_the_next_reset(void) {
    printf("a daily comes back on its own, with no dialogue change anywhere\n");

    ActivePlayer p;
    memset(&p, 0, sizeof(p));

    const QuestDef* q = quest_get(3);
    check(q != NULL, "the daily loaded");

    quest_history_add(&p.quests.history, 3);
    quest_counters_record(&p.quests.counters, 3,
                          quest_repeat_window_of(QUEST_REPEAT_DAILY, NOON));

    check(!quest_is_available_at(q, &p, NOON), "not again in the same window");
    check(quest_is_available_at(q, &p, NOON + DAY), "but yes in the next");

    quest_state_release(&p.quests);
}

static void test_a_capped_repeatable_runs_out(void) {
    printf("a capped repeatable stops being offered at its cap\n");

    ActivePlayer p;
    memset(&p, 0, sizeof(p));

    const QuestDef* q = quest_get(4);
    check(q != NULL, "the capped repeatable loaded");
    check(q->max_count == 5, "its cap was read from the file");

    quest_history_add(&p.quests.history, 4);
    for (int i = 0; i < 4; i++) quest_counters_record(&p.quests.counters, 4, 0);
    check(quest_is_available_at(q, &p, NOON), "four of five leaves one");

    quest_counters_record(&p.quests.counters, 4, 0);
    check(!quest_is_available_at(q, &p, NOON), "five of five is the end of it");

    quest_state_release(&p.quests);
}

static void test_a_missing_counter_reads_as_never_repeated(void) {
    printf("a character with history but no counter is treated as due, not broken\n");

    /* This is the shape of every character loaded from a file written before
     * counters existed: history says they did it, and nothing says when. */
    ActivePlayer p;
    memset(&p, 0, sizeof(p));

    quest_history_add(&p.quests.history, 3);
    check(quest_is_available_at(quest_get(3), &p, NOON),
          "a daily with no recorded window is available rather than stuck");

    quest_state_release(&p.quests);
}

int main(void) {
    printf("=== repeatable quests ===\n");

    test_once_is_never_offered_again();
    test_free_repeat_has_no_wait();
    test_a_cap_stops_a_free_repeatable();
    test_a_daily_waits_for_the_next_window();
    test_a_weekly_groups_seven_days();
    test_a_cap_outranks_a_reset();
    test_windows_are_stable_and_ordered();
    test_the_window_counter_running_out_fails_open();
    test_json_spellings();

    test_a_turnin_writes_every_table_it_belongs_in();
    test_a_story_turnin_costs_no_counter();
    test_a_turnin_records_the_window_it_happened_in();

    test_counters_hold_only_repeatables();
    test_counters_have_no_ceiling();
    test_a_count_saturates_rather_than_wrapping();

    char directory[] = "/tmp/mmo-quest-repeat-XXXXXX";
    check(mkdtemp(directory) != NULL, "temporary directory");
    char quests_path[600];
    snprintf(quests_path, sizeof(quests_path), "%s/quests.json", directory);
    write_quests(quests_path);
    check(quest_registry_load(quests_path) == 1, "the test quest file loads");

    test_a_story_quest_is_offered_once();
    test_a_free_repeatable_returns_immediately();
    test_a_repeatable_still_unlocks_what_follows_it();
    test_a_daily_returns_on_the_next_reset();
    test_a_capped_repeatable_runs_out();
    test_a_missing_counter_reads_as_never_repeated();

    quest_registry_clear();
    unlink(quests_path);
    rmdir(directory);

    printf("\n%d checks passed\n", passed);
    return 0;
}
