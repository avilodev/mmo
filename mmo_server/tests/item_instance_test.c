/**
 * @file
 * Check item-instance identity, stacking, movement, binding, and quantity conservation.
 */

#include "item_instance.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/** Define stack capacities used by the inventory fixtures. */
#define STACK_100 100
#define UNIQUE      1

static ItemInstance inv[INVENTORY_SLOTS];

static void reset(void) { memset(inv, 0, sizeof(inv)); }

static int used_slots(void) {
    int n = 0;
    for (int i = 0; i < INVENTORY_SLOTS; i++) if (inv[i].instance_id) n++;
    return n;
}

/** Assert uniqueness and completeness invariants for occupied inventory slots. */
static void check_invariants(void) {
    for (int i = 0; i < INVENTORY_SLOTS; i++) {
        if (inv[i].instance_id == 0) {
            assert(inv[i].item_id == 0 && inv[i].quantity == 0);
            continue;
        }
        assert(inv[i].quantity > 0);
        assert(inv[i].item_id != 0);
        for (int j = i + 1; j < INVENTORY_SLOTS; j++)
            if (inv[j].instance_id != 0)
                assert(inv[j].instance_id != inv[i].instance_id);
    }
}

/**
 * Run item-instance and inventory conservation assertions.
 *
 * @return      Zero after all assertions pass.
 */
int main(void) {
    item_instance_seed(0);

    printf("TEST 1: ids are unique and monotonic\n");
    uint64_t a = item_instance_next_id();
    uint64_t b = item_instance_next_id();
    assert(b > a && a > 0);
    item_instance_seed(5000);
    assert(item_instance_next_id() == 5001);
    printf("  seeding past existing ids works (%lu -> 5001)\n", (unsigned long)b);

    printf("\nTEST 2: stackables merge before opening new slots\n");
    reset();
    assert(inventory_add(inv, 10, 30, STACK_100, 0) == 0);
    assert(used_slots() == 1);
    assert(inventory_add(inv, 10, 30, STACK_100, 0) == 0);
    assert(used_slots() == 1);                 // merged, not a second slot
    assert(inventory_count(inv, 10) == 60);
    check_invariants();
    printf("  30 + 30 of a stackable occupies 1 slot, count=60\n");

    printf("\nTEST 3: overflow spills into a new slot, nothing is lost\n");
    reset();
    assert(inventory_add(inv, 10, 250, STACK_100, 0) == 0);
    assert(used_slots() == 3);                 // 100 + 100 + 50
    assert(inventory_count(inv, 10) == 250);
    check_invariants();
    printf("  250 of a 100-stack fills 3 slots, count=250\n");

    printf("\nTEST 4: unstackables take one slot each\n");
    reset();
    assert(inventory_add(inv, 20, 5, UNIQUE, 0) == 0);
    assert(used_slots() == 5);
    assert(inventory_count(inv, 20) == 5);
    check_invariants();
    printf("  5 unstackable items occupy 5 slots\n");

    // treat zero stack capacity as unstackable
    reset();
    assert(inventory_add(inv, 21, 3, 0, 0) == 0);
    assert(used_slots() == 3);
    printf("  max_stack of 0 is treated as unstackable\n");

    printf("\nTEST 5: a full bag reports the leftover instead of eating it\n");
    reset();
    // Fill every slot with a distinct unstackable item.
    for (int i = 0; i < INVENTORY_SLOTS; i++)
        assert(inventory_add(inv, 500 + (uint32_t)i, 1, UNIQUE, 0) == 0);
    assert(used_slots() == INVENTORY_SLOTS);
    assert(inventory_first_free(inv) == -1);

    uint16_t leftover = inventory_add(inv, 999, 7, UNIQUE, 0);
    assert(leftover == 7);                     // none of it fitted
    assert(inventory_count(inv, 999) == 0);
    check_invariants();
    printf("  full bag: add of 7 returns 7 leftover, stores none\n");

    // retain quantities that cannot fit
    reset();
    for (int i = 0; i < INVENTORY_SLOTS - 1; i++)
        assert(inventory_add(inv, 500 + (uint32_t)i, 1, UNIQUE, 0) == 0);
    leftover = inventory_add(inv, 777, 150, STACK_100, 0);
    assert(leftover == 50);
    assert(inventory_count(inv, 777) == 100);
    check_invariants();
    printf("  one slot left: 150 offered, 100 stored, 50 returned\n");

    printf("\nTEST 6: removal frees the slot only when the stack empties\n");
    reset();
    assert(inventory_add(inv, 10, 10, STACK_100, 0) == 0);
    assert(inventory_remove_at(inv, 0, 4) == 4);
    assert(inv[0].instance_id != 0 && inv[0].quantity == 6);
    assert(inventory_remove_at(inv, 0, 99) == 6);   // clamps to what is there
    assert(inv[0].instance_id == 0);
    assert(used_slots() == 0);
    check_invariants();
    printf("  partial removal keeps the slot, full removal frees it\n");

    assert(inventory_remove_at(inv, -1, 1) == 0);
    assert(inventory_remove_at(inv, INVENTORY_SLOTS, 1) == 0);
    assert(inventory_remove_at(inv, 5, 1) == 0);
    printf("  out-of-range and empty removals are refused\n");

    printf("\nTEST 7: moving conserves items\n");
    reset();
    assert(inventory_add(inv, 10, 60, STACK_100, 0) == 0);   // slot 0
    assert(inventory_add(inv, 11, 1,  UNIQUE, 0) == 0);      // slot 1
    uint32_t before_10 = inventory_count(inv, 10);
    uint32_t before_11 = inventory_count(inv, 11);

    assert(inventory_move(inv, 0, 1, STACK_100) == 1);       // different types: swap
    assert(inventory_count(inv, 10) == before_10);
    assert(inventory_count(inv, 11) == before_11);
    assert(inv[1].item_id == 10 && inv[0].item_id == 11);
    check_invariants();
    printf("  swap of different types conserves both counts\n");

    // consume the source after a complete merge
    reset();
    assert(inventory_add(inv, 10, 100, STACK_100, 0) == 0);  // slot 0 full
    inv[1] = inv[0]; inv[1].instance_id = item_instance_next_id(); inv[1].quantity = 30;
    inv[0].quantity = 40;
    uint32_t total_before = inventory_count(inv, 10);
    assert(inventory_move(inv, 1, 0, STACK_100) == 1);
    assert(inventory_count(inv, 10) == total_before);        // 70, nothing created
    assert(inv[1].instance_id == 0);                         // source consumed
    check_invariants();
    printf("  merge into a partial stack conserves the total (%u)\n", total_before);

    // retain the source remainder after a partial merge
    reset();
    assert(inventory_add(inv, 10, 90, STACK_100, 0) == 0);   // slot 0: 90
    inv[1].instance_id = item_instance_next_id();
    inv[1].item_id = 10; inv[1].quantity = 50;               // slot 1: 50
    total_before = inventory_count(inv, 10);                 // 140
    assert(inventory_move(inv, 1, 0, STACK_100) == 1);
    assert(inv[0].quantity == 100);
    assert(inv[1].quantity == 40);                           // remainder survives
    assert(inventory_count(inv, 10) == total_before);
    check_invariants();
    printf("  partial merge leaves the remainder behind (100 + 40 = %u)\n", total_before);

    printf("\nTEST 8: binding is recorded on pickup\n");
    reset();
    assert(inventory_add(inv, 30, 1, UNIQUE, 1) == 0);
    assert(inv[0].is_bound == 1);
    assert(inventory_add(inv, 31, 1, UNIQUE, 0) == 0);
    assert(inv[1].is_bound == 0);
    printf("  bind_on_pickup sets the flag, absence leaves it clear\n");

    printf("\nALL ASSERTIONS PASSED\n");
    return 0;
}
