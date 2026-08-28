#ifndef ITEM_INSTANCE_H
#define ITEM_INSTANCE_H

/** @file Define server-side item identities and in-memory inventory operations. */

#include "protocol.h"

#include <stdint.h>

#define INVENTORY_SLOTS INVENTORY_SLOT_COUNT

/** Represent one uniquely identified item stack owned by a character. */
typedef struct {
    uint64_t instance_id;   /**< Zero marks an empty slot. */
    uint32_t item_id;
    uint16_t quantity;      /**< At least one for an occupied slot. */
    uint8_t  is_bound;      /**< Prevents transfer to another character. */
    uint8_t  _pad;
} ItemInstance;

// seed once from the world's highest persisted identifier
void item_instance_seed(uint64_t highest_existing_id);

// allocate identifiers safely across threads
uint64_t item_instance_next_id(void);

// return the quantity that cannot fit in caller-owned slots
uint16_t inventory_add(ItemInstance* slots, uint32_t item_id, uint16_t quantity,
                       uint16_t max_stack, uint8_t bind_on_pickup);

/** Add items and report every slot the addition touched.
 *
 * The same operation as inventory_add(), which is written in terms of this
 * one. It exists because callers have to tell the client which slots changed,
 * and every one of them used to answer that with inventory_first_free() read
 * *before* the add -- which is the right slot only when nothing merged. A
 * pickup that topped up a stack the player was already carrying named an empty
 * slot instead, so the client redrew a slot that had not changed and left the
 * one that had. The function doing the merging is the only thing that knows
 * where the items went, so it is what says.
 *
 * @param changed        Receives touched slot indices, in the order touched;
 *                       partial stacks first, then newly opened slots. May be
 *                       NULL to ignore the report.
 * @param max_changed    Capacity of `changed`.
 * @param changed_count  Receives how many were written, capped at max_changed.
 * @return               The quantity that could not fit.
 */
uint16_t inventory_add_tracked(ItemInstance* slots, uint32_t item_id,
                               uint16_t quantity, uint16_t max_stack,
                               uint8_t bind_on_pickup,
                               uint16_t* changed, int max_changed,
                               int* changed_count);

// return the quantity removed and clear exhausted stacks
uint16_t inventory_remove_at(ItemInstance* slots, int slot, uint16_t quantity);

uint32_t inventory_count(const ItemInstance* slots, uint32_t item_id);

// return -1 when every inventory slot is occupied
int inventory_first_free(const ItemInstance* slots);

// merge compatible stacks, otherwise swap, returning whether state changed
int inventory_move(ItemInstance* slots, int from, int to, uint16_t max_stack);

#endif // ITEM_INSTANCE_H
