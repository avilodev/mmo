#define _GNU_SOURCE

/**
 * @file
 * Check that a player can give up a quest, and that giving up costs nothing.
 *
 * Abandoning is the first thing in the quest protocol a client may start. Every
 * other quest packet is server-to-client, and quests are taken and handed in
 * through dialogue, so this is the only quest opcode whose payload a player
 * chooses. That property is worth keeping deliberately, and it is why the two
 * cases below matter more than the happy path: an abandon naming a quest the
 * caller does not hold must change nothing and send nothing, and an abandon
 * must never write to the completion history, where an identifier is permanent
 * and would make the quest unavailable for the rest of the character's life.
 *
 * The player is stubbed rather than linked, the way dialogue_system_test does
 * it: what is under test is what the handler does to a character's log, not how
 * that character is stored.
 */

#include "quest_system.h"
#include "item_instance.h"
#include "items_database.h"

#include <arpa/inet.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* --- Stubs --------------------------------------------------------------- */

static ActivePlayer g_player;
static int          g_online = 1;

ActivePlayer* player_acquire(uint32_t character_id) {
    if (!g_online) return NULL;
    g_player.character_id = character_id;
    return &g_player;
}
void player_release(ActivePlayer* player) { (void)player; }

void player_send_slot_updates(int fd, uint32_t c, const uint16_t* s, int n) {
    (void)fd; (void)c; (void)s; (void)n;
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

typedef struct NPCWorldStub { int unused; } NPCWorldStub;
NPCWorldStub g_npc_world;
int npc_world_nearest_of_type(void* w, uint16_t t, float fx, float fy, float* ox, float* oy) {
    (void)w; (void)t; (void)fx; (void)fy; (void)ox; (void)oy; return 0;
}

/** Record what the server sent, so the test can assert on the wire. */
static struct {
    int      count;
    uint8_t  last_type;
    uint32_t last_quest_id;
} g_sent;

ssize_t server_send(int fd, void* buf, size_t len) {
    (void)fd;
    const PacketHeader* header = (const PacketHeader*)buf;
    g_sent.count++;
    g_sent.last_type = header->type;
    if (len >= sizeof(QuestAbandonPacket) && header->type == PACKET_QUEST_ABANDONED)
        g_sent.last_quest_id = ntohl(((const QuestAbandonPacket*)buf)->quest_id);
    return (ssize_t)len;
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

/** Reset the stub character and the send log. */
static void reset(void) {
    quest_state_release(&g_player.quests);
    memset(&g_player, 0, sizeof(g_player));
    memset(&g_sent, 0, sizeof(g_sent));
    g_online = 1;
}

/** Put a quest into the stub character's log, as an accept would. */
static void give_quest(uint32_t quest_id, int32_t progress0, int complete) {
    check(quest_log_reserve_one(&g_player.quests.log), "growing the log");
    struct PlayerQuestSlot* slot = &g_player.quests.log.slots[g_player.quests.log.count++];
    memset(slot, 0, sizeof(*slot));
    slot->quest_id    = quest_id;
    slot->is_active   = 1;
    slot->is_complete = (uint8_t)complete;
    slot->progress[0] = progress0;
}

/** Write a quest file with one ordinary quest in it. */
static void write_quests(const char* path) {
    FILE* f = fopen(path, "w");
    check(f != NULL, "writing %s", path);
    fprintf(f, "{\"quests\":[{\"quest_id\":7,\"title\":\"A Long Errand\"},"
               "{\"quest_id\":8,\"title\":\"Another\"}]}\n");
    fclose(f);
}

/* --- Tests --------------------------------------------------------------- */

static void test_abandoning_an_active_quest(void) {
    printf("giving up a quest frees its slot and says so\n");
    reset();

    give_quest(7, 3, 0);
    give_quest(8, 1, 0);

    check(quest_player_abandon(1, 99, 7), "the abandon succeeds");
    check(g_player.quests.log.count == 1, "the log is one shorter");
    check(quest_log_find(&g_player.quests.log, 7) == NULL, "and no longer holds the quest");
    check(quest_log_find(&g_player.quests.log, 8) != NULL, "the other one is untouched");
    check(g_player.is_dirty, "the character is marked for saving");

    check(g_sent.count == 1, "exactly one packet went out");
    check(g_sent.last_type == PACKET_QUEST_ABANDONED, "and it confirms the abandon");
    check(g_sent.last_quest_id == 7, "naming the quest that was given up");
}

static void test_abandoning_records_no_completion(void) {
    printf("a quest given up was never finished\n");
    reset();

    give_quest(7, 3, 0);
    check(quest_player_abandon(1, 99, 7), "the abandon succeeds");

    /* The history set never forgets, so an entry here would take the quest
     * away for the rest of the character's life. */
    check(quest_history_count(&g_player.quests.history) == 0, "nothing entered the history");
    check(!quest_history_contains(&g_player.quests.history, 7), "least of all this quest");
    check(quest_counters_count(&g_player.quests.counters) == 0, "and no counter either");

    check(quest_is_available_at(quest_get(7), &g_player, 1750000000),
          "so the quest can simply be taken again");
}

static void test_abandoning_discards_progress(void) {
    printf("taking a quest again after giving it up starts it over\n");
    reset();

    give_quest(7, 3, 0);
    check(quest_player_abandon(1, 99, 7), "the abandon succeeds");

    give_quest(7, 0, 0);
    check(quest_log_find(&g_player.quests.log, 7)->progress[0] == 0,
          "progress lives in the slot, so dropping the slot dropped it");
}

static void test_a_completed_quest_can_still_be_given_up(void) {
    printf("a quest finished but not handed in can still be given up\n");
    reset();

    give_quest(7, 5, 1);
    check(quest_player_abandon(1, 99, 7), "the abandon succeeds");
    check(g_player.quests.log.count == 0, "the slot is free");
    check(quest_history_count(&g_player.quests.history) == 0,
          "and finishing the objectives is still not the same as handing it in");
}

static void test_a_quest_the_character_does_not_hold(void) {
    printf("an abandon naming a quest the caller does not hold is refused\n");
    reset();

    give_quest(7, 3, 0);

    check(!quest_player_abandon(1, 99, 8), "a quest that exists but is not held");
    check(!quest_player_abandon(1, 99, 4242), "a quest that does not exist at all");
    check(!quest_player_abandon(1, 99, 0), "and quest id zero");

    check(g_player.quests.log.count == 1, "the log is unchanged");
    check(quest_log_find(&g_player.quests.log, 7)->progress[0] == 3, "progress and all");
    check(g_sent.count == 0, "and nothing was sent back");
}

static void test_an_offline_character(void) {
    printf("an abandon for a character who is not loaded is refused\n");
    reset();

    give_quest(7, 3, 0);
    g_online = 0;

    check(!quest_player_abandon(1, 99, 7), "the abandon is refused");
    check(g_sent.count == 0, "and nothing was sent");
}

int main(void) {
    printf("=== abandoning a quest ===\n");

    char directory[] = "/tmp/mmo-quest-abandon-XXXXXX";
    check(mkdtemp(directory) != NULL, "temporary directory");
    char quests_path[600];
    snprintf(quests_path, sizeof(quests_path), "%s/quests.json", directory);
    write_quests(quests_path);
    check(quest_registry_load(quests_path) == 1, "the test quest file loads");

    test_abandoning_an_active_quest();
    test_abandoning_records_no_completion();
    test_abandoning_discards_progress();
    test_a_completed_quest_can_still_be_given_up();
    test_a_quest_the_character_does_not_hold();
    test_an_offline_character();

    reset();
    quest_registry_clear();
    remove(quests_path);
    rmdir(directory);

    printf("\n%d checks passed\n", passed);
    return 0;
}
