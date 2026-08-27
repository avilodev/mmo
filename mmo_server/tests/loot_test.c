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

/** The players the fixture keeps online.
 *
 * Most cases use one. The pickup race needs several, because a duplication bug
 * needs two players to duplicate an item between -- one player racing itself
 * through one lock proves nothing about the path where two workers hold two
 * different players and reach for the same thing on the ground.
 */
ActivePlayer active_players[MAX_PLAYERS];

#define FIXTURE_PLAYERS 8

static uint32_t g_player_character_id = 4001;   /**< The first; slot 0. */
static int      g_player_fd           = 42;     /**< Its descriptor; slot i gets +i. */

/** The character id occupying fixture slot i. */
static uint32_t fixture_character(int slot) {
    return g_player_character_id + (uint32_t)slot;
}

/** The descriptor of the character in fixture slot i. */
static int fixture_fd(int slot) { return g_player_fd + slot; }

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

/** Report every fixture player as near every point. */
int interest_collect_fds(float x, float y, float radius, int* out_fds, int max_out) {
    (void)x; (void)y; (void)radius;
    int n = 0;
    for (int i = 0; i < FIXTURE_PLAYERS && n < max_out; i++)
        out_fds[n++] = fixture_fd(i);
    return n;
}

/** Hand back a fixture player, locked, exactly as the real registry does.
 *
 * The slot is derived from the character id rather than searched for, so the
 * race below reaches eight independently locked players and not one shared
 * one. A test whose registry handed every thread the same slot would serialise
 * the very contention it exists to create.
 */
ActivePlayer* player_acquire(uint32_t character_id) {
    if (character_id < g_player_character_id) return NULL;
    uint32_t slot = character_id - g_player_character_id;
    if (slot >= FIXTURE_PLAYERS) return NULL;
    pthread_mutex_lock(&active_players[slot].lock);
    return &active_players[slot];
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

/** One stackable definition, which is all the pickup path reads.
 *
 * Built once and never written again. The obvious stub -- fill a function
 * static on each call and hand back its address -- is a data race the moment
 * two workers ask at the same time, and ThreadSanitizer is right to say so: the
 * real item_get() returns a pointer into a table loaded before any thread
 * starts, so a stub that rewrites shared storage models something the server
 * does not do. Only the item id varies between callers, and nothing on the
 * pickup path reads it.
 */
static ItemDefinition g_item_def;

static void item_def_init(void) {
    memset(&g_item_def, 0, sizeof(g_item_def));
    g_item_def.stackable      = 1;
    g_item_def.max_stack      = 20;
    g_item_def.bind_on_pickup = 0;
    g_item_def.rarity         = RARITY_COMMON;
}

const ItemDefinition* item_get(uint32_t item_id) {
    (void)item_id;
    return &g_item_def;
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

/** Put one empty-handed player at the origin in a fixture slot. */
static void reset_player_slot(int slot) {
    memset(&active_players[slot], 0, sizeof(active_players[slot]));
    pthread_mutex_init(&active_players[slot].lock, NULL);
    active_players[slot].character_id = fixture_character(slot);
    active_players[slot].client_fd    = fixture_fd(slot);
    active_players[slot].is_loaded    = 1;
    active_players[slot].pos_x        = 0.0f;
    active_players[slot].pos_y        = 0.0f;
}

/** Put one empty-handed player at the origin. */
static void reset_player(void) { reset_player_slot(0); }

/** Put every fixture player at the origin, empty-handed. */
static void reset_all_players(void) {
    for (int i = 0; i < FIXTURE_PLAYERS; i++) reset_player_slot(i);
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

/** Total quantity of one item in one fixture player's bag. */
static int inventory_total_in(int slot, uint32_t item_id) {
    int total = 0;
    for (int i = 0; i < INVENTORY_SLOTS; i++)
        if (active_players[slot].inventory[i].item_id == item_id)
            total += active_players[slot].inventory[i].quantity;
    return total;
}

/** Total quantity of one item across the fixture player's bag. */
static int inventory_total(uint32_t item_id) { return inventory_total_in(0, item_id); }

/** Total quantity of one item held by every fixture player together. */
static int inventory_total_everywhere(uint32_t item_id) {
    int total = 0;
    for (int i = 0; i < FIXTURE_PLAYERS; i++) total += inventory_total_in(i, item_id);
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

/** One player's thread in the pickup race. */
typedef struct {
    int                 slot;
    const uint32_t*     ground_ids;
    int                 count;
    pthread_barrier_t*  start;
} PickupRaceArgs;

/** Walk every ground item, asking to pick each one up. */
static void* pickup_race_worker(void* arg) {
    PickupRaceArgs* a = arg;
    uint32_t character_id = fixture_character(a->slot);
    int      client_fd    = fixture_fd(a->slot);

    pthread_barrier_wait(a->start);

    /* Every thread walks the whole floor, so each item is contested by all
     * eight rather than by whoever happens to reach it. Two passes, because
     * the interesting failure is a second attempt at an item this thread
     * already lost -- the "Cannot pick up yet" and "Item not found" paths run
     * concurrently with somebody else's commit. */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < a->count; i++) {
            LootPickupRequestPacket req = {0};
            req.header.type         = PACKET_LOOT_PICKUP_REQUEST;
            req.header.payload_size = htons(sizeof(req) - sizeof(PacketHeader));
            req.ground_item_id      = htonl(a->ground_ids[i]);
            loot_handle_pickup_request(character_id, client_fd,
                                       (const uint8_t*)&req, (ssize_t)sizeof(req));
        }
    }
    return NULL;
}

/** Eight players racing for a floor full of loot conserve every item. */
static void test_concurrent_pickups_conserve_every_item(void) {
    printf("concurrent pickups\n");

    /* The case the whole suite was missing, and the one a duplication bug
     * lives in. test_concurrent_claims_have_one_winner races loot_reserve
     * alone, which is one lock doing one thing. This races the handler:
     * snapshot, range check, reserve, insert into a *player's* inventory,
     * then commit or restore -- five steps across two locks, with the item's
     * existence living in one and its destination in the other.
     *
     * Two properties, and both have to hold at once:
     *
     *   Nothing is duplicated. Two players cannot both bank the same drop --
     *   which is the exploit: kill something, have a friend stand on it.
     *   Nothing is destroyed. An item whose insert failed goes back on the
     *   ground; the commit-then-insert ordering this suite already tests for
     *   one player has to survive being raced by eight.
     *
     * Conservation is the assertion for both: every dropped item is either in
     * exactly one bag or still on the ground, and the two counts add up.
     */
    const uint32_t ITEM = 7777;
    enum { DROPS = 120, TRIALS = 20 };

    int trials_wrong = 0, duplicated = 0, destroyed = 0;

    for (int trial = 0; trial < TRIALS; trial++) {
        fresh_world();
        reset_all_players();

        uint32_t ids[DROPS];
        for (int i = 0; i < DROPS; i++)
            /* owner 0: unowned, so no exclusive window decides the race for
             * us. Stacked at the origin, where every fixture player stands. */
            ids[i] = loot_drop_item(ITEM, 1, 0.0f, 0.0f, 0);

        pthread_t      t[FIXTURE_PLAYERS];
        PickupRaceArgs args[FIXTURE_PLAYERS];
        pthread_barrier_t start;
        pthread_barrier_init(&start, NULL, FIXTURE_PLAYERS);

        for (int i = 0; i < FIXTURE_PLAYERS; i++) {
            args[i] = (PickupRaceArgs){ .slot = i, .ground_ids = ids,
                                        .count = DROPS, .start = &start };
            pthread_create(&t[i], NULL, pickup_race_worker, &args[i]);
        }
        for (int i = 0; i < FIXTURE_PLAYERS; i++) pthread_join(t[i], NULL);
        pthread_barrier_destroy(&start);

        int held      = inventory_total_everywhere(ITEM);
        int on_ground = (int)loot_active_count();

        if (held + on_ground != DROPS) {
            trials_wrong++;
            if (held + on_ground > DROPS) duplicated++;
            else                          destroyed++;
        }
    }

    if (duplicated)
        printf("  FAIL %d trial(s) ended with more of the item than was dropped "
               "(%s:%d)\n", duplicated, __FILE__, __LINE__);
    if (destroyed)
        printf("  FAIL %d trial(s) lost items entirely (%s:%d)\n",
               destroyed, __FILE__, __LINE__);
    if (trials_wrong) g_failures++;

    CHECK(trials_wrong == 0,
          "20 eight-way races over 120 drops each: nothing duplicated, nothing lost");

    /* And with the floor emptied, the ledger is exact rather than merely
     * balanced: every drop is in somebody's bag. */
    fresh_world();
    reset_all_players();

    uint32_t ids[DROPS];
    for (int i = 0; i < DROPS; i++)
        ids[i] = loot_drop_item(ITEM, 1, 0.0f, 0.0f, 0);

    pthread_t      t[FIXTURE_PLAYERS];
    PickupRaceArgs args[FIXTURE_PLAYERS];
    pthread_barrier_t start;
    pthread_barrier_init(&start, NULL, FIXTURE_PLAYERS);
    for (int i = 0; i < FIXTURE_PLAYERS; i++) {
        args[i] = (PickupRaceArgs){ .slot = i, .ground_ids = ids,
                                    .count = DROPS, .start = &start };
        pthread_create(&t[i], NULL, pickup_race_worker, &args[i]);
    }
    for (int i = 0; i < FIXTURE_PLAYERS; i++) pthread_join(t[i], NULL);
    pthread_barrier_destroy(&start);

    CHECK(loot_active_count() == 0, "eight players between them clear the floor");
    CHECK(inventory_total_everywhere(ITEM) == DROPS,
          "and hold exactly what was dropped, no more and no less");

    /* Each ground identifier resolved exactly once, which is what "no
     * duplication" means at the level of the pool rather than the bag. */
    int still_there = 0;
    for (int i = 0; i < DROPS; i++) {
        GroundItem snap;
        if (loot_snapshot(ids[i], &snap)) still_there++;
    }
    CHECK(still_there == 0, "and no ground item outlived its pickup");
}

/** Buying loot off the floor while it is expiring underneath. */
typedef struct {
    const uint32_t*    ground_ids;
    int                count;
    _Atomic int*       running;
    pthread_barrier_t* start;
} ExpiryArgs;

static void* expiry_worker(void* arg) {
    ExpiryArgs* a = arg;
    pthread_barrier_wait(a->start);
    /* The tick that expires ground items runs on the simulation thread while
     * pickups run on workers, so this is the real pairing. A NULL snapshot
     * expires silently, which is what the fixture wants -- what is under test
     * is the pool, not the broadcast. */
    while (atomic_load(a->running)) loot_tick(NULL);
    return NULL;
}

static void test_pickups_race_the_expiry_sweep(void) {
    printf("pickups against the expiry sweep\n");

    /* loot_tick() walks the pool and deactivates what has timed out. It runs
     * on the simulation thread; pickups run on network workers. Nothing else
     * in this suite puts the two together, and the pairing is exactly where a
     * pickup could commit an item the sweep had already retired -- crediting a
     * player for something that no longer existed. */
    const uint32_t ITEM = 8888;
    enum { DROPS = 60 };

    fresh_world();
    reset_all_players();

    uint32_t ids[DROPS];
    for (int i = 0; i < DROPS; i++)
        ids[i] = loot_drop_item(ITEM, 1, 0.0f, 0.0f, 0);

    _Atomic int running = 1;
    pthread_barrier_t start;
    pthread_barrier_init(&start, NULL, FIXTURE_PLAYERS + 1);

    pthread_t      sweeper;
    ExpiryArgs     sweep_args = { .ground_ids = ids, .count = DROPS,
                                  .running = &running, .start = &start };
    pthread_create(&sweeper, NULL, expiry_worker, &sweep_args);

    pthread_t      t[FIXTURE_PLAYERS];
    PickupRaceArgs args[FIXTURE_PLAYERS];
    for (int i = 0; i < FIXTURE_PLAYERS; i++) {
        args[i] = (PickupRaceArgs){ .slot = i, .ground_ids = ids,
                                    .count = DROPS, .start = &start };
        pthread_create(&t[i], NULL, pickup_race_worker, &args[i]);
    }
    for (int i = 0; i < FIXTURE_PLAYERS; i++) pthread_join(t[i], NULL);

    atomic_store(&running, 0);
    pthread_join(sweeper, NULL);

    /* The sweep only removes items; it never grants them. So however the two
     * interleaved, no player may hold more than was dropped -- and every item
     * a player does hold must have left the ground. */
    int held      = inventory_total_everywhere(ITEM);
    int on_ground = (int)loot_active_count();

    CHECK(held <= DROPS, "the sweep running underneath duplicated nothing");
    CHECK(held + on_ground <= DROPS,
          "and bags plus floor never exceed what was dropped");
}

int main(void) {
    log_init();
    item_def_init();   /* before any thread exists; see item_get() */

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
    test_concurrent_pickups_conserve_every_item();
    test_pickups_race_the_expiry_sweep();

    loot_cleanup();
    remove(g_empty_registry);

    if (g_failures) {
        printf("\n%d loot check(s) FAILED\n", g_failures);
        return 1;
    }
    printf("\nAll loot checks passed\n");
    return 0;
}
