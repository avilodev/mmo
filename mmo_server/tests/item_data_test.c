/**
 * @file
 * Check item JSON fields and shop references, including resale-price constraints.
 */

#include "items_database.h"
#include "log.h"
#include "shop.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct ActivePlayer;
/** Stub player lookup for linking shop validation tests. */
struct ActivePlayer* player_acquire(unsigned int c) { (void)c; return 0; }
/** Stub player release for linking shop validation tests. */
void player_release(struct ActivePlayer* p) { (void)p; }
/** Stub packet send for linking shop validation tests. */
ssize_t server_send(int fd, void* d, size_t n) { (void)fd;(void)d;(void)n; return (ssize_t)n; }
/** Stub inventory synchronization for linking shop validation tests. */
void player_send_slot_updates(int fd, unsigned c, const unsigned short* s, int n) {
    (void)fd; (void)c; (void)s; (void)n;   // no client to notify in this test
}

static void write_file(const char* path, const char* body) {
    FILE* f = fopen(path, "w");
    assert(f != NULL);
    fputs(body, f);
    fclose(f);
}

/**
 * Run item loading and shop validation assertions.
 *
 * @return      Zero after all assertions pass.
 */
int main(void) {
    log_init();
    log_set_level(LOG_LEVEL_ERROR);

    const char* items_path = "/tmp/mmo_item_data_test_items.json";
    const char* shops_path = "/tmp/mmo_item_data_test_shops.json";

    printf("TEST 1: stacking, binding and value are read from the file\n");
    write_file(items_path,
        "{\"items\":["
        "{\"id\":10,\"name\":\"Feather\",\"type\":\"item\",\"max_stack\":100,\"rarity\":\"common\"},"
        "{\"id\":11,\"name\":\"Sword\",\"type\":\"weapon\",\"slot\":\"main_hand\",\"max_stack\":1,"
          "\"rarity\":\"rare\",\"value\":250,\"bind_on_equip\":true},"
        "{\"id\":12,\"name\":\"Relic\",\"type\":\"armor\",\"slot\":\"helmet\",\"max_stack\":1,"
          "\"rarity\":\"epic\",\"value\":1000,\"bind_on_pickup\":true},"
        "{\"id\":13,\"name\":\"Nameless\",\"type\":\"item\"}"
        "]}");
    assert(items_init(items_path) == 1);

    const ItemDefinition* feather = item_get(10);
    assert(feather != NULL);
    assert(feather->max_stack == 100);
    assert(feather->stackable == 1);
    printf("  stackable item: max_stack=%u stackable=%u\n",
           feather->max_stack, feather->stackable);

    const ItemDefinition* sword = item_get(11);
    assert(sword != NULL);
    assert(sword->max_stack == 1 && sword->stackable == 0);
    assert(sword->value == 250);            // explicit value wins
    assert(sword->bind_on_equip == 1 && sword->bind_on_pickup == 0);
    printf("  non-stackable: max_stack=1 stackable=0 value=%u bind_on_equip=%u\n",
           sword->value, sword->bind_on_equip);

    const ItemDefinition* relic = item_get(12);
    assert(relic != NULL && relic->bind_on_pickup == 1 && relic->value == 1000);
    printf("  bind_on_pickup honoured, value=%u\n", relic->value);

    // verify defaults for omitted stack and value fields
    const ItemDefinition* bare = item_get(13);
    assert(bare != NULL);
    assert(bare->max_stack == 1 && bare->stackable == 0);
    assert(bare->value > 0);
    printf("  item with neither field: max_stack=1, value=%u (rarity fallback)\n",
           bare->value);

    printf("\nTEST 2: a clean shop file validates\n");
    write_file(shops_path,
        "{\"shops\":[{\"shop_id\":1,\"name\":\"Good\",\"items\":["
        "{\"item_id\":11,\"buy_price\":500},"
        "{\"item_id\":12,\"buy_price\":2000}"
        "]}]}");
    assert(shop_init(shops_path) == 1);
    int problems = shop_validate();
    printf("  problems reported: %d (expect 0)\n", problems);
    assert(problems == 0);
    shop_cleanup();

    printf("\nTEST 3: a shop selling a nonexistent item is caught\n");
    write_file(shops_path,
        "{\"shops\":[{\"shop_id\":2,\"name\":\"Ghost\",\"items\":["
        "{\"item_id\":99999,\"buy_price\":50}"
        "]}]}");
    assert(shop_init(shops_path) == 1);
    problems = shop_validate();
    printf("  problems reported: %d (expect 1)\n", problems);
    assert(problems == 1);
    shop_cleanup();

    printf("\nTEST 4: a buy price at or below resale is caught\n");
    // reject buy prices that do not exceed resale value
    write_file(shops_path,
        "{\"shops\":[{\"shop_id\":3,\"name\":\"Printer\",\"items\":["
        "{\"item_id\":11,\"buy_price\":100},"      // below value 250
        "{\"item_id\":12,\"buy_price\":1000}"      // exactly value 1000
        "]}]}");
    assert(shop_init(shops_path) == 1);
    problems = shop_validate();
    printf("  problems reported: %d (expect 2 — below, and equal)\n", problems);
    assert(problems == 2);
    shop_cleanup();

    printf("\nTEST 5: one shop's keys do not bleed into the shop before it\n");
    /* The defect this pins down.
     *
     * The loader this replaced found a key with strstr() starting at the shop
     * object it was reading. strstr() does not stop at that object's closing
     * brace, so a shop that omitted a key silently picked up the *next*
     * shop's value for it. The first shop below has no "items" and no
     * "currency"; the second has both. Under the old scanner the first shop
     * came out holding the second's entire stock, priced in the second's
     * coin -- a merchant selling goods nobody put on its list, with no error
     * anywhere.
     *
     * Shop 10 must therefore be empty and trade in Ennara's coin (the
     * documented default for an absent "currency"), and shop 20 must keep its
     * own two items and its own currency. */
    write_file(shops_path,
        "{\"shops\":["
        "{\"shop_id\":10,\"name\":\"Empty\"},"
        "{\"shop_id\":20,\"name\":\"Stocked\",\"currency\":1,\"items\":["
        "{\"item_id\":11,\"buy_price\":500},"
        "{\"item_id\":12,\"buy_price\":2000}"
        "]}]}");
    assert(shop_init(shops_path) == 1);
    {
        ShopDef* empty   = shop_find(10);
        ShopDef* stocked = shop_find(20);
        assert(empty && stocked);
        printf("  shop 10 holds %u items (expect 0), currency %u (expect %u)\n",
               empty->item_count, empty->currency_id, (unsigned)CURRENCY_ENNARA);
        assert(empty->item_count == 0);
        assert(empty->currency_id == (uint8_t)CURRENCY_ENNARA);
        printf("  shop 20 holds %u items (expect 2), currency %u (expect 1)\n",
               stocked->item_count, stocked->currency_id);
        assert(stocked->item_count == 2);
        assert(stocked->currency_id == 1);
    }
    shop_cleanup();

    printf("\nTEST 6: the shipped content files are clean\n");
    assert(items_init("world_server/data/items.json") == 1);
    assert(shop_init("world_server/data/shops.json") == 1);
    problems = shop_validate();
    printf("  world_server/data problems: %d (expect 0)\n", problems);
    assert(problems == 0);

    printf("\nTEST 7: the registry grows past its initial buckets and has no id ceiling\n");
    {
        /* The registry used to be a direct-indexed array of 100,000 pointers, so
         * ids above that were dropped and the table never grew. Both properties
         * are now the opposite, and neither was covered: the tests above load
         * four items, which fits the initial bucket count with room to spare. */
        const int    kCount = 1000;
        const uint32_t kBase = 4000000000u;   // past the old MAX_ITEMS ceiling

        size_t cap = (size_t)kCount * 96 + 64;
        char* body = malloc(cap);
        assert(body != NULL);
        size_t len = (size_t)snprintf(body, cap, "{\"items\":[");

        for (int i = 0; i < kCount; i++) {
            len += (size_t)snprintf(body + len, cap - len,
                                    "%s{\"id\":%u,\"name\":\"Bulk%d\",\"type\":\"item\"}",
                                    i ? "," : "", kBase + (uint32_t)i * 7u, i);
        }
        snprintf(body + len, cap - len, "]}");

        write_file(items_path, body);
        free(body);

        assert(items_init(items_path) == 1);
        printf("  loaded %d items (initial bucket count is 256)\n", items_get_count());
        assert(items_get_count() == kCount);

        for (int i = 0; i < kCount; i++) {
            const ItemDefinition* it = item_get(kBase + (uint32_t)i * 7u);
            assert(it != NULL);
            assert(it->id == kBase + (uint32_t)i * 7u);
        }
        printf("  every id round-trips, including ids above the old 100000 cap\n");

        // Ids that were never inserted must miss rather than collide onto a neighbour.
        assert(item_get(kBase + 1u) == NULL);
        assert(item_get(0) == NULL);
        assert(item_get(0xFFFFFFFFu) == NULL);
        printf("  absent ids miss cleanly\n");
    }

    printf("\nTEST 8: a repeated id replaces rather than leaks, and is counted once\n");
    write_file(items_path,
        "{\"items\":["
        "{\"id\":42,\"name\":\"First\",\"type\":\"item\"},"
        "{\"id\":42,\"name\":\"Second\",\"type\":\"item\"}"
        "]}");
    assert(items_init(items_path) == 1);
    {
        const ItemDefinition* dup = item_get(42);
        assert(dup != NULL);
        assert(strcmp(dup->name, "Second") == 0);
        printf("  id 42 resolves to '%s' and the registry holds %d item\n",
               dup->name, items_get_count());
        assert(items_get_count() == 1);
    }

    printf("\nALL ASSERTIONS PASSED\n");
    items_cleanup();
    shop_cleanup();
    remove(items_path);
    remove(shops_path);
    return 0;
}
