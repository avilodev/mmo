#ifndef ITEM_DB_H
#define ITEM_DB_H

#include <stdint.h>
#include "core/game_types.h"

// Where the client reads item templates from (written by tools/sync_items.py)
#define ITEM_DB_PATH "Game/data/items.json"

// Room for the fact lines item_tooltip_lines() can produce for one item
#define ITEM_TOOLTIP_MAX_LINES 24
#define ITEM_TOOLTIP_LINE_LEN 40

// Load ITEM_DB_PATH, replacing whatever was loaded before
void item_db_init(void);

// Load an items file, replacing what was loaded; returns the item count
int item_db_load(const char* path);

// Look an item up by id; NULL when the id is not in the file
const ItemTemplate* item_db_get(uint32_t item_id);

// Display name for an id, or "Unknown Item"
const char* item_db_get_name(uint32_t item_id);

// Fill lines with what a tooltip states about an item, not its name or
// description; returns the number of lines written
int item_tooltip_lines(const ItemTemplate* item,
                       char lines[][ITEM_TOOLTIP_LINE_LEN], int max_lines);

#endif /* ITEM_DB_H */
