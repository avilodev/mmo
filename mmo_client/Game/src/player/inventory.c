/**
 * @file
 * Load client item metadata and manage inventory interaction and rendering.
 */

#include "inventory.h"
#include "network.h"
#include "renderer.h"
#include "ui/ui_kit.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include "core/client_log.h"

// Height of the title strip above the slots, and the close button's size and inset
#define INV_HEADER_H 34.0f
#define INV_CLOSE_SIZE 22.0f
#define INV_CLOSE_INSET 6.0f

/**
 * Initialize inventory layout and interaction state.
 *
 * @param screen_width  Logical screen width in pixels.
 * @param screen_height  Logical screen height in pixels.
 */
void inventory_init(InventoryState* inv, float screen_width, float screen_height) {
    memset(inv, 0, sizeof(InventoryState));
    
    inv->slot_size = 34.0f;
    inv->slot_padding = 3.0f;
    
    inv->window_width = INVENTORY_COLS * (inv->slot_size + inv->slot_padding) + inv->slot_padding * 2;
    inv->window_height = INVENTORY_ROWS * (inv->slot_size + inv->slot_padding) + inv->slot_padding * 2 + INV_HEADER_H;
    
    inv->window_x = (screen_width - inv->window_width) / 2.0f;
    inv->window_y = (screen_height - inv->window_height) / 2.0f;
    
    inv->screen_width = screen_width;
    inv->screen_height = screen_height;
    
    inv->hovered_slot = -1;
    inv->selected_slot = -1;
    inv->is_open = 0;
    inv->is_dragging_window = 0;
    
    // Close button (top right corner)
    inv->close_button_size = INV_CLOSE_SIZE;
    inv->close_button_x = inv->window_x + inv->window_width - inv->close_button_size - INV_CLOSE_INSET;
    inv->close_button_y = inv->window_y + INV_CLOSE_INSET;
    
    CLOG_INFO("[INVENTORY] Initialized %dx%d grid", INVENTORY_COLS, INVENTORY_ROWS);
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

    CLOG_INFO("[INVENTORY] Loaded from server data");
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
    if (!inv->is_open) 
        return -1;
    
    float content_x = inv->window_x + inv->slot_padding;
    float content_y = inv->window_y + INV_HEADER_H;
    
    float rel_x = mouse_x - content_x;
    float rel_y = mouse_y - content_y;
    
    if (rel_x < 0 || rel_y < 0) 
        return -1;
    
    float slot_total = inv->slot_size + inv->slot_padding;
    int col = (int)(rel_x / slot_total);
    int row = (int)(rel_y / slot_total);
    
    if (col < 0 || col >= INVENTORY_COLS) 
        return -1;
    if (row < 0 || row >= INVENTORY_ROWS) 
        return -1;
    
    float slot_x = col * slot_total;
    float slot_y = row * slot_total;
    
    if (rel_x < slot_x || rel_x > slot_x + inv->slot_size) 
        return -1;
    if (rel_y < slot_y || rel_y > slot_y + inv->slot_size) 
        return -1;
    
    return row * INVENTORY_COLS + col;
}

static int is_mouse_in_title_bar(const InventoryState* inv, float mouse_x, float mouse_y) {
    return (mouse_x >= inv->window_x && 
            mouse_x <= inv->window_x + inv->window_width &&
            mouse_y >= inv->window_y && 
            mouse_y <= inv->window_y + INV_HEADER_H);
}

/**
 * Check whether a mouse position hits the visible close button.
 *
 * @return      Nonzero on a hit; otherwise zero.
 */
int inventory_check_close_button(const InventoryState* inv, float mouse_x, float mouse_y) {
    if (!inv->is_open) 
        return 0;
    
    float x = inv->close_button_x;
    float y = inv->close_button_y;
    float size = inv->close_button_size;
    
    return (mouse_x >= x && mouse_x <= x + size && mouse_y >= y && mouse_y <= y + size);
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
                CLOG_DEBUG("[INV] Started dragging window");
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
            inv->close_button_x = inv->window_x + inv->window_width - inv->close_button_size - INV_CLOSE_INSET;
            inv->close_button_y = inv->window_y + INV_CLOSE_INSET;
            
            // Don't process slots while dragging window
            inv->hovered_slot = -1;
            inv->tooltip_visible = 0;
            return;
        }
    } else {
        // Mouse released - stop dragging
        if (inv->is_dragging_window) {
            CLOG_DEBUG("[INV] Stopped dragging window");
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
                    CLOG_DEBUG("[INV] Picked up item from slot %d", slot);
                }
            } else {
                // Place item
                if (slot == inv->selected_slot) {
                    inv->selected_slot = -1;
                    CLOG_DEBUG("[INV] Put item back in same slot");
                } else {
                    /* Swapped locally first so the bag answers the click
                     * immediately, then asked of the server. A refusal is
                     * undone by inventory_revert_move() from the
                     * MOVE_ITEM_RESPONSE handler. */
                    ItemSlot temp = inv->slots[slot];
                    inv->slots[slot] = inv->slots[inv->selected_slot];
                    inv->slots[inv->selected_slot] = temp;
                    network_send_move_item((uint8_t)inv->selected_slot, (uint8_t)slot);
                    inv->selected_slot = -1;
                    CLOG_DEBUG("[INV] Swapped items");
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

int inventory_revert_move(InventoryState* inv, int from_slot, int to_slot) {
    if (!inv) 
        return 0;
    if (from_slot < 0 || from_slot >= INVENTORY_SIZE) 
        return 0;
    if (to_slot   < 0 || to_slot   >= INVENTORY_SIZE) 
        return 0;
    if (from_slot == to_slot) 
        return 0;

    ItemSlot temp       = inv->slots[to_slot];
    inv->slots[to_slot] = inv->slots[from_slot];
    inv->slots[from_slot] = temp;

    /* A held item referring to either slot is no longer meaningful once the
     * arrangement has moved under it. */
    if (inv->selected_slot == from_slot || inv->selected_slot == to_slot)
        inv->selected_slot = -1;

    return 1;
}

static void get_rarity_color(ItemRarity rarity, float* r, float* g, float* b) {
    switch (rarity) {
        case ITEM_RARITY_COMMON: 
            *r=0.8f; *g=0.8f; *b=0.8f; 
            break;
        case ITEM_RARITY_UNCOMMON: 
            *r=0.2f; *g=1.0f; *b=0.2f; 
            break;
        case ITEM_RARITY_RARE: 
            *r=0.3f; *g=0.5f; *b=1.0f; 
            break;
        case ITEM_RARITY_EPIC: 
            *r=0.8f; *g=0.3f; *b=1.0f; 
            break;
        case ITEM_RARITY_LEGENDARY: 
            *r=1.0f; *g=0.6f; *b=0.0f; 
            break;
        default: 
            *r=0.5f; *g=0.5f; *b=0.5f;
    }
}

/**
 * Draw the inventory frame, title, and close button.
 */
static void render_inventory_window(const InventoryState* inv) {
	ui_kit_px_begin((int)inv->screen_width, (int)inv->screen_height);
	ui_kit_px_rect(inv->window_x, inv->window_y, inv->window_width, inv->window_height, 0.05f,
	               0.05f, 0.09f, 0.97f);
	ui_kit_px_panel(inv->window_x, inv->window_y, inv->window_width, inv->window_height);
	ui_kit_px_text(inv->window_x + 14.0f, inv->window_y + INV_HEADER_H * 0.5f, "Inventory",
	               20.0f, KIT_GOLD, KIT_LEFT);
	ui_kit_px_close_button(inv->close_button_x, inv->close_button_y, inv->close_button_size, 0);
	ui_kit_px_end();
}

/**
 * Draw visible inventory slots and their item representations.
 */
static void render_inventory_slots(const InventoryState* inv) {
    float cx = inv->window_x + inv->slot_padding;
    float cy = inv->window_y + INV_HEADER_H;
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
    if (inv->selected_slot < 0 || inv->selected_slot >= INVENTORY_SIZE) 
        return;
    
    const ItemSlot* slot = &inv->slots[inv->selected_slot];
    if (slot->template_id == 0) 
        return;
    
    const ItemTemplate* item = item_db_get(slot->template_id);
    if (!item) 
        return;
    
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
    if (item->type == ITEM_TYPE_CONSUMABLE) { 
        ir=0.3f; ig=0.8f; ib=0.3f; 
    } else if (item->type == ITEM_TYPE_EQUIPMENT) { 
        ir=0.7f; ig=0.7f; ib=0.8f; 
    } else if (item->type == ITEM_TYPE_QUEST) { 
        ir=1.0f; ig=0.8f; ib=0.2f; 
    }
    
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
// Tooltip box width and line spacing, in pixels
#define TOOLTIP_W 220.0f
#define TOOLTIP_LINE_H 15.0f

void inventory_draw_item_tooltip(const ItemTemplate* item, float x, float y,
                                 float screen_width, float screen_height) {
	char lines[ITEM_TOOLTIP_MAX_LINES][ITEM_TOOLTIP_LINE_LEN];
	int count = item_tooltip_lines(item, lines, ITEM_TOOLTIP_MAX_LINES);
	int desc_lines = 0;
	if(item->description[0])
		desc_lines = renderer_measure_text_wrapped(TOOLTIP_W - 10.0f, item->description);

	float h = 35.0f + (count + desc_lines) * TOOLTIP_LINE_H + 8.0f;
	if(x + TOOLTIP_W > screen_width)
		x = screen_width - TOOLTIP_W - 10;
	if(y + h > screen_height)
		y = screen_height - h - 10;

	renderer_draw_rect(x, y, TOOLTIP_W, h, 0.05f, 0.05f, 0.1f, 0.95f);
	float r, g, b;
	get_rarity_color(item->rarity, &r, &g, &b);
	renderer_draw_rect(x, y, TOOLTIP_W, 2, r, g, b, 1.0f);
	renderer_draw_rect(x, y + h - 2, TOOLTIP_W, 2, r, g, b, 1.0f);
	renderer_draw_rect(x, y, 2, h, r, g, b, 1.0f);
	renderer_draw_rect(x + TOOLTIP_W - 2, y, 2, h, r, g, b, 1.0f);

	renderer_draw_text(x + 5, y + 15, item->name);
	for(int i = 0; i < count; i++)
		renderer_draw_text(x + 5, y + 35 + i * TOOLTIP_LINE_H, lines[i]);

	if(desc_lines > 0)
		renderer_draw_text_wrapped(x + 5, y + 35 + count * TOOLTIP_LINE_H + 5,
		                           TOOLTIP_W - 10.0f, TOOLTIP_LINE_H, item->description);
}

static void render_tooltip(const InventoryState* inv) {
	if(!inv->tooltip_visible || inv->hovered_slot < 0)
		return;

	const ItemSlot* slot = &inv->slots[inv->hovered_slot];
	if(slot->template_id == 0)
		return;

	const ItemTemplate* item = item_db_get(slot->template_id);
	if(!item)
		return;

	inventory_draw_item_tooltip(item, inv->tooltip_x, inv->tooltip_y,
	                            inv->screen_width, inv->screen_height);
}

/**
 * Render an open inventory in screen space.
 *
 * The function preserves the current OpenGL projection and model-view matrices.
 */
void inventory_render(const InventoryState* inv) {
    if (!inv->is_open) return;
    renderer_begin_screen_space();
    
    render_inventory_window(inv);
    render_inventory_slots(inv);
    render_tooltip(inv);
    
    // Render dragged item on top of everything
    render_dragged_item(inv);
    
    renderer_end_screen_space();
}

/**
 * Remove up to a requested quantity from one slot.
 *
 * @param si  Slot index, which may be outside the inventory range.
 * @param q  Maximum quantity to remove.
 * @return      Quantity actually removed.
 */
uint16_t inventory_remove_item(InventoryState* inv, int si, uint16_t q) {
    if (si < 0 || si >= INVENTORY_SIZE) 
        return 0;

    ItemSlot* s = &inv->slots[si];
    if (s->template_id == 0) 
        return 0;

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
    if (si < 0 || si >= INVENTORY_SIZE) 
        return;

    ItemSlot* s = &inv->slots[si];
    if (s->template_id == 0) 
        return;

    const ItemTemplate* item = item_db_get(s->template_id);
    if (!item) 
        return;

    if (item->type == ITEM_TYPE_CONSUMABLE) {
        CLOG_INFO("[INVENTORY] Using %s (sending to server)", item->name);
        network_send_use_item((uint8_t)si);
        // Server will confirm via USE_ITEM_RESPONSE
    } else if (item->type == ITEM_TYPE_EQUIPMENT && item->equip_slot != EQUIP_SLOT_NONE) {
        // Map client EquipSlot to server EquipSlotId
        uint8_t server_slot = 0;
        switch (item->equip_slot) {
            case EQUIP_SLOT_HELMET:      
                server_slot = EQUIP_SLOT_ID_HELMET; 
                break;
            case EQUIP_SLOT_CHEST:       
                server_slot = EQUIP_SLOT_ID_CHEST; 
                break;
            case EQUIP_SLOT_GLOVES:      
                server_slot = EQUIP_SLOT_ID_GLOVES; 
                break;
            case EQUIP_SLOT_LEGGINGS:    
                server_slot = EQUIP_SLOT_ID_LEGGINGS; 
                break;
            case EQUIP_SLOT_BOOTS:       
                server_slot = EQUIP_SLOT_ID_BOOTS; 
                break;
            case EQUIP_SLOT_MAIN_HAND:   
                server_slot = EQUIP_SLOT_ID_MAIN_HAND; 
                break;
            case EQUIP_SLOT_SECOND_HAND: 
                server_slot = EQUIP_SLOT_ID_OFF_HAND; 
                break;
            default: 
                CLOG_ERROR("[INVENTORY] Unknown equipment slot for item %s", item->name);
                break;
        }

        if (server_slot > 0) {
            CLOG_INFO("[INVENTORY] Equipping %s to slot %u", item->name, server_slot);
            network_send_equip_item(s->template_id, (uint8_t)si, server_slot);
        }
    }
}
