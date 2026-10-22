#ifndef INVENTORY_SYSTEM_H
#define INVENTORY_SYSTEM_H

// Inventory rules. Network code parses a request and calls these.

#include <stdint.h>

// validate and apply an inventory-to-equipment transfer.
void inventory_system_equip(int client_fd, uint32_t character_id, uint32_t item_id, uint8_t inventory_slot, uint8_t equip_slot);

// validate and move equipped gear into the inventory.
void inventory_system_unequip(int client_fd, uint32_t character_id, uint8_t equip_slot);

// validate, consume, and apply one inventory item.
void inventory_system_use(int client_fd, uint32_t character_id, uint8_t inventory_slot);

// remove one inventory unit and create its ground item.
void inventory_system_drop(int client_fd, uint32_t character_id, uint8_t inventory_slot);

// move, merge, or swap inventory slots requested by a client.
void inventory_system_move(int client_fd, uint32_t character_id, uint8_t from_slot, uint8_t to_slot);

#endif /* INVENTORY_SYSTEM_H */
