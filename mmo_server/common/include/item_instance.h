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

// return the quantity removed and clear exhausted stacks
uint16_t inventory_remove_at(ItemInstance* slots, int slot, uint16_t quantity);

uint32_t inventory_count(const ItemInstance* slots, uint32_t item_id);

// return -1 when every inventory slot is occupied
int inventory_first_free(const ItemInstance* slots);

// merge compatible stacks, otherwise swap, returning whether state changed
int inventory_move(ItemInstance* slots, int from, int to, uint16_t max_stack);

#endif // ITEM_INSTANCE_H
