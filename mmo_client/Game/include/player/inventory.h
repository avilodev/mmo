#ifndef INVENTORY_H
#define INVENTORY_H

#include <stdint.h>
#include "core/game_types.h"

#define EQUIP_SLOT_ID_HELMET    1
#define EQUIP_SLOT_ID_CHEST     2
#define EQUIP_SLOT_ID_GLOVES    3
#define EQUIP_SLOT_ID_LEGGINGS  4
#define EQUIP_SLOT_ID_BOOTS     5
#define EQUIP_SLOT_ID_MAIN_HAND 6
#define EQUIP_SLOT_ID_OFF_HAND  7

// Item database functions
void item_db_init(void);
const ItemTemplate* item_db_get(uint32_t item_id);
const char* item_db_get_name(uint32_t item_id);

// Inventory functions
void inventory_init(InventoryState* inv, float screen_width, float screen_height);
void inventory_load_from_server(InventoryState* inv, const uint32_t* server_data);
void inventory_toggle(InventoryState* inv);
void inventory_update(InventoryState* inv, float mouse_x, float mouse_y,
                     int mouse_clicked, int mouse_down, int right_clicked);
void inventory_render(const InventoryState* inv);

// Inventory operations
int inventory_add_item(InventoryState* inv, uint32_t item_id, uint16_t quantity);
uint16_t inventory_remove_item(InventoryState* inv, int slot_index, uint16_t quantity);
void inventory_use_item(InventoryState* inv, int slot_index);
uint32_t inventory_get_item_count(const InventoryState* inv, uint32_t item_id);
int inventory_slot_is_empty(const InventoryState* inv, int slot_index);

// Check if close button was clicked
int inventory_check_close_button(const InventoryState* inv, float mouse_x, float mouse_y);

#endif // INVENTORY_H