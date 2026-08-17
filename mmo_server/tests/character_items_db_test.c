/**
 * @file
 * Exercise inventory persistence against PostgreSQL using an isolated character identifier.
 * Set MMO_TEST_PGCONN to run the test; an unset variable produces a successful skip.
 */

#include "players_database.h"
#include "item_instance.h"
#include "log.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** Character identifier reserved above the database SERIAL range. */
#define TEST_CHARACTER_ID 2147483601u

static ItemInstance inv[INVENTORY_SLOTS];
static ItemInstance equip[EQUIP_SLOTS];
static ItemInstance loaded_inv[INVENTORY_SLOTS];
static ItemInstance loaded_equip[EQUIP_SLOTS];

static void clear_all(void) {
    memset(inv, 0, sizeof(inv));
    memset(equip, 0, sizeof(equip));
    memset(loaded_inv, 0, sizeof(loaded_inv));
    memset(loaded_equip, 0, sizeof(loaded_equip));
}

/** Save the working arrays and reload them into comparison arrays. */
static void save_and_reload(void) {
    assert(character_items_save(TEST_CHARACTER_ID, inv, INVENTORY_SLOTS,
                                equip, EQUIP_SLOTS) == 1);
    memset(loaded_inv, 0, sizeof(loaded_inv));
    memset(loaded_equip, 0, sizeof(loaded_equip));
    assert(character_items_load(TEST_CHARACTER_ID, loaded_inv, INVENTORY_SLOTS,
                                loaded_equip, EQUIP_SLOTS) == 1);
}

static int occupied(const ItemInstance* a, int n) {
    int c = 0;
    for (int i = 0; i < n; i++) if (a[i].instance_id) c++;
    return c;
}

/** Assert that two item-instance arrays contain identical persisted fields. */
static void expect_same(const ItemInstance* a, const ItemInstance* b, int n,
                        const char* what) {
    for (int i = 0; i < n; i++) {
        if (a[i].instance_id != b[i].instance_id ||
            a[i].item_id     != b[i].item_id     ||
            a[i].quantity    != b[i].quantity    ||
            a[i].is_bound    != b[i].is_bound) {
            fprintf(stderr,
                    "%s slot %d differs: saved {id=%llu item=%u qty=%u bound=%u} "
                    "loaded {id=%llu item=%u qty=%u bound=%u}\n",
                    what, i,
                    (unsigned long long)a[i].instance_id, a[i].item_id,
                    a[i].quantity, a[i].is_bound,
                    (unsigned long long)b[i].instance_id, b[i].item_id,
                    b[i].quantity, b[i].is_bound);
            assert(0);
        }
    }
}

/**
 * Run live database round-trip checks when configured.
 *
 * @return      Zero for a successful run or configuration skip, or one on connection failure.
 */
int main(void) {
    log_init();
    log_set_level(LOG_LEVEL_ERROR);

    const char* conn = getenv("MMO_TEST_PGCONN");
    if (!conn || !*conn) {
        printf("SKIPPED: set MMO_TEST_PGCONN to run the database round-trip test\n");
        printf("  e.g. MMO_TEST_PGCONN=\"host=127.0.0.1 dbname=prototype_db "
               "user=postgres password=...\"\n");
        return 0;
    }

    if (!character_database_init(conn)) {
        fprintf(stderr, "FAILED: could not connect using MMO_TEST_PGCONN\n");
        return 1;
    }

    clear_all();

    // Leave nothing behind from an interrupted earlier run.
    assert(character_items_save(TEST_CHARACTER_ID, inv, INVENTORY_SLOTS,
                                equip, EQUIP_SLOTS) == 1);

    printf("TEST 1: an empty character loads as empty\n");
    assert(character_items_load(TEST_CHARACTER_ID, loaded_inv, INVENTORY_SLOTS,
                                loaded_equip, EQUIP_SLOTS) == 1);
    assert(occupied(loaded_inv, INVENTORY_SLOTS) == 0);
    assert(occupied(loaded_equip, EQUIP_SLOTS) == 0);
    printf("  0 bag rows, 0 equipment rows\n");

    printf("\nTEST 2: bag and equipment survive a round trip\n");
    item_instance_seed(character_items_max_instance_id());

    assert(inventory_add(inv, 4000, 25, 50, 0) == 0);   // stackable potion
    assert(inventory_add(inv, 1,    60, 100, 0) == 0);  // stackable feathers
    assert(inventory_add(inv, 1000, 1,  1, 0) == 0);    // unstackable weapon

    equip[EQUIP_HELMET].instance_id = item_instance_next_id();
    equip[EQUIP_HELMET].item_id     = 2000;
    equip[EQUIP_HELMET].quantity    = 1;
    equip[EQUIP_HELMET].is_bound    = 1;                // binding must persist

    save_and_reload();
    expect_same(inv, loaded_inv, INVENTORY_SLOTS, "inventory");
    expect_same(equip, loaded_equip, EQUIP_SLOTS, "equipment");
    printf("  %d bag stacks and %d worn items reloaded identically\n",
           occupied(inv, INVENTORY_SLOTS), occupied(equip, EQUIP_SLOTS));
    assert(loaded_equip[EQUIP_HELMET].is_bound == 1);
    printf("  is_bound survived the round trip\n");

    printf("\nTEST 3: moving an item keeps its instance id\n");
    // preserve instance identity across slot moves
    uint64_t id_before = inv[0].instance_id;
    assert(inventory_move(inv, 0, 40, 50) == 1);
    assert(inv[40].instance_id == id_before);

    save_and_reload();
    assert(loaded_inv[0].instance_id == 0);
    assert(loaded_inv[40].instance_id == id_before);
    expect_same(inv, loaded_inv, INVENTORY_SLOTS, "inventory");
    printf("  instance %llu moved slot 0 -> 40, id unchanged\n",
           (unsigned long long)id_before);

    printf("\nTEST 4: consumed items are deleted, not left behind\n");
    uint64_t doomed = inv[1].instance_id;
    assert(doomed != 0);
    assert(inventory_remove_at(inv, 1, 60) == 60);   // whole stack
    assert(inv[1].instance_id == 0);

    save_and_reload();
    assert(loaded_inv[1].instance_id == 0);
    for (int i = 0; i < INVENTORY_SLOTS; i++)
        assert(loaded_inv[i].instance_id != doomed);   // gone everywhere
    expect_same(inv, loaded_inv, INVENTORY_SLOTS, "inventory");
    printf("  instance %llu no longer present anywhere\n",
           (unsigned long long)doomed);

    printf("\nTEST 5: quantity changes persist\n");
    int qty_slot = -1;
    for (int i = 0; i < INVENTORY_SLOTS; i++)
        if (loaded_inv[i].instance_id && loaded_inv[i].quantity > 1) { qty_slot = i; break; }
    assert(qty_slot >= 0);

    uint16_t before = inv[qty_slot].quantity;
    assert(inventory_remove_at(inv, qty_slot, 5) == 5);
    save_and_reload();
    assert(loaded_inv[qty_slot].quantity == (uint16_t)(before - 5));
    printf("  slot %d quantity %u -> %u persisted\n",
           qty_slot, before, loaded_inv[qty_slot].quantity);

    printf("\nTEST 6: the id allocator is seeded above everything saved\n");
    uint64_t highest = character_items_max_instance_id();
    item_instance_seed(highest);
    uint64_t fresh = item_instance_next_id();
    assert(fresh > highest);
    printf("  max saved id %llu, next allocated %llu\n",
           (unsigned long long)highest, (unsigned long long)fresh);

    clear_all();
    assert(character_items_save(TEST_CHARACTER_ID, inv, INVENTORY_SLOTS,
                                equip, EQUIP_SLOTS) == 1);
    assert(character_items_load(TEST_CHARACTER_ID, loaded_inv, INVENTORY_SLOTS,
                                loaded_equip, EQUIP_SLOTS) == 1);
    assert(occupied(loaded_inv, INVENTORY_SLOTS) == 0);

    character_database_close();
    printf("\nALL ASSERTIONS PASSED\n");
    return 0;
}
