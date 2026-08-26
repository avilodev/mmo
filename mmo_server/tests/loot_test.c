/**
 * @file
 * Check the ground-item lifecycle: reserve-then-commit, snapshots, and growth.
 *
 * The two defects this suite exists to keep closed are both invisible with one
 * player and one item. Pickup used to remove the item from the world before it
 * knew the inventory would accept it, so a full bag destroyed the loot. And
 * lookups returned a pointer into the pool, which another thread was free to
 * overwrite between the lookup and the read.
 */

#include "loot.h"
#include "log.h"
#include "types.h"
#include "player_data.h"
#include "items_database.h"
#include "quest_system.h"
#include "interest.h"
#include "utils.h"

#include <arpa/inet.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
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

/* --- The world this suite runs loot against ----------------------------- */

/** The one player the fixture keeps online. */
ActivePlayer active_players[MAX_PLAYERS];

static uint32_t g_player_character_id = 4001;
static int      g_player_fd           = 42;

/** Packets the module tried to send, counted rather than delivered. */
static _Atomic int g_sends = 0;

/** Stub the send path, counting rather than writing to a socket. */
ssize_t server_send(int fd, void* buf, size_t len) {
    (void)fd; (void)buf;
    atomic_fetch_add(&g_sends, 1);
    return (ssize_t)len;
}

/** Stub the direct send path the reject case uses. */
ssize_t server_send_direct(int fd, void* buf, size_t len) {
    (void)fd; (void)buf;
    atomic_fetch_add(&g_sends, 1);
    return (ssize_t)len;
}

/** Report the fixture's single player as near every point. */
int interest_collect_fds(float x, float y, float radius, int* out_fds, int max_out) {
    (void)x; (void)y; (void)radius;
    if (max_out < 1) return 0;
    out_fds[0] = g_player_fd;
    return 1;
}

/** Hand back the fixture player, locked, exactly as the real registry does. */
ActivePlayer* player_acquire(uint32_t character_id) {
    if (character_id != g_player_character_id) return NULL;
    pthread_mutex_lock(&active_players[0].lock);
    return &active_players[0];
}

/** Release the fixture player. */
void player_release(ActivePlayer* p) {
    if (p) pthread_mutex_unlock(&p->lock);
}

/** Count slot-refresh packets without building one. */
void player_send_slot_updates(int client_fd, uint32_t character_id,
                              const uint16_t* slots, int count) {
    (void)client_fd; (void)character_id; (void)slots; (void)count;
    atomic_fetch_add(&g_sends, 1);
}

/** Stub the quest hook. */
void quest_on_item_collect(uint32_t c, int fd, uint32_t item) {
    (void)c; (void)fd; (void)item;
}

/** One stackable definition, which is all the pickup path reads. */
const ItemDefinition* item_get(uint32_t item_id) {
    static ItemDefinition def;
    memset(&def, 0, sizeof(def));
    def.id             = item_id;
    def.stackable      = 1;
    def.max_stack      = 20;
    def.bind_on_pickup = 0;
    return &def;
}

/** Never used by these cases; the tick paths under test pass a NULL snapshot. */
int tick_snapshot_query(TickSnapshot* s, float x, float y, float radius,
                        int* out, int max_out) {
    (void)s; (void)x; (void)y; (void)radius; (void)out; (void)max_out;
    return 0;
}

/* --- Fixture ------------------------------------------------------------- */

/** Empty the world floor and put one empty-handed player at the origin.
 *
 * Cases are independent: a drop left behind by one would otherwise be counted
 * by the next, and a count is what most of these assertions are.
 */
static char g_empty_registry[256];

static void fresh_world(void) {
    loot_cleanup();
    loot_init(g_empty_registry);
}

/** Put one empty-handed player at the origin. */
static void reset_player(void) {
    memset(&active_players[0], 0, sizeof(active_players[0]));
    pthread_mutex_init(&active_players[0].lock, NULL);
    active_players[0].character_id = g_player_character_id;
    active_players[0].client_fd    = g_player_fd;
    active_players[0].is_loaded    = 1;
    active_players[0].pos_x        = 0.0f;
    active_players[0].pos_y        = 0.0f;
}

/** Fill every inventory slot so no pickup can be stored. */
static void fill_inventory(void) {
    for (int i = 0; i < INVENTORY_SLOTS; i++) {
        /* instance_id is what marks a slot occupied; a nonzero item_id alone
         * leaves the slot free as far as inventory_add is concerned. */
        active_players[0].inventory[i].instance_id = 900000 + (uint64_t)i;
        active_players[0].inventory[i].item_id     = 9000 + (uint32_t)i;
        active_players[0].inventory[i].quantity    = 20;   /* at max_stack */
    }
}

/** Send one pickup request for a ground item. */
static void request_pickup(uint32_t ground_item_id) {
    LootPickupRequestPacket req = {0};
    req.header.type         = PACKET_LOOT_PICKUP_REQUEST;
    req.header.payload_size = htons(sizeof(req) - sizeof(PacketHeader));
    req.ground_item_id      = htonl(ground_item_id);

    loot_handle_pickup_request(g_player_character_id, g_player_fd,
                               (const uint8_t*)&req, (ssize_t)sizeof(req));
}

/** Total quantity of one item across the fixture player's bag. */
static int inventory_total(uint32_t item_id) {
    int total = 0;
    for (int i = 0; i < INVENTORY_SLOTS; i++)
        if (active_players[0].inventory[i].item_id == item_id)
            total += active_players[0].inventory[i].quantity;
    return total;
}

/* --- Cases --------------------------------------------------------------- */

/** A full inventory must leave the item on the ground, not destroy it. */
static void test_full_inventory_does_not_destroy_the_item(void) {
    printf("full inventory keeps the item\n");
    fresh_world();
    reset_player();
    fill_inventory();

    /* owner_id 0 so no exclusive window blocks the fixture player. */
    uint32_t gid = loot_drop_item(1234, 3, 0.0f, 0.0f, 0);
    CHECK(gid != 0, "the item reached the ground");
    CHECK(loot_active_count() == 1, "one item is on the ground");

    request_pickup(gid);

    CHECK(loot_active_count() == 1,
          "the item is still on the ground after a refused pickup");

    GroundItem snap;
    CHECK(loot_snapshot(gid, &snap), "and is still findable by identifier");
    CHECK(snap.item_id == 1234 && snap.quantity == 3,
          "with its item and quantity intact");
    CHECK(snap.reserved_by == 0,
          "and with the failed reservation released");

    /* Emptying the bag and asking again must now succeed, which is the proof
     * the refusal cost nothing. */
    reset_player();
    request_pickup(gid);
    CHECK(loot_active_count() == 0, "a second attempt with room succeeds");
    CHECK(inventory_total(1234) == 3, "and the full stack reached the bag");
}

/** A successful pickup removes the item exactly once. */
static void test_successful_pickup_removes_the_item(void) {
    printf("successful pickup\n");
    fresh_world();
    reset_player();

    uint32_t gid = loot_drop_item(77, 5, 10.0f, 10.0f, 0);
    CHECK(gid != 0, "the item reached the ground");

    request_pickup(gid);
    CHECK(loot_active_count() == 0, "the item left the ground");
    CHECK(inventory_total(77) == 5, "the stack reached the bag");

    GroundItem snap;
    CHECK(!loot_snapshot(gid, &snap), "and no longer resolves by identifier");

    /* Asking again must not credit the player a second time. */
    request_pickup(gid);
    CHECK(inventory_total(77) == 5, "a repeated request grants nothing more");
}

/** Distance is checked before anything is reserved. */
static void test_out_of_range_pickup_is_refused(void) {
    printf("range check\n");
    fresh_world();
    reset_player();

    uint32_t gid = loot_drop_item(88, 1,
                                  LOOT_PICKUP_RANGE * 4.0f, 0.0f, 0);
    request_pickup(gid);

    CHECK(loot_active_count() == 1, "a distant item stays on the ground");
    CHECK(inventory_total(88) == 0, "and nothing reached the bag");

    GroundItem snap;
    CHECK(loot_snapshot(gid, &snap) && snap.reserved_by == 0,
          "and it was never reserved");
}

/** The exclusive window belongs to the killer. */
static void test_owner_window_blocks_others(void) {
    printf("owner window\n");
    fresh_world();
    reset_player();

    /* Owned by somebody else, dropped just now. */
    uint32_t gid = loot_drop_item(99, 1, 0.0f, 0.0f, g_player_character_id + 1);
    request_pickup(gid);

    CHECK(loot_active_count() == 1, "another player's fresh drop is refused");
    CHECK(inventory_total(99) == 0, "and nothing reached the bag");
}

/** A reservation excludes a second claimant until it is resolved. */
static void test_reservation_is_exclusive(void) {
    printf("reservation exclusivity\n");
    fresh_world();
    reset_player();

    uint32_t gid = loot_drop_item(55, 2, 0.0f, 0.0f, 0);

    uint32_t item_id = 0;
    uint8_t  qty = 0;
    CHECK(loot_reserve(gid, 100, &item_id, &qty), "the first claim reserves it");
    CHECK(item_id == 55 && qty == 2, "and reports the item and quantity");

    CHECK(!loot_reserve(gid, 200, NULL, NULL),
          "a second player cannot reserve the same item");
    CHECK(loot_reserve(gid, 100, NULL, NULL),
          "the holder may re-reserve its own claim");

    loot_restore(gid, 200);
    CHECK(!loot_reserve(gid, 200, NULL, NULL),
          "a non-holder cannot release someone else's reservation");

    loot_restore(gid, 100);
    CHECK(loot_reserve(gid, 200, NULL, NULL),
          "once the holder releases it, another player may claim it");

    loot_commit(gid);
    CHECK(loot_active_count() == 0, "committing removes it from the world");
}

/** loot_snapshot copies; the caller cannot be handed pool storage. */
static void test_snapshot_is_a_copy(void) {
    printf("snapshot is a copy\n");
    fresh_world();
    reset_player();

    uint32_t gid = loot_drop_item(31, 1, 123.0f, 456.0f, 0);

    GroundItem first;
    CHECK(loot_snapshot(gid, &first), "the snapshot resolves");
    CHECK(first.pos_x == 123.0f && first.pos_y == 456.0f, "with the right position");

    /* Take the item out from under the caller. A pointer into the pool would
     * now describe a free slot, or whatever lands in it next. */
    loot_reserve(gid, 1, NULL, NULL);
    loot_commit(gid);

    CHECK(first.pos_x == 123.0f && first.pos_y == 456.0f,
          "the caller's copy is unaffected by the item being removed");
    CHECK(first.item_id == 31, "and still names the item it described");

    GroundItem second;
    CHECK(!loot_snapshot(gid, &second), "while a fresh lookup reports it gone");
}

/** The floor grows past the fixed pool the module used to have. */
static void test_ground_pool_grows(void) {
    printf("ground pool growth\n");
    fresh_world();
    reset_player();

    /* The old compiled ceiling was 256 simultaneous ground items, and the 257th
     * drop was refused with "Ground item pool full". */
    const int wanted = 700;
    int placed = 0;
    for (int i = 0; i < wanted; i++)
        if (loot_drop_item(1000 + (uint32_t)i, 1, (float)i, 0.0f, 0) != 0) placed++;

    CHECK(placed == wanted, "every drop past the old 256-slot ceiling was placed");
    CHECK(loot_active_count() == (size_t)wanted, "and all of them are on the ground");
    CHECK(loot_pool_capacity() >= (size_t)wanted, "the pool grew to hold them");

    /* Identifiers stay unique across a growth, which is what makes lookups
     * meaningful after a realloc moved everything. */
    GroundItem a, b;
    CHECK(loot_snapshot(1, &a) && loot_snapshot((uint32_t)wanted, &b),
          "the first and last items both resolve after the pool grew");
    CHECK(a.id != b.id, "and carry distinct identifiers");
}

/** Slots are reused, so a floor that empties does not keep growing. */
static void test_slots_are_reused(void) {
    printf("slot reuse\n");
    fresh_world();
    reset_player();

    size_t before = loot_pool_capacity();

    for (int round = 0; round < 5; round++) {
        uint32_t ids[64];
        for (int i = 0; i < 64; i++)
            ids[i] = loot_drop_item(2000 + (uint32_t)i, 1, (float)i, 0.0f, 0);
        for (int i = 0; i < 64; i++) {
            loot_reserve(ids[i], 1, NULL, NULL);
            loot_commit(ids[i]);
        }
    }

    CHECK(loot_active_count() == 0, "the floor is empty again");
    CHECK(loot_pool_capacity() == before,
          "and the pool did not grow across five drop-and-clear rounds");
}

/** Concurrent claimants: exactly one may take an item. */
typedef struct {
    uint32_t     ground_id;
    uint32_t     player_id;
    _Atomic int* winners;
    pthread_barrier_t* start;
} ClaimArgs;

static void* claim_worker(void* arg) {
    ClaimArgs* a = arg;
    pthread_barrier_wait(a->start);
    if (loot_reserve(a->ground_id, a->player_id, NULL, NULL))
        atomic_fetch_add(a->winners, 1);
    return NULL;
}

/** Eight threads race for one item; exactly one wins. */
static void test_concurrent_claims_have_one_winner(void) {
    printf("concurrent claims\n");
    fresh_world();
    reset_player();

    for (int trial = 0; trial < 50; trial++) {
        uint32_t gid = loot_drop_item(4242, 1, 0.0f, 0.0f, 0);

        enum { THREADS = 8 };
        pthread_t t[THREADS];
        ClaimArgs args[THREADS];
        _Atomic int winners = 0;
        pthread_barrier_t start;
        pthread_barrier_init(&start, NULL, THREADS);

        for (int i = 0; i < THREADS; i++) {
            args[i] = (ClaimArgs){ .ground_id = gid,
                                   .player_id = 500 + (uint32_t)i,
                                   .winners = &winners,
                                   .start = &start };
            pthread_create(&t[i], NULL, claim_worker, &args[i]);
        }
        for (int i = 0; i < THREADS; i++) pthread_join(t[i], NULL);
        pthread_barrier_destroy(&start);

        if (atomic_load(&winners) != 1) {
            printf("  FAIL %d threads claimed one item (%s:%d)\n",
                   atomic_load(&winners), __FILE__, __LINE__);
            g_failures++;
            return;
        }

        /* Clear it for the next trial. */
        GroundItem snap;
        if (loot_snapshot(gid, &snap)) {
            loot_restore(gid, snap.reserved_by);
            loot_reserve(gid, 1, NULL, NULL);
            loot_commit(gid);
        }
    }
    CHECK(1, "50 eight-way races each produced exactly one winner");
}

int main(void) {
    log_init();

    /* A registry with no loot_tables section: these cases exercise the
     * ground-item pool, and item definitions come from the item_get() stub. */
    snprintf(g_empty_registry, sizeof(g_empty_registry),
             "/tmp/loot_test_registry_%d.json", (int)getpid());
    FILE* f = fopen(g_empty_registry, "w");
    if (f) { fputs("{ \"items\": [] }\n", f); fclose(f); }

    loot_init(g_empty_registry);
    CHECK(loot_pool_capacity() > 0, "the ground-item pool was allocated");

    test_full_inventory_does_not_destroy_the_item();
    test_successful_pickup_removes_the_item();
    test_out_of_range_pickup_is_refused();
    test_owner_window_blocks_others();
    test_reservation_is_exclusive();
    test_snapshot_is_a_copy();
    test_ground_pool_grows();
    test_slots_are_reused();
    test_concurrent_claims_have_one_winner();

    loot_cleanup();
    remove(g_empty_registry);

    if (g_failures) {
        printf("\n%d loot check(s) FAILED\n", g_failures);
        return 1;
    }
    printf("\nAll loot checks passed\n");
    return 0;
}
