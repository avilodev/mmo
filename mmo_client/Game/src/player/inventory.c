/**
 * @file
 * Load client item metadata and manage inventory interaction and rendering.
 */

#include "inventory.h"
#include "network.h"
#include "renderer.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

/** Maximum number of item templates retained by the client. */
#define MAX_ITEM_TEMPLATES 1000
static ItemTemplate g_item_db[MAX_ITEM_TEMPLATES];
static int g_item_db_count = 0;

// Forward declarations
static char* read_file(const char* filepath);
static const char* find_json_value(const char* json, const char* key);
static EquipSlot parse_slot(const char* slot_str);
static ItemType parse_type(const char* type_str);
static ItemRarity parse_rarity(const char* rarity_str);
static int parse_items_json(const char* json_content);

/**
 * Reload the client item-template database from its JSON data file.
 */
void item_db_init(void) {
    g_item_db_count = 0;
    memset(g_item_db, 0, sizeof(g_item_db));
    
    printf("[ITEM_DB] Loading items from: Game/Data/items.json\n");
    
    // Read JSON file
    char* json_content = read_file("Game/Data/items.json"); 
    if (!json_content) {
        fprintf(stderr, "[ITEM_DB] ERROR: Could not read Game/Data/items.json\n");
        return;
    }
    
    // Parse JSON
    int result = parse_items_json(json_content);
    free(json_content);
    
    if (result) {
        printf("[ITEM_DB] Successfully loaded %d items\n", g_item_db_count);
    } else {
        fprintf(stderr, "[ITEM_DB] Failed to parse items JSON\n");
    }
}

/**
 * Find an item template by its protocol identifier.
 *
 * The returned pointer refers to static database storage and remains valid until item_db_init is called again.
 *
 * @return      Matching template, or NULL when the identifier is unknown.
 */
const ItemTemplate* item_db_get(uint32_t item_id) {
    for (int i = 0; i < g_item_db_count; i++) {
        if (g_item_db[i].id == item_id) {
            return &g_item_db[i];
        }
    }
    return NULL;
}

/**
 * Return the display name for an item identifier.
 *
 * @return      Static template name, or "Unknown Item" when no template matches.
 */
const char* item_db_get_name(uint32_t item_id) {
    const ItemTemplate* item = item_db_get(item_id);
    return item ? item->name : "Unknown Item";
}

/**
 * Read an entire file into a NUL-terminated allocation.
 *
 * The caller must free the returned buffer.
 *
 * @return      Allocated file contents, or NULL on open or allocation failure.
 */
static char* read_file(const char* filepath) {
    FILE* f = fopen(filepath, "rb");
    if (!f) return NULL;
    
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    char* buffer = (char*)malloc(size + 1);
    if (!buffer) {
        fclose(f);
        return NULL;
    }
    
    size_t read_size = fread(buffer, 1, size, f);
    buffer[read_size] = '\0';
    fclose(f);
    
    return buffer;
}

/**
 * Locate the first value token following a JSON key.
 *
 * @return      Pointer into json at the first non-space value byte, or NULL when absent.
 */
static const char* find_json_value(const char* json, const char* key) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    
    const char* pos = strstr(json, search);
    if (!pos) return NULL;
    
    pos = strchr(pos, ':');
    if (!pos) return NULL;
    pos++;
    
    while (*pos && isspace((unsigned char)*pos)) pos++;
    
    return pos;
}

static EquipSlot parse_slot(const char* slot_str) {
    if (strcmp(slot_str, "helmet") == 0) return EQUIP_SLOT_HELMET;
    if (strcmp(slot_str, "gloves") == 0) return EQUIP_SLOT_GLOVES;
    if (strcmp(slot_str, "chest") == 0) return EQUIP_SLOT_CHEST;
    if (strcmp(slot_str, "leggings") == 0) return EQUIP_SLOT_LEGGINGS;
    if (strcmp(slot_str, "boots") == 0) return EQUIP_SLOT_BOOTS;
    if (strcmp(slot_str, "main_hand") == 0) return EQUIP_SLOT_MAIN_HAND;
    if (strcmp(slot_str, "off_hand") == 0) return EQUIP_SLOT_SECOND_HAND;
    if (strcmp(slot_str, "two_handed") == 0) return EQUIP_SLOT_MAIN_HAND;
    return EQUIP_SLOT_NONE;
}

static ItemType parse_type(const char* type_str) {
    if (strcmp(type_str, "consumable") == 0) return ITEM_TYPE_CONSUMABLE;
    if (strcmp(type_str, "weapon") == 0) return ITEM_TYPE_EQUIPMENT;
    if (strcmp(type_str, "shield") == 0) return ITEM_TYPE_EQUIPMENT;
    if (strcmp(type_str, "armor") == 0) return ITEM_TYPE_EQUIPMENT;
    if (strcmp(type_str, "item") == 0) return ITEM_TYPE_MATERIAL;
    if (strcmp(type_str, "quest") == 0) return ITEM_TYPE_QUEST;
    return ITEM_TYPE_MATERIAL;
}

static ItemRarity parse_rarity(const char* rarity_str) {
    if (strcmp(rarity_str, "common") == 0) return ITEM_RARITY_COMMON;
    if (strcmp(rarity_str, "uncommon") == 0) return ITEM_RARITY_UNCOMMON;
    if (strcmp(rarity_str, "rare") == 0) return ITEM_RARITY_RARE;
    if (strcmp(rarity_str, "epic") == 0) return ITEM_RARITY_EPIC;
    if (strcmp(rarity_str, "legendary") == 0) return ITEM_RARITY_LEGENDARY;
    return ITEM_RARITY_COMMON;
}

/**
 * Parse item objects into the static template database.
 *
 * @return      Nonzero when the items array is found and processed; otherwise zero.
 */
static int parse_items_json(const char* json_content) {
    const char* items_start = strstr(json_content, "\"items\"");
    if (!items_start) {
        fprintf(stderr, "[ITEM_DB] No 'items' array found in JSON\n");
        return 0;
    }
    
    const char* array_start = strchr(items_start, '[');
    if (!array_start) return 0;
    
    const char* pos = array_start + 1;
    
    while (*pos) {
        while (*pos && isspace((unsigned char)*pos)) pos++;
        
        if (*pos == ']') break;
        if (*pos != '{') {
            pos++;
            continue;
        }
        
        const char* obj_start = pos;
        int brace_count = 0;
        const char* obj_end = pos;
        
        while (*obj_end) {
            if (*obj_end == '{') brace_count++;
            if (*obj_end == '}') {
                brace_count--;
                if (brace_count == 0) break;
            }
            obj_end++;
        }
        
        if (brace_count != 0) break;
        
        size_t obj_len = obj_end - obj_start + 1;
        char* obj_json = (char*)malloc(obj_len + 1);
        if (!obj_json) {
            fprintf(stderr, "[ITEM_DB] Out of memory parsing items\n");
            break;
        }
        memcpy(obj_json, obj_start, obj_len);
        obj_json[obj_len] = '\0';
        
        if (g_item_db_count >= MAX_ITEM_TEMPLATES) {
            fprintf(stderr, "[ITEM_DB] Max items reached (%d)\n", MAX_ITEM_TEMPLATES);
            free(obj_json);
            break;
        }
        
        ItemTemplate* item = &g_item_db[g_item_db_count];
        memset(item, 0, sizeof(ItemTemplate));
        
        const char* id_val = find_json_value(obj_json, "id");
        if (id_val) item->id = (uint32_t)atoi(id_val);
        
        const char* name_val = find_json_value(obj_json, "name");
        if (name_val && *name_val == '"') {
            const char* name_end = strchr(name_val + 1, '"');
            if (name_end) {
                size_t name_len = name_end - (name_val + 1);
                if (name_len >= 32) name_len = 31;
                memcpy(item->name, name_val + 1, name_len);
                item->name[name_len] = '\0';
            }
        }
        
        const char* desc_val = find_json_value(obj_json, "description");
        if (desc_val && *desc_val == '"') {
            const char* desc_end = strchr(desc_val + 1, '"');
            if (desc_end) {
                size_t desc_len = desc_end - (desc_val + 1);
                if (desc_len >= 128) desc_len = 127;
                memcpy(item->description, desc_val + 1, desc_len);
                item->description[desc_len] = '\0';
            }
        }
        
        const char* type_val = find_json_value(obj_json, "type");
        if (type_val && *type_val == '"') {
            const char* type_end = strchr(type_val + 1, '"');
            if (type_end) {
                size_t type_len = type_end - (type_val + 1);
                char type_str[32] = {0};
                if (type_len < 32) {
                    memcpy(type_str, type_val + 1, type_len);
                    item->type = parse_type(type_str);
                }
            }
        }
        
        const char* slot_val = find_json_value(obj_json, "slot");
        if (slot_val && *slot_val == '"') {
            const char* slot_end = strchr(slot_val + 1, '"');
            if (slot_end) {
                size_t slot_len = slot_end - (slot_val + 1);
                char slot_str[32] = {0};
                if (slot_len < 32) {
                    memcpy(slot_str, slot_val + 1, slot_len);
                    item->equip_slot = parse_slot(slot_str);
                }
            }
        }
        
        const char* stack_val = find_json_value(obj_json, "max_stack");
        if (stack_val) {
            item->max_stack = (uint16_t)atoi(stack_val);
        } else {
            item->max_stack = 1;
        }
        
        const char* dmg_val = find_json_value(obj_json, "damage");
        if (dmg_val) item->damage = (int16_t)atoi(dmg_val);
        
        const char* def_val = find_json_value(obj_json, "defense");
        if (def_val) item->defense = (int16_t)atoi(def_val);
        
        const char* effects_val = find_json_value(obj_json, "effects");
        if (effects_val) item->hp_restore = (int16_t)atoi(effects_val);
        
        const char* rarity_val = find_json_value(obj_json, "rarity");
        if (rarity_val && *rarity_val == '"') {
            const char* rarity_end = strchr(rarity_val + 1, '"');
            if (rarity_end) {
                size_t rarity_len = rarity_end - (rarity_val + 1);
                char rarity_str[32] = {0};
                if (rarity_len < 32) {
                    memcpy(rarity_str, rarity_val + 1, rarity_len);
                    item->rarity = parse_rarity(rarity_str);
                }
            }
        }
        
        item->sprite_id = (uint16_t)item->id;
        item->vendor_price = (item->rarity + 1) * 10;
        
        g_item_db_count++;
        printf("[ITEM_DB]   Loaded: [%u] %s\n", item->id, item->name);
        
        free(obj_json);
        
        pos = obj_end + 1;
        while (*pos && *pos != ',' && *pos != ']') pos++;
        if (*pos == ',') pos++;
    }
    
    return 1;
}

/**
 * Initialize inventory layout and interaction state.
 *
 * @param screen_width  Logical screen width in pixels.
 * @param screen_height  Logical screen height in pixels.
 */
void inventory_init(InventoryState* inv, float screen_width, float screen_height) {
    memset(inv, 0, sizeof(InventoryState));
    
    inv->slot_size = 40.0f;
    inv->slot_padding = 4.0f;
    
    inv->window_width = INVENTORY_COLS * (inv->slot_size + inv->slot_padding) + inv->slot_padding * 2;
    inv->window_height = INVENTORY_ROWS * (inv->slot_size + inv->slot_padding) + inv->slot_padding * 2 + 40;
    
    inv->window_x = (screen_width - inv->window_width) / 2.0f;
    inv->window_y = (screen_height - inv->window_height) / 2.0f;
    
    inv->screen_width = screen_width;
    inv->screen_height = screen_height;
    
    inv->hovered_slot = -1;
    inv->selected_slot = -1;
    inv->is_open = 0;
    inv->is_dragging_window = 0;
    
    // Close button (top right corner)
    inv->close_button_size = 25.0f;
    inv->close_button_x = inv->window_x + inv->window_width - inv->close_button_size - 5.0f;
    inv->close_button_y = inv->window_y + 5.0f;
    
    printf("[INVENTORY] Initialized %dx%d grid\n", INVENTORY_COLS, INVENTORY_ROWS);
}

/**
 * Replace local inventory slots with server-provided slot records.
 *
 * The input records must already be converted to host byte order.
 *
 * @param server_data  Array containing at least INVENTORY_SLOT_COUNT records.
 */
void inventory_load_from_server(InventoryState* inv, const InventorySlotData* server_data) {
    for (int i = 0; i < INVENTORY_SIZE && i < INVENTORY_SLOT_COUNT; i++) {
        const InventorySlotData* src = &server_data[i];

        if (src->instance_id == 0) {
            inv->slots[i].template_id = 0;
            inv->slots[i].quantity    = 0;
            inv->slots[i].instance_id = 0;
            inv->slots[i].is_bound    = 0;
            continue;
        }

        inv->slots[i].instance_id = src->instance_id;
        inv->slots[i].template_id = src->item_id;
        inv->slots[i].is_bound    = src->is_bound;

        // Never store 0 alongside a live instance: a slot with an id and no
        // quantity would render as an empty square that still refuses a drop.
        inv->slots[i].quantity = src->quantity ? src->quantity : 1;
    }

    printf("[INVENTORY] Loaded from server data\n");
}

/**
 * Toggle inventory visibility and clear interaction state when closing.
 */
void inventory_toggle(InventoryState* inv) {
    inv->is_open = !inv->is_open;
    if (!inv->is_open) {
        inv->selected_slot = -1;
        inv->hovered_slot = -1;
        inv->tooltip_visible = 0;
        inv->is_dragging_window = 0;
    }
}

/**
 * Resolve a mouse position to an inventory slot.
 *
 * @return      Slot index, or -1 outside an open inventory slot.
 */
static int get_slot_at_position(const InventoryState* inv, float mouse_x, float mouse_y) {
    if (!inv->is_open) return -1;
    
    float content_x = inv->window_x + inv->slot_padding;
    float content_y = inv->window_y + 40;
    
    float rel_x = mouse_x - content_x;
    float rel_y = mouse_y - content_y;
    
    if (rel_x < 0 || rel_y < 0) return -1;
    
    float slot_total = inv->slot_size + inv->slot_padding;
    int col = (int)(rel_x / slot_total);
    int row = (int)(rel_y / slot_total);
    
    if (col < 0 || col >= INVENTORY_COLS) return -1;
    if (row < 0 || row >= INVENTORY_ROWS) return -1;
    
    float slot_x = col * slot_total;
    float slot_y = row * slot_total;
    
    if (rel_x < slot_x || rel_x > slot_x + inv->slot_size) return -1;
    if (rel_y < slot_y || rel_y > slot_y + inv->slot_size) return -1;
    
    return row * INVENTORY_COLS + col;
}

static int is_mouse_in_title_bar(const InventoryState* inv, float mouse_x, float mouse_y) {
    return (mouse_x >= inv->window_x && 
            mouse_x <= inv->window_x + inv->window_width &&
            mouse_y >= inv->window_y && 
            mouse_y <= inv->window_y + 35);
}

/**
 * Check whether a mouse position hits the visible close button.
 *
 * @return      Nonzero on a hit; otherwise zero.
 */
int inventory_check_close_button(const InventoryState* inv, float mouse_x, float mouse_y) {
    if (!inv->is_open) return 0;
    
    float x = inv->close_button_x;
    float y = inv->close_button_y;
    float size = inv->close_button_size;
    
    return (mouse_x >= x && mouse_x <= x + size &&
            mouse_y >= y && mouse_y <= y + size);
}

/**
 * Update inventory dragging, selection, tooltips, and item activation.
 *
 * @param mouse_clicked  Nonzero on a new left-button press.
 * @param mouse_down  Nonzero while the left button is held.
 * @param right_clicked  Nonzero on a new right-button press.
 */
void inventory_update(InventoryState* inv, float mouse_x, float mouse_y,
                     int mouse_clicked, int mouse_down, int right_clicked) {
    if (!inv->is_open) {
        inv->hovered_slot = -1;
        inv->tooltip_visible = 0;
        inv->is_dragging_window = 0;
        return;
    }
    
    // Always update mouse position for dragged item rendering
    inv->tooltip_x = mouse_x;
    inv->tooltip_y = mouse_y;
    
    // Handle close button click FIRST (before drag check)
    if (mouse_clicked && inventory_check_close_button(inv, mouse_x, mouse_y)) {
        inventory_toggle(inv);
        return;
    }
    
    // Handle window dragging using mouse_down (held state)
    if (mouse_down) {
        if (!inv->is_dragging_window) {
            // Check if we just clicked on the title bar (and NOT the close button)
            if (mouse_clicked && is_mouse_in_title_bar(inv, mouse_x, mouse_y) 
                && !inventory_check_close_button(inv, mouse_x, mouse_y)) {
                // Start dragging
                inv->is_dragging_window = 1;
                inv->drag_offset_x = mouse_x - inv->window_x;
                inv->drag_offset_y = mouse_y - inv->window_y;
                printf("[INV] Started dragging window\n");
            }
        } else {
            // Continue dragging
            inv->window_x = mouse_x - inv->drag_offset_x;
            inv->window_y = mouse_y - inv->drag_offset_y;
            
            // Clamp to screen bounds
            if (inv->window_x < 0) inv->window_x = 0;
            if (inv->window_y < 0) inv->window_y = 0;
            if (inv->window_x + inv->window_width > inv->screen_width) {
                inv->window_x = inv->screen_width - inv->window_width;
            }
            if (inv->window_y + inv->window_height > inv->screen_height) {
                inv->window_y = inv->screen_height - inv->window_height;
            }
            
            // Update close button position when dragging
            inv->close_button_x = inv->window_x + inv->window_width - inv->close_button_size - 5.0f;
            inv->close_button_y = inv->window_y + 5.0f;
            
            // Don't process slots while dragging window
            inv->hovered_slot = -1;
            inv->tooltip_visible = 0;
            return;
        }
    } else {
        // Mouse released - stop dragging
        if (inv->is_dragging_window) {
            printf("[INV] Stopped dragging window\n");
        }
        inv->is_dragging_window = 0;
    }
    
    // Get slot under mouse
    int slot = get_slot_at_position(inv, mouse_x, mouse_y);
    inv->hovered_slot = slot;
    
    // Show tooltip only if not dragging an item
    if (slot >= 0 && slot < INVENTORY_SIZE && inv->slots[slot].template_id > 0 && inv->selected_slot == -1) {
        inv->tooltip_visible = 1;
    } else {
        inv->tooltip_visible = 0;
    }
    
    // Handle item clicks - only process if NOT dragging window
    if (mouse_clicked && !inv->is_dragging_window) {
        if (slot >= 0 && slot < INVENTORY_SIZE) {
            if (inv->selected_slot == -1) {
                // Pick up item
                if (inv->slots[slot].template_id > 0) {
                    inv->selected_slot = slot;
                    printf("[INV] Picked up item from slot %d\n", slot);
                }
            } else {
                // Place item
                if (slot == inv->selected_slot) {
                    inv->selected_slot = -1;
                    printf("[INV] Put item back in same slot\n");
                } else {
                    ItemSlot temp = inv->slots[slot];
                    inv->slots[slot] = inv->slots[inv->selected_slot];
                    inv->slots[inv->selected_slot] = temp;
                    network_send_move_item((uint8_t)inv->selected_slot, (uint8_t)slot);
                    inv->selected_slot = -1;
                    printf("[INV] Swapped items\n");
                }
            }
        } else {
            // Clicked outside inventory - drop item
            inv->selected_slot = -1;
        }
    }
    
    // Handle right-click to use item
    if (right_clicked && slot >= 0 && slot < INVENTORY_SIZE) {
        if (inv->slots[slot].template_id > 0) {
            inventory_use_item(inv, slot);
        }
    }
}

static void get_rarity_color(ItemRarity rarity, float* r, float* g, float* b) {
    switch (rarity) {
        case ITEM_RARITY_COMMON: *r=0.8f; *g=0.8f; *b=0.8f; break;
        case ITEM_RARITY_UNCOMMON: *r=0.2f; *g=1.0f; *b=0.2f; break;
        case ITEM_RARITY_RARE: *r=0.3f; *g=0.5f; *b=1.0f; break;
        case ITEM_RARITY_EPIC: *r=0.8f; *g=0.3f; *b=1.0f; break;
        case ITEM_RARITY_LEGENDARY: *r=1.0f; *g=0.6f; *b=0.0f; break;
        default: *r=0.5f; *g=0.5f; *b=0.5f;
    }
}

/**
 * Draw the inventory frame, title, and close button.
 */
static void render_inventory_window(const InventoryState* inv) {
    renderer_draw_rect(inv->window_x, inv->window_y, inv->window_width, inv->window_height, 0.1f, 0.1f, 0.15f, 0.95f);
    renderer_draw_rect(inv->window_x, inv->window_y, inv->window_width, 35, 0.15f, 0.1f, 0.2f, 1.0f);
    float b = 2.0f;
    renderer_draw_rect(inv->window_x, inv->window_y, inv->window_width, b, 0.6f, 0.6f, 0.6f, 1.0f);
    renderer_draw_rect(inv->window_x, inv->window_y + inv->window_height - b, inv->window_width, b, 0.6f, 0.6f, 0.6f, 1.0f);
    renderer_draw_rect(inv->window_x, inv->window_y, b, inv->window_height, 0.6f, 0.6f, 0.6f, 1.0f);
    renderer_draw_rect(inv->window_x + inv->window_width - b, inv->window_y, b, inv->window_height, 0.6f, 0.6f, 0.6f, 1.0f);
    renderer_draw_text(inv->window_x + 10, inv->window_y + 20, "Inventory");
    
    // Close button (X)
    float close_x = inv->close_button_x;
    float close_y = inv->close_button_y;
    float close_size = inv->close_button_size;
    
    renderer_draw_rect(close_x, close_y, close_size, close_size, 0.3f, 0.1f, 0.1f, 1.0f);
    renderer_draw_rect(close_x, close_y, close_size, 1, 0.6f, 0.2f, 0.2f, 1.0f);
    renderer_draw_rect(close_x, close_y + close_size - 1, close_size, 1, 0.6f, 0.2f, 0.2f, 1.0f);
    renderer_draw_rect(close_x, close_y, 1, close_size, 0.6f, 0.2f, 0.2f, 1.0f);
    renderer_draw_rect(close_x + close_size - 1, close_y, 1, close_size, 0.6f, 0.2f, 0.2f, 1.0f);
    renderer_draw_text(close_x + 7, close_y + 18, "X");
}

/**
 * Draw visible inventory slots and their item representations.
 */
static void render_inventory_slots(const InventoryState* inv) {
    float cx = inv->window_x + inv->slot_padding;
    float cy = inv->window_y + 40;
    float st = inv->slot_size + inv->slot_padding;
    
    for (int i = 0; i < INVENTORY_SIZE; i++) {
        // Skip rendering the selected slot normally (it will be rendered following mouse)
        if (i == inv->selected_slot) continue;
        
        int row = i / INVENTORY_COLS;
        int col = i % INVENTORY_COLS;
        float x = cx + col * st;
        float y = cy + row * st;
        float r=0.15f, g=0.15f, b=0.2f;
        
        if (i == inv->hovered_slot) { r+=0.1f; g+=0.1f; b+=0.1f; }
        
        renderer_draw_rect(x, y, inv->slot_size, inv->slot_size, r, g, b, 1.0f);
        renderer_draw_rect(x, y, inv->slot_size, 1, 0.4f, 0.4f, 0.4f, 1.0f);
        renderer_draw_rect(x, y + inv->slot_size - 1, inv->slot_size, 1, 0.4f, 0.4f, 0.4f, 1.0f);
        renderer_draw_rect(x, y, 1, inv->slot_size, 0.4f, 0.4f, 0.4f, 1.0f);
        renderer_draw_rect(x + inv->slot_size - 1, y, 1, inv->slot_size, 0.4f, 0.4f, 0.4f, 1.0f);
        
        if (inv->slots[i].template_id > 0) {
            const ItemTemplate* item = item_db_get(inv->slots[i].template_id);
            if (item) {
                float rr, gg, bb;
                get_rarity_color(item->rarity, &rr, &gg, &bb);
                float bs = 2.0f;
                renderer_draw_rect(x+bs, y+bs, inv->slot_size-bs*2, 1, rr, gg, bb, 1.0f);
                renderer_draw_rect(x+bs, y+inv->slot_size-bs-1, inv->slot_size-bs*2, 1, rr, gg, bb, 1.0f);
                renderer_draw_rect(x+bs, y+bs, 1, inv->slot_size-bs*2, rr, gg, bb, 1.0f);
                renderer_draw_rect(x+inv->slot_size-bs-1, y+bs, 1, inv->slot_size-bs*2, rr, gg, bb, 1.0f);
                
                float ir=0.5f, ig=0.5f, ib=0.5f;
                if (item->type == ITEM_TYPE_CONSUMABLE) { ir=0.3f; ig=0.8f; ib=0.3f; }
                else if (item->type == ITEM_TYPE_EQUIPMENT) { ir=0.7f; ig=0.7f; ib=0.8f; }
                else if (item->type == ITEM_TYPE_QUEST) { ir=1.0f; ig=0.8f; ib=0.2f; }
                
                float ip = 8.0f;
                renderer_draw_rect(x+ip, y+ip, inv->slot_size-ip*2, inv->slot_size-ip*2, ir, ig, ib, 0.8f);
                
                if (item->max_stack > 1 && inv->slots[i].quantity > 1) {
                    char qt[8];
                    snprintf(qt, 8, "%u", inv->slots[i].quantity);
                    renderer_draw_text(x + 3, y + inv->slot_size - 7, qt);
                }
            }
        }
    }
}

/**
 * Draw the selected item at the current mouse position.
 */
static void render_dragged_item(const InventoryState* inv) {
    if (inv->selected_slot < 0 || inv->selected_slot >= INVENTORY_SIZE) return;
    
    const ItemSlot* slot = &inv->slots[inv->selected_slot];
    if (slot->template_id == 0) return;
    
    const ItemTemplate* item = item_db_get(slot->template_id);
    if (!item) return;
    
    // Center item on mouse
    float x = inv->tooltip_x - inv->slot_size / 2.0f;
    float y = inv->tooltip_y - inv->slot_size / 2.0f;
    
    // Semi-transparent background
    renderer_draw_rect(x, y, inv->slot_size, inv->slot_size, 0.15f, 0.15f, 0.2f, 0.7f);
    
    // Rarity border
    float r, g, b;
    get_rarity_color(item->rarity, &r, &g, &b);
    float bs = 2.0f;
    renderer_draw_rect(x+bs, y+bs, inv->slot_size-bs*2, 1, r, g, b, 0.9f);
    renderer_draw_rect(x+bs, y+inv->slot_size-bs-1, inv->slot_size-bs*2, 1, r, g, b, 0.9f);
    renderer_draw_rect(x+bs, y+bs, 1, inv->slot_size-bs*2, r, g, b, 0.9f);
    renderer_draw_rect(x+inv->slot_size-bs-1, y+bs, 1, inv->slot_size-bs*2, r, g, b, 0.9f);
    
    // Item icon
    float ir=0.5f, ig=0.5f, ib=0.5f;
    if (item->type == ITEM_TYPE_CONSUMABLE) { ir=0.3f; ig=0.8f; ib=0.3f; }
    else if (item->type == ITEM_TYPE_EQUIPMENT) { ir=0.7f; ig=0.7f; ib=0.8f; }
    else if (item->type == ITEM_TYPE_QUEST) { ir=1.0f; ig=0.8f; ib=0.2f; }
    
    float ip = 8.0f;
    renderer_draw_rect(x+ip, y+ip, inv->slot_size-ip*2, inv->slot_size-ip*2, ir, ig, ib, 0.7f);
    
    // Quantity
    if (item->max_stack > 1 && slot->quantity > 1) {
        char qt[8];
        snprintf(qt, 8, "%u", slot->quantity);
        renderer_draw_text(x + 3, y + inv->slot_size - 7, qt);
    }
}

/**
 * Draw the hovered item's bounded tooltip.
 */
static void render_tooltip(const InventoryState* inv) {
    if (!inv->tooltip_visible || inv->hovered_slot < 0) return;
    const ItemSlot* slot = &inv->slots[inv->hovered_slot];
    if (slot->template_id == 0) return;
    const ItemTemplate* item = item_db_get(slot->template_id);
    if (!item) return;
    
    float w = 220.0f, h = 120.0f, x = inv->tooltip_x, y = inv->tooltip_y;
    if (x + w > inv->screen_width) x = inv->screen_width - w - 10;
    if (y + h > inv->screen_height) y = inv->screen_height - h - 10;
    
    renderer_draw_rect(x, y, w, h, 0.05f, 0.05f, 0.1f, 0.95f);
    float r, g, b;
    get_rarity_color(item->rarity, &r, &g, &b);
    renderer_draw_rect(x, y, w, 2, r, g, b, 1.0f);
    renderer_draw_rect(x, y+h-2, w, 2, r, g, b, 1.0f);
    renderer_draw_rect(x, y, 2, h, r, g, b, 1.0f);
    renderer_draw_rect(x+w-2, y, 2, h, r, g, b, 1.0f);
    
    renderer_draw_text(x + 5, y + 15, item->name);
    renderer_draw_text(x + 5, y + 50, item->description);
    
    if (item->damage > 0) { 
        char s[32]; 
        snprintf(s, 32, "+%d Damage", item->damage); 
        renderer_draw_text(x+5, y+70, s); 
    }
    if (item->defense > 0) { 
        char s[32]; 
        snprintf(s, 32, "+%d Defense", item->defense); 
        renderer_draw_text(x+5, y+85, s); 
    }
    if (item->hp_restore > 0) { 
        char s[32]; 
        snprintf(s, 32, "Restores %d HP", item->hp_restore); 
        renderer_draw_text(x+5, y+70, s); 
    }
}

/**
 * Render an open inventory in screen space.
 *
 * The function preserves the current OpenGL projection and model-view matrices.
 */
void inventory_render(const InventoryState* inv) {
    if (!inv->is_open) return;
    glMatrixMode(GL_PROJECTION); 
    glPushMatrix(); 
    glLoadIdentity();
    glOrtho(0, inv->screen_width, inv->screen_height, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW); 
    glPushMatrix(); 
    glLoadIdentity();
    
    render_inventory_window(inv);
    render_inventory_slots(inv);
    render_tooltip(inv);
    
    // Render dragged item on top of everything
    render_dragged_item(inv);
    
    glMatrixMode(GL_PROJECTION); 
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW); 
    glPopMatrix();
}

/**
 * Add a quantity across compatible stacks and empty slots.
 *
 * @return      Nonzero when the entire quantity is stored; otherwise zero.
 */
int inventory_add_item(InventoryState* inv, uint32_t item_id, uint16_t quantity) {
    const ItemTemplate* item = item_db_get(item_id);
    if (!item) return 0;
    
    if (item->max_stack > 1) {
        for (int i = 0; i < INVENTORY_SIZE; i++) {
            if (inv->slots[i].template_id == item_id) {
                uint16_t space = item->max_stack - inv->slots[i].quantity;
                if (space > 0) {
                    uint16_t add = (quantity < space) ? quantity : space;
                    inv->slots[i].quantity += add;
                    quantity -= add;
                    if (quantity == 0) return 1;
                }
            }
        }
    }
    
    while (quantity > 0) {
        int es = -1;
        for (int i = 0; i < INVENTORY_SIZE; i++) { 
            if (inv->slots[i].template_id == 0) { 
                es = i; 
                break; 
            } 
        }
        if (es == -1) return 0;
        uint16_t add = (quantity < item->max_stack) ? quantity : item->max_stack;
        inv->slots[es].template_id = item_id;
        inv->slots[es].quantity = add;
        quantity -= add;
    }
    return 1;
}

/**
 * Remove up to a requested quantity from one slot.
 *
 * @param si  Slot index, which may be outside the inventory range.
 * @param q  Maximum quantity to remove.
 * @return      Quantity actually removed.
 */
uint16_t inventory_remove_item(InventoryState* inv, int si, uint16_t q) {
    if (si < 0 || si >= INVENTORY_SIZE) return 0;
    ItemSlot* s = &inv->slots[si];
    if (s->template_id == 0) return 0;
    uint16_t r = (q < s->quantity) ? q : s->quantity;
    s->quantity -= r;
    if (s->quantity == 0) s->template_id = 0;
    return r;
}

/**
 * Request use or equipment of the item in a slot.
 *
 * @param si  Slot index, which may be outside the inventory range.
 */
void inventory_use_item(InventoryState* inv, int si) {
    if (si < 0 || si >= INVENTORY_SIZE) return;
    ItemSlot* s = &inv->slots[si];
    if (s->template_id == 0) return;
    const ItemTemplate* item = item_db_get(s->template_id);
    if (!item) return;
    if (item->type == ITEM_TYPE_CONSUMABLE) {
        printf("[INVENTORY] Using %s (sending to server)\n", item->name);
        network_send_use_item((uint8_t)si);
        // Server will confirm via USE_ITEM_RESPONSE; removal handled there
    } else if (item->type == ITEM_TYPE_EQUIPMENT && item->equip_slot != EQUIP_SLOT_NONE) {
        // Map client EquipSlot to server EquipSlotId
        uint8_t server_slot = 0;
        switch (item->equip_slot) {
            case EQUIP_SLOT_HELMET:      server_slot = EQUIP_SLOT_ID_HELMET; break;
            case EQUIP_SLOT_CHEST:       server_slot = EQUIP_SLOT_ID_CHEST; break;
            case EQUIP_SLOT_GLOVES:      server_slot = EQUIP_SLOT_ID_GLOVES; break;
            case EQUIP_SLOT_LEGGINGS:    server_slot = EQUIP_SLOT_ID_LEGGINGS; break;
            case EQUIP_SLOT_BOOTS:       server_slot = EQUIP_SLOT_ID_BOOTS; break;
            case EQUIP_SLOT_MAIN_HAND:   server_slot = EQUIP_SLOT_ID_MAIN_HAND; break;
            case EQUIP_SLOT_SECOND_HAND: server_slot = EQUIP_SLOT_ID_OFF_HAND; break;
            default: break;
        }
        if (server_slot > 0) {
            printf("[INVENTORY] Equipping %s to slot %u\n", item->name, server_slot);
            network_send_equip_item(s->template_id, (uint8_t)si, server_slot);
        }
    }
}

/**
 * Sum the local quantity of an item across all slots.
 *
 * @return      Total recorded quantity.
 */
uint32_t inventory_get_item_count(const InventoryState* inv, uint32_t item_id) {
    uint32_t c = 0;
    for (int i = 0; i < INVENTORY_SIZE; i++) {
        if (inv->slots[i].template_id == item_id) c += inv->slots[i].quantity;
    }
    return c;
}

/**
 * Check whether a slot is invalid or contains no item template.
 *
 * @param si  Slot index, which may be outside the inventory range.
 * @return      Nonzero when invalid or empty; otherwise zero.
 */
int inventory_slot_is_empty(const InventoryState* inv, int si) {
    if (si < 0 || si >= INVENTORY_SIZE) return 1;
    return inv->slots[si].template_id == 0;
}
