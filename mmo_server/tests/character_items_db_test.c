/**
 * @file
 * Exercise inventory persistence against PostgreSQL using an isolated character identifier.
 * Set MMO_TEST_PGCONN to run the test; an unset variable produces a successful skip.
 */

#include "players_database.h"
#include "item_instance.h"
#include "log.h"

#include <libpq-fe.h>

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

/** The test's own connection, for the fixture rows the module has no API for.
 *
 * character_items rows now carry a foreign key to characters, so a bag cannot
 * be saved for a character that does not exist -- which is the whole point of
 * the constraint, and which means this test has to create one.
 */
static PGconn* g_fixture = NULL;

/** Run one statement on the fixture connection.
 *
 * @return 1 when the command succeeded, otherwise 0.
 */
static int fixture_exec(const char* sql) {
    PGresult* res = PQexec(g_fixture, sql);
    int ok = (PQresultStatus(res) == PGRES_COMMAND_OK ||
              PQresultStatus(res) == PGRES_TUPLES_OK);
    if (!ok) fprintf(stderr, "fixture: %s\n  %s\n", sql, PQerrorMessage(g_fixture));
    PQclear(res);
    return ok;
}

/** Create the character these items belong to. */
static void create_test_character(void) {
    char sql[512];
    snprintf(sql, sizeof(sql),
             "INSERT INTO characters (character_id, account_id, world_id, name, "
             "                        class_id, race_id) "
             "VALUES (%u, 1, 1, 'db_test_character', 1, 1) "
             "ON CONFLICT (character_id) DO NOTHING;",
             TEST_CHARACTER_ID);
    assert(fixture_exec(sql));
}

/** Remove the character, which removes its items with it.
 *
 * By name as well as by identifier: `characters` carries UNIQUE(name, world_id),
 * so a row left behind by an interrupted run under a different identifier would
 * make the insert below fail on a constraint the identifier clause cannot see.
 */
static void delete_test_character(void) {
    char sql[256];
    snprintf(sql, sizeof(sql),
             "DELETE FROM characters WHERE character_id = %u "
             "   OR (name = 'db_test_character' AND world_id = 1);",
             TEST_CHARACTER_ID);
    assert(fixture_exec(sql));
}

/** Count this character's persisted item rows.
 *
 * @return The row count, or -1 when the query fails.
 */
static int count_item_rows(void) {
    char sql[256];
    snprintf(sql, sizeof(sql),
             "SELECT COUNT(*) FROM character_items WHERE character_id = %u;",
             TEST_CHARACTER_ID);

    PGresult* res = PQexec(g_fixture, sql);
    int count = -1;
    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) == 1)
        count = atoi(PQgetvalue(res, 0, 0));
    PQclear(res);
    return count;
}

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

    /* A second connection of the test's own, for the fixture rows. Opened
     * after character_database_init(), which is what creates the schema. */
    g_fixture = PQconnectdb(conn);
    if (PQstatus(g_fixture) != CONNECTION_OK) {
        fprintf(stderr, "FAILED: fixture connection: %s\n", PQerrorMessage(g_fixture));
        return 1;
    }

    /* Start from nothing, then create the character these items hang off. */
    delete_test_character();
    create_test_character();

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

    printf("\nTEST 7: deleting the character takes its items with it\n");
    /* Give it something to lose, first. */
    clear_all();
    assert(inventory_add(inv, 4000, 3, 50, 0) == 0);
    equip[EQUIP_HELMET].instance_id = item_instance_next_id();
    equip[EQUIP_HELMET].item_id     = 2000;
    equip[EQUIP_HELMET].quantity    = 1;
    assert(character_items_save(TEST_CHARACTER_ID, inv, INVENTORY_SLOTS,
                                equip, EQUIP_SLOTS) == 1);

    int rows_before = count_item_rows();
    assert(rows_before > 0);
    printf("  %d item row(s) before the delete\n", rows_before);

    /* Before the foreign key existed, these rows outlived the character
     * forever -- still owning their instance identifiers, still occupying
     * UNIQUE(character_id, slot) for a character that no longer existed. */
    delete_test_character();
    assert(count_item_rows() == 0);
    printf("  0 item rows after it, cascaded by the foreign key\n");

    printf("\nTEST 8: items cannot be saved for a character that does not exist\n");
    /* The other half of the same constraint: a save for a deleted character is
     * refused rather than silently re-creating orphans. The world server used
     * to do exactly that on its next periodic save after a mid-session
     * deletion. */
    assert(character_items_save(TEST_CHARACTER_ID, inv, INVENTORY_SLOTS,
                                equip, EQUIP_SLOTS) == 0);
    assert(count_item_rows() == 0);
    printf("  the save was refused and left nothing behind\n");

    /* Put the character back for the remaining cases. */
    create_test_character();
    clear_all();

    printf("\nTEST 9: one instance in two slots at once is refused\n");
    /* The save sends every retained row in one statement, which cannot write
     * the same instance twice. Neither should it want to: an identifier in two
     * slots at once is a duplicated item, and the per-row loop this replaced
     * would have written the second row over the first and reported success --
     * persisting the duplication as an ordinary move and losing one of the two
     * stacks without a word. The refusal is what makes that visible.
     *
     * The forged state is built by hand because nothing in the inventory API
     * can produce it; that is the point. */
    memset(inv,   0, sizeof(ItemInstance) * INVENTORY_SLOTS);
    memset(equip, 0, sizeof(ItemInstance) * EQUIP_SLOTS);
    assert(inventory_add(inv, 4000, 5, 50, 0) == 0);
    assert(inv[0].instance_id != 0);
    inv[7] = inv[0];                       /* the same instance, twice */

    assert(character_items_save(TEST_CHARACTER_ID, inv, INVENTORY_SLOTS,
                                equip, EQUIP_SLOTS) == 0);
    assert(count_item_rows() == 0);
    printf("  the save was refused and wrote nothing\n");

    /* And the same state minus the duplicate saves normally, so the refusal
     * above is about the duplication and not about the fixture. */
    inv[7] = (ItemInstance){0};
    assert(character_items_save(TEST_CHARACTER_ID, inv, INVENTORY_SLOTS,
                                equip, EQUIP_SLOTS) == 1);
    assert(count_item_rows() == 1);
    printf("  without the duplicate, the same bag saves\n");

    clear_all();
    memset(inv,   0, sizeof(ItemInstance) * INVENTORY_SLOTS);
    memset(equip, 0, sizeof(ItemInstance) * EQUIP_SLOTS);
    assert(character_items_save(TEST_CHARACTER_ID, inv, INVENTORY_SLOTS,
                                equip, EQUIP_SLOTS) == 1);
    assert(character_items_load(TEST_CHARACTER_ID, loaded_inv, INVENTORY_SLOTS,
                                loaded_equip, EQUIP_SLOTS) == 1);
    assert(occupied(loaded_inv, INVENTORY_SLOTS) == 0);

    /* Leave the database as this test found it. */
    delete_test_character();
    PQfinish(g_fixture);
    g_fixture = NULL;

    character_database_close();
    printf("\nALL ASSERTIONS PASSED\n");
    return 0;
}
