/**
 * @file
 * Check party membership invariants, including under concurrent accepts.
 *
 * The party pool is read from network loop threads and written from others, and
 * its lookups hand back pointers into recyclable slots. What this suite pins
 * down is the part callers depend on: a character belongs to at most one party,
 * membership decisions are atomic, and every query answers from a consistent
 * instant rather than from live pool storage.
 */

#include "types.h"
#include "log.h"
#include "party.h"
#include "player_data.h"
#include "players_database.h"
#include "ability_def.h"
#include "quest_system.h"
#include "utils.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int g_failures = 0;

#define CHECK(cond, what)                                                   \
    do {                                                                    \
        if (cond) {                                                         \
            printf("  ok   %s\n", (what));                                  \
        } else {                                                            \
            printf("  FAIL %s  (%s:%d)\n", (what), __FILE__, __LINE__);     \
            g_failures++;                                                   \
        }                                                                   \
    } while (0)

static int g_silent_failures = 0;

/** Assert inside a loop without printing a line per iteration. */
#define CHECK_SILENT(cond, what)                                            \
    do {                                                                    \
        if (!(cond)) {                                                      \
            printf("  FAIL %s  (%s:%d)\n", (what), __FILE__, __LINE__);     \
            g_silent_failures++;                                            \
        }                                                                   \
    } while (0)

/* --- Stubs: the party module only needs players to exist and sends to work --- */

/** Stub successful database initialization. */
int character_database_init(const char* c) { (void)c; return 1; }
/** Stub database shutdown. */
void character_database_close(void) {}
/** Stub successful character persistence. */
int character_update_full_data(const CharacterInfo* d) { (void)d; return 1; }
/** Stub loading empty inventory and equipment arrays. */
int character_items_load(uint32_t c, ItemInstance* inv, int n_inv,
                         ItemInstance* eq, int n_eq) {
    (void)c;
    if (inv) memset(inv, 0, (size_t)n_inv * sizeof(*inv));
    if (eq)  memset(eq,  0, (size_t)n_eq  * sizeof(*eq));
    return 1;
}
/** Stub successful inventory persistence. */
int character_items_save(uint32_t c, const ItemInstance* inv, int n_inv,
                         const ItemInstance* eq, int n_eq) {
    (void)c; (void)inv; (void)n_inv; (void)eq; (void)n_eq; return 1;
}
/** Stub the whole-character save that commits scalars, currency and items together. */
int character_save_all(const CharacterInfo* d, const ItemInstance* inv, int n_inv,
                       const ItemInstance* eq, int n_eq) {
    (void)d; (void)inv; (void)n_inv; (void)eq; (void)n_eq; return 1;
}
/** Stub recording that a character is live in a world. */
int world_session_mark(uint32_t character_id, uint32_t world_id) {
    (void)character_id; (void)world_id; return 1;
}
/** Stub clearing that record. */
void world_session_clear(uint32_t character_id) { (void)character_id; }
/** Stub an empty persisted instance-id range. */
uint64_t character_items_max_instance_id(void) { return 0; }
/** Stub the checked form of the same query. */
int character_items_max_instance_id_checked(uint64_t* out) {
    if (out) *out = 0;
    return 1;
}
/** Stub a populated level-one character record. */
int character_get_full_data(uint32_t character_id, CharacterInfo* out) {
    memset(out, 0, sizeof(*out));
    out->character_id = character_id;
    snprintf(out->name, sizeof(out->name), "char%u", character_id);
    out->level = 1; out->health = 100; out->max_health = 100;
    out->resource = 50; out->max_resource = 50;
    out->pos_x = 100.0f; out->pos_y = 100.0f;
    return 1;
}
/** Stub class-stat application. */
void player_apply_class_stats(ActivePlayer* p) { (void)p; }
/** Stub equipment-stat application. */
void player_apply_equipment_bonuses(ActivePlayer* p) { (void)p; }
/** Stub an empty hotbar for both forms. */
int ability_get_form_abilities(uint8_t race, uint8_t form, uint16_t* out, int max) {
    (void)race; (void)form; (void)out; (void)max; return 0;
}
/** Stub the hotbar rebuild. */
void ability_refresh_hotbars(ActivePlayer* p) { (void)p; }
/** Stub the stat recompute. */
void player_recompute_stats(ActivePlayer* p) { (void)p; }
/** Stub an absent ability definition. */
const AbilityDef* ability_get(uint16_t id) { (void)id; return NULL; }
/** Stub an empty persisted quest list. */
int quest_player_load(uint32_t c, PlayerQuestState* s) { (void)c; (void)s; return 0; }
/** Stub successful quest persistence. */
int quest_player_save(uint32_t c, const PlayerQuestState* s) { (void)c; (void)s; return 1; }
/** Stub quest storage teardown. */
void quest_state_release(PlayerQuestState* s) { (void)s; }
/** Stub the snapshot's deep copy. */
int quest_state_copy(PlayerQuestState* out, const PlayerQuestState* src) {
    (void)src; if (out) memset(out, 0, sizeof(*out)); return 1;
}
/** Stub the completion set. */
int quest_history_add(QuestHistory* h, uint32_t id) { (void)h; (void)id; return 1; }
/** Count packets rather than sending them. */
static _Atomic int g_sends = 0;
ssize_t server_send(int fd, void* d, size_t n) {
    (void)fd; (void)d;
    atomic_fetch_add(&g_sends, 1);
    return (ssize_t)n;
}
/** Record awarded XP on the player, which is all the party split needs. */
void player_award_xp_locked(ActivePlayer* p, uint64_t amount) {
    if (p) p->experience += amount;
}

/* --- Helpers ------------------------------------------------------------- */

/** Bring a character online so party functions can resolve it. */
static void bring_online(uint32_t character_id) {
    int slot = -1;
    player_add_active(character_id, (int)(100 + character_id), &slot);
}

/** Read a character's cached party identifier from its slot. */
static uint32_t player_party_field(uint32_t character_id) {
    ActivePlayer* p = player_acquire(character_id);
    if (!p) return 0;
    uint32_t pid = p->party_id;
    player_release(p);
    return pid;
}

/* --- Concurrency fixture ------------------------------------------------- */

/** How many threads race to add the same character to the same party. */
#define RACERS 8

static _Atomic int g_start = 0;
static _Atomic int g_accepted = 0;
static uint32_t    g_race_party = 0;
static uint32_t    g_race_char = 0;

/** Spin until released, then attempt the same membership insertion. */
static void* race_add(void* arg) {
    (void)arg;
    while (!atomic_load(&g_start)) { }
    if (party_add_member(g_race_party, g_race_char))
        atomic_fetch_add(&g_accepted, 1);
    return NULL;
}

static uint32_t    g_race_leader = 0;
static _Atomic int g_created = 0;

/** Spin until released, then attempt to create a party for the same leader. */
static void* race_create(void* arg) {
    (void)arg;
    while (!atomic_load(&g_start)) { }
    if (party_create(g_race_leader) != 0)
        atomic_fetch_add(&g_created, 1);
    return NULL;
}

int main(void) {
    log_init();
    /* Initializes the slot table and its identifier index; the database calls it
     * makes are stubbed above. */
    playerdata_init("stub");
    party_init();

    printf("TEST 1: a party forms and reports its membership\n");
    {
        bring_online(1);
        bring_online(2);

        uint32_t pid = party_create(1);
        CHECK(pid != 0, "a party is created for an unattached leader");
        CHECK(party_add_member(pid, 2), "a second character joins");

        PartySnapshot snap;
        CHECK(party_snapshot(pid, &snap), "the party can be snapshotted by id");
        CHECK(snap.party_id == pid, "the snapshot carries the party id");
        CHECK(snap.leader_id == 1, "the creator is the leader");
        CHECK(snap.member_count == 2, "it holds two members");
        CHECK(party_has_member(pid, 1) && party_has_member(pid, 2),
              "both characters are reported as members");
        CHECK(party_is_leader(pid, 1), "the leader is reported as leader");
        CHECK(!party_is_leader(pid, 2), "the other member is not");
        CHECK(party_id_of_player(2) == pid, "the member resolves back to the party");
        CHECK(player_party_field(2) == pid, "the member's slot records the party");
    }

    printf("\nTEST 2: a character belongs to at most one party\n");
    {
        bring_online(3);
        uint32_t other = party_create(3);
        CHECK(other != 0, "a second party is created");
        CHECK(!party_add_member(other, 2),
              "a character already in a party cannot join another");
        CHECK(party_id_of_player(2) != other, "its membership is unchanged");
        CHECK(party_create(2) == 0,
              "a character already in a party cannot create one either");
        party_disband(other);
    }

    printf("\nTEST 3: absent parties and characters answer safely\n");
    {
        PartySnapshot snap;
        CHECK(!party_snapshot(0, &snap), "party id zero is not a party");
        CHECK(!party_snapshot(999999, &snap), "an unknown party id is not found");
        CHECK(!party_snapshot_of_player(0, &snap), "character id zero is in no party");
        CHECK(!party_snapshot_of_player(4242, &snap), "an offline character is in no party");
        CHECK(party_id_of_player(4242) == 0, "and resolves to no party id");
        CHECK(!party_is_leader(0, 1), "party id zero has no leader");
        CHECK(!party_has_member(0, 1), "party id zero has no members");
        CHECK(!party_snapshot(1, NULL), "a NULL output is refused");
    }

    printf("\nTEST 4: leaving updates both the party and the player\n");
    {
        bring_online(10);
        bring_online(11);
        bring_online(12);

        uint32_t pid = party_create(10);
        party_add_member(pid, 11);
        party_add_member(pid, 12);

        PartySnapshot snap;
        party_snapshot(pid, &snap);
        CHECK(snap.member_count == 3, "three members joined");

        party_remove_member(11);
        CHECK(party_id_of_player(11) == 0, "the leaver belongs to no party");
        CHECK(player_party_field(11) == 0, "and its slot no longer names one");
        CHECK(party_snapshot(pid, &snap) && snap.member_count == 2,
              "the party is down to two members");
        CHECK(party_has_member(pid, 10) && party_has_member(pid, 12),
              "the remaining members are still there");
    }

    printf("\nTEST 5: the leader leaving promotes someone else\n");
    {
        bring_online(20);
        bring_online(21);
        bring_online(22);

        uint32_t pid = party_create(20);
        party_add_member(pid, 21);
        party_add_member(pid, 22);

        party_remove_member(20);

        PartySnapshot snap;
        CHECK(party_snapshot(pid, &snap), "the party survives its leader leaving");
        CHECK(snap.leader_id != 20, "the departed leader is no longer leader");
        CHECK(snap.leader_id == 21 || snap.leader_id == 22,
              "a remaining member was promoted");
        CHECK(party_is_leader(pid, snap.leader_id), "and is reported as leader");
    }

    printf("\nTEST 6: a party disbands when it drops below two members\n");
    {
        bring_online(30);
        bring_online(31);

        uint32_t pid = party_create(30);
        party_add_member(pid, 31);
        party_remove_member(31);

        PartySnapshot snap;
        CHECK(!party_snapshot(pid, &snap), "the one-member party is gone");
        CHECK(party_id_of_player(30) == 0, "the last member belongs to no party");
        CHECK(player_party_field(30) == 0, "and its slot is cleared");
        CHECK(party_create(30) != 0, "so it can form a new party");
        party_disband(party_id_of_player(30));
    }

    printf("\nTEST 7: concurrent accepts add a character exactly once\n");
    {
        bring_online(40);
        bring_online(41);

        g_race_party = party_create(40);
        g_race_char  = 41;
        atomic_store(&g_accepted, 0);
        atomic_store(&g_start, 0);

        pthread_t threads[RACERS];
        for (int i = 0; i < RACERS; i++)
            pthread_create(&threads[i], NULL, race_add, NULL);

        atomic_store(&g_start, 1);
        for (int i = 0; i < RACERS; i++) pthread_join(threads[i], NULL);

        CHECK(atomic_load(&g_accepted) == 1,
              "exactly one of the racing accepts succeeded");

        PartySnapshot snap;
        CHECK(party_snapshot(g_race_party, &snap), "the party still exists");
        CHECK(snap.member_count == 2, "it holds the leader and one joiner");

        int occurrences = 0;
        for (int i = 0; i < MAX_PARTY_SIZE; i++)
            if (snap.members[i] == 41) occurrences++;
        CHECK(occurrences == 1, "the joiner appears in exactly one member slot");
    }

    printf("\nTEST 8: concurrent creates produce one party for one leader\n");
    {
        bring_online(50);
        g_race_leader = 50;
        atomic_store(&g_created, 0);
        atomic_store(&g_start, 0);

        pthread_t threads[RACERS];
        for (int i = 0; i < RACERS; i++)
            pthread_create(&threads[i], NULL, race_create, NULL);

        atomic_store(&g_start, 1);
        for (int i = 0; i < RACERS; i++) pthread_join(threads[i], NULL);

        CHECK(atomic_load(&g_created) == 1,
              "exactly one of the racing creates succeeded");
        CHECK(party_id_of_player(50) != 0, "the leader is in a party");
        party_disband(party_id_of_player(50));
    }

    printf("\nTEST 9: an invitation is copied out, not aliased\n");
    {
        bring_online(60);
        bring_online(61);

        PendingInvite invite;
        CHECK(!party_invite_find_for_player(61, &invite),
              "no invitation exists before one is created");
        CHECK(party_invite_create(60, 61, 0), "an invitation is stored");
        CHECK(party_invite_find_for_player(61, &invite), "it is found");
        CHECK(invite.from_id == 60 && invite.to_id == 61,
              "the copy carries both parties to the invitation");

        party_invite_remove(61);
        CHECK(!party_invite_find_for_player(61, &invite),
              "removing it makes it unfindable");
        CHECK(invite.from_id == 60,
              "the caller's earlier copy is untouched by the removal");
        CHECK(!party_invite_find_for_player(61, NULL), "a NULL output is refused");
    }

    printf("\nTEST 10: XP is split among nearby living members\n");
    {
        bring_online(70);
        bring_online(71);

        uint32_t pid = party_create(70);
        party_add_member(pid, 71);

        party_award_xp(70, 100);

        ActivePlayer* a = player_acquire(70);
        uint64_t xp_a = a ? a->experience : 0;
        if (a) player_release(a);

        ActivePlayer* b = player_acquire(71);
        uint64_t xp_b = b ? b->experience : 0;
        if (b) player_release(b);

        CHECK(xp_a == 50 && xp_b == 50, "100 XP split evenly between two members");

        party_remove_member(71);
        party_award_xp(70, 40);

        a = player_acquire(70);
        xp_a = a ? a->experience : 0;
        if (a) player_release(a);
        CHECK(xp_a == 90, "an unpartied killer keeps the whole award");
    }

    printf("\nTEST 11: disbanding clears every member\n");
    {
        bring_online(80);
        bring_online(81);
        bring_online(82);

        uint32_t pid = party_create(80);
        party_add_member(pid, 81);
        party_add_member(pid, 82);
        party_disband(pid);

        PartySnapshot snap;
        CHECK(!party_snapshot(pid, &snap), "the party is gone");
        CHECK(player_party_field(80) == 0, "the leader's slot is cleared");
        CHECK(player_party_field(81) == 0, "a member's slot is cleared");
        CHECK(player_party_field(82) == 0, "the other member's slot is cleared");
    }

    printf("\nTEST 12: name lookup answers from the index, not a scan\n");
    {
        bring_online(90);
        bring_online(91);

        CHECK(player_find_by_name("char90") == 90,
              "an online character resolves by name");
        CHECK(player_find_by_name("CHAR90") == 90,
              "and case-insensitively, the way a player types it");
        CHECK(player_fd_by_name("char91") == (int)(100 + 91),
              "the descriptor resolves too");
        CHECK(player_find_by_name("nobody") == 0,
              "a name nobody carries resolves to nothing");
        CHECK(player_fd_by_name("nobody") == -1,
              "and to no descriptor");

        /* The index must let go when the character does; a stale entry would
         * deliver someone's whisper to whoever inherits the slot. */
        player_remove_active(90);
        CHECK(player_find_by_name("char90") == 0,
              "a departed character stops resolving");
        CHECK(player_find_by_name("char91") == 91,
              "and the ones still here are unaffected");

        bring_online(90);
        CHECK(player_find_by_name("char90") == 90,
              "and resolves again after reconnecting");
    }

    printf("\nTEST 13: the party indexes survive churn\n");
    {
        /* Form and disband far more parties than the pool holds, so every slot
         * is recycled many times. A stale index entry shows up here as a
         * lookup answering with a slot that has since been given to someone
         * else -- which is exactly what the pool scans could never get wrong
         * and an index can. */
        for (int round = 0; round < 50; round++) {
            uint32_t a = 200 + (uint32_t)(round * 2);
            uint32_t b = a + 1;
            bring_online(a);
            bring_online(b);

            uint32_t pid = party_create(a);
            CHECK_SILENT(pid != 0, "a party is created each round");
            CHECK_SILENT(party_add_member(pid, b), "the second character joins");
            CHECK_SILENT(party_id_of_player(a) == pid, "the leader resolves to it");
            CHECK_SILENT(party_id_of_player(b) == pid, "the member resolves to it");

            party_disband(pid);
            CHECK_SILENT(party_id_of_player(a) == 0, "the leader is unattached again");
            CHECK_SILENT(party_id_of_player(b) == 0, "and so is the member");

            PartySnapshot gone;
            CHECK_SILENT(!party_snapshot(pid, &gone), "the disbanded id resolves to nothing");

            player_remove_active(a);
            player_remove_active(b);
        }
        CHECK(g_silent_failures == 0,
              "50 rounds of forming and disbanding leave no stale index entry");
    }

    printf("\n");
    if (g_failures) {
        printf("%d ASSERTION%s FAILED\n", g_failures, g_failures == 1 ? "" : "S");
        return 1;
    }
    printf("ALL ASSERTIONS PASSED\n");
    return 0;
}
