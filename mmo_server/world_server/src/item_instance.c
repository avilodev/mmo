/**
 * @file
 * Allocate item-instance identifiers and mutate caller-owned inventory arrays.
 */

#include "item_instance.h"

#include <stdatomic.h>
#include <string.h>

static _Atomic uint64_t g_next_instance_id = 1;

/**
 * Seed the allocator above the highest persisted instance identifier.
 */
void item_instance_seed(uint64_t highest_existing_id) {
    atomic_store(&g_next_instance_id, highest_existing_id + 1);
}

/**
 * Allocate the next process-local item-instance identifier atomically.
 *
 * @return A monotonically increasing identifier.
 */
uint64_t item_instance_next_id(void) {
    return atomic_fetch_add(&g_next_instance_id, 1);
}

static inline uint16_t effective_stack(uint16_t max_stack) {
    return max_stack < 1 ? 1 : max_stack;
}

/** Record one touched slot for the caller, if it still fits and wants them. */
static inline void note_changed(uint16_t* changed, int max_changed, int* count,
                                int slot) {
    if (!changed || !count || *count >= max_changed) return;
    changed[(*count)++] = (uint16_t)slot;
}

/**
 * Add items by filling partial stacks before opening empty slots.
 *
 * Binding is part of stack identity here: a bound stack and an unbound one are
 * never merged, even when they hold the same item. Merging them would move
 * units across the boundary in whichever direction the destination happened to
 * sit -- laundering a bound unit into a free stack, or binding a free one by
 * accident -- and which of those happened would depend on nothing but bag
 * order. The flag is only recorded today and enforced nowhere, so this cannot
 * yet be exploited; it is written this way so that it never can be.
 *
 * @param slots           Caller-owned array of INVENTORY_SLOTS entries.
 * @param item_id         Nonzero item definition identifier.
 * @param quantity        Number of units requested.
 * @param max_stack       Per-slot capacity; zero is normalized to one.
 * @param bind_on_pickup  Nonzero to bind newly created stacks.
 * @param changed         Receives touched slot indices, or NULL.
 * @param max_changed     Capacity of `changed`.
 * @param changed_count   Receives how many were written, or NULL.
 * @return                The quantity that could not fit.
 */
uint16_t inventory_add_tracked(ItemInstance* slots, uint32_t item_id,
                               uint16_t quantity, uint16_t max_stack,
                               uint8_t bind_on_pickup,
                               uint16_t* changed, int max_changed,
                               int* changed_count) {
    int written = 0;
    if (changed_count) *changed_count = 0;
    if (!slots || item_id == 0 || quantity == 0) return quantity;

    const uint16_t cap   = effective_stack(max_stack);
    const uint8_t  bound = bind_on_pickup ? 1 : 0;

    // fill compatible partial stacks first
    if (cap > 1) {
        for (int i = 0; i < INVENTORY_SLOTS && quantity > 0; i++) {
            if (slots[i].instance_id == 0) continue;
            if (slots[i].item_id != item_id) continue;
            if ((slots[i].is_bound ? 1 : 0) != bound) continue;
            if (slots[i].quantity >= cap) continue;

            uint16_t room = (uint16_t)(cap - slots[i].quantity);
            uint16_t take = quantity < room ? quantity : room;
            slots[i].quantity = (uint16_t)(slots[i].quantity + take);
            quantity = (uint16_t)(quantity - take);
            note_changed(changed, max_changed, &written, i);
        }
    }

    // Then open new slots for whatever is left.
    for (int i = 0; i < INVENTORY_SLOTS && quantity > 0; i++) {
        if (slots[i].instance_id != 0) continue;

        uint16_t take = quantity < cap ? quantity : cap;
        slots[i].instance_id = item_instance_next_id();
        slots[i].item_id     = item_id;
        slots[i].quantity    = take;
        slots[i].is_bound    = bound;
        slots[i]._pad        = 0;
        quantity = (uint16_t)(quantity - take);
        note_changed(changed, max_changed, &written, i);
    }

    if (changed_count) *changed_count = written;
    return quantity;   // whatever would not fit
}

/**
 * Add items, discarding the report of which slots changed.
 *
 * @return The quantity that could not fit.
 */
uint16_t inventory_add(ItemInstance* slots, uint32_t item_id, uint16_t quantity,
                       uint16_t max_stack, uint8_t bind_on_pickup) {
    return inventory_add_tracked(slots, item_id, quantity, max_stack,
                                 bind_on_pickup, NULL, 0, NULL);
}

/**
 * Remove up to a requested quantity from one inventory slot.
 *
 * @return The number removed, or 0 for invalid or empty input.
 */
uint16_t inventory_remove_at(ItemInstance* slots, int slot, uint16_t quantity) {
    if (!slots || slot < 0 || slot >= INVENTORY_SLOTS) return 0;
    if (slots[slot].instance_id == 0 || quantity == 0) return 0;

    uint16_t have = slots[slot].quantity;
    uint16_t take = quantity < have ? quantity : have;

    slots[slot].quantity = (uint16_t)(have - take);
    if (slots[slot].quantity == 0)
        memset(&slots[slot], 0, sizeof(slots[slot]));   // frees the slot

    return take;
}

/**
 * Count an item type across every inventory stack.
 *
 * @return The total quantity, or 0 for invalid input or absence.
 */
uint32_t inventory_count(const ItemInstance* slots, uint32_t item_id) {
    if (!slots || item_id == 0) return 0;

    uint32_t total = 0;
    for (int i = 0; i < INVENTORY_SLOTS; i++)
        if (slots[i].instance_id != 0 && slots[i].item_id == item_id)
            total += slots[i].quantity;
    return total;
}

/**
 * Find the first empty inventory slot.
 *
 * @return The slot index, or -1 for NULL input or a full inventory.
 */
int inventory_first_free(const ItemInstance* slots) {
    if (!slots) return -1;
    for (int i = 0; i < INVENTORY_SLOTS; i++)
        if (slots[i].instance_id == 0) return i;
    return -1;
}

/**
 * Move, merge, or swap two inventory slots.
 *
 * @param max_stack  Per-slot capacity for same-item merging; zero becomes one.
 * @return           1 when slots change, or 0 for invalid input or an empty source.
 */
int inventory_move(ItemInstance* slots, int from, int to, uint16_t max_stack) {
    if (!slots) return 0;
    if (from < 0 || from >= INVENTORY_SLOTS) return 0;
    if (to   < 0 || to   >= INVENTORY_SLOTS) return 0;
    if (from == to) return 0;
    if (slots[from].instance_id == 0) return 0;

    const uint16_t cap = effective_stack(max_stack);

    /* Preserve any source remainder after a partial merge.
     *
     * Binding has to match for the same reason it does in
     * inventory_add_tracked(): a merge moves units into the destination's
     * identity, so merging across the boundary would silently bind or unbind
     * them depending on which slot was dragged onto which. Mismatched stacks
     * fall through to the swap below, which is what a player dragging one onto
     * the other would expect anyway. */
    if (cap > 1 &&
        slots[to].instance_id != 0 &&
        slots[to].item_id == slots[from].item_id &&
        (slots[to].is_bound ? 1 : 0) == (slots[from].is_bound ? 1 : 0) &&
        slots[to].quantity < cap) {

        uint16_t room = (uint16_t)(cap - slots[to].quantity);
        uint16_t take = slots[from].quantity < room ? slots[from].quantity : room;

        slots[to].quantity   = (uint16_t)(slots[to].quantity + take);
        slots[from].quantity = (uint16_t)(slots[from].quantity - take);

        // retain the destination instance identity
        if (slots[from].quantity == 0)
            memset(&slots[from], 0, sizeof(slots[from]));
        return 1;
    }

    ItemInstance tmp = slots[to];
    slots[to]   = slots[from];
    slots[from] = tmp;
    return 1;
}
