/**
 * @file
 * Manage the draggable client character panel, equipment slots, doll, and stats.
 */

#include "character_screen.h"
#include "core/race_registry.h"
#include "renderer.h"
#include "inventory.h"
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "core/client_log.h"

// Equipment slot indices for hovered_slot
#define SLOT_MAIN_HAND  0
#define SLOT_OFF_HAND   1
#define SLOT_HELMET     2
#define SLOT_CHEST      3
#define SLOT_GLOVES     4
#define SLOT_LEGGINGS   5
#define SLOT_BOOTS      6
#define SLOT_BLESSING   7  // NEW

/**
 * Place every part of the panel relative to its window position.
 */
static void layout(CharacterScreenState* char_screen) {
	float weapons_width = char_screen->slot_size + char_screen->slot_padding * 2;
	float weapon_x = char_screen->window_x + char_screen->slot_padding;
	float weapon_start_y = char_screen->window_y + 80.0f;
	float armor_x = char_screen->window_x + weapons_width + char_screen->char_display_width +
	                char_screen->slot_padding * 3;
	float armor_start_y = char_screen->window_y + 80.0f;
	float armor_spacing = char_screen->slot_size + char_screen->slot_padding;
	EquipSlotUI* armor[] = {
		&char_screen->helmet_slot, &char_screen->chest_slot, &char_screen->gloves_slot,
		&char_screen->leggings_slot, &char_screen->boots_slot, &char_screen->blessing_slot
	};

	char_screen->char_display_x = char_screen->window_x + weapons_width +
	                              char_screen->slot_padding * 2;
	char_screen->char_display_y = char_screen->window_y + 50.0f;

	// Left side - weapons (vertical)
	char_screen->main_hand_slot.x = weapon_x;
	char_screen->main_hand_slot.y = weapon_start_y;
	char_screen->main_hand_slot.size = char_screen->slot_size;
	char_screen->off_hand_slot.x = weapon_x;
	char_screen->off_hand_slot.y = weapon_start_y + armor_spacing;
	char_screen->off_hand_slot.size = char_screen->slot_size;

	// Right side - armor (vertical), blessing last
	for(int i = 0; i < 6; i++) {
		armor[i]->x = armor_x;
		armor[i]->y = armor_start_y + armor_spacing * (float)i;
		armor[i]->size = char_screen->slot_size;
	}

	// Close button (top right corner)
	char_screen->close_button_x = char_screen->window_x + char_screen->window_width -
	                              char_screen->close_button_size - 5.0f;
	char_screen->close_button_y = char_screen->window_y + 5.0f;
}

/**
 * Initialize character-panel layout and interaction state.
 *
 * @param screen_width  Logical screen width in pixels.
 * @param screen_height  Logical screen height in pixels.
 */
void character_screen_init(CharacterScreenState* char_screen, float screen_width, float screen_height) {
    memset(char_screen, 0, sizeof(CharacterScreenState));

    char_screen->slot_size = 50.0f;
    char_screen->slot_padding = 10.0f;

    // Window size: weapons (left) + character (center) + armor (right) + padding
    float weapons_width = char_screen->slot_size + char_screen->slot_padding * 2;
    float armor_width = char_screen->slot_size + char_screen->slot_padding * 2;
    float char_display_width = 150.0f;

    char_screen->window_width = weapons_width + char_display_width + armor_width + char_screen->slot_padding * 4;
    char_screen->window_height = 590.0f;

    // Center window on screen
    char_screen->window_x = (screen_width - char_screen->window_width) / 2.0f;
    char_screen->window_y = (screen_height - char_screen->window_height) / 2.0f;

    char_screen->screen_width = screen_width;
    char_screen->screen_height = screen_height;

    // Character display area (center)
    char_screen->char_display_width = char_display_width;
    char_screen->char_display_height = 200.0f;

    char_screen->close_button_size = 25.0f;
    layout(char_screen);

    char_screen->hovered_slot = -1;
    char_screen->is_open = 0;

    CLOG_INFO("[CHAR_SCREEN] Initialized %.0fx%.0f window with blessing slot",
           char_screen->window_width, char_screen->window_height);
}

/**
 * Move the window, keeping all of it on screen, and lay out its parts again.
 */
void character_screen_place(CharacterScreenState* char_screen, float x, float y) {
	float max_x = char_screen->screen_width - char_screen->window_width;
	float max_y = char_screen->screen_height - char_screen->window_height;

	char_screen->window_x = x > max_x ? max_x : x;
	char_screen->window_y = y > max_y ? max_y : y;
	if(char_screen->window_x < 0)
		char_screen->window_x = 0;
	if(char_screen->window_y < 0)
		char_screen->window_y = 0;
	layout(char_screen);
}

/**
 * Toggle character-panel visibility and clear interaction state when closing.
 */
void character_screen_toggle(CharacterScreenState* char_screen) {
    char_screen->is_open = !char_screen->is_open;
    if (!char_screen->is_open) {
        char_screen->hovered_slot = -1;
        char_screen->tooltip_visible = 0;
        char_screen->is_dragging_window = 0;
    }
}

/**
 * Resolve a mouse position to an equipment slot.
 *
 * @return      Equipment slot index, or -1 outside the open panel's slots.
 */
static int get_slot_at_position(const CharacterScreenState* char_screen, float mouse_x, float mouse_y) {
    if (!char_screen->is_open) return -1;

    // Check each equipment slot (now 8 slots with blessing)
    const EquipSlotUI* slots[] = {
        &char_screen->main_hand_slot,   // 0
        &char_screen->off_hand_slot,    // 1
        &char_screen->helmet_slot,      // 2
        &char_screen->chest_slot,       // 3
        &char_screen->gloves_slot,      // 4
        &char_screen->leggings_slot,    // 5
        &char_screen->boots_slot,       // 6
        &char_screen->blessing_slot     // 7 
    };

    for (int i = 0; i < 8; i++) {
        float x = slots[i]->x;
        float y = slots[i]->y;
        float size = slots[i]->size;

        if (mouse_x >= x && mouse_x <= x + size &&
            mouse_y >= y && mouse_y <= y + size) {
            return i;
        }
    }

    return -1;
}

static int is_mouse_in_title_bar(const CharacterScreenState* char_screen, float mouse_x, float mouse_y) {
    return (mouse_x >= char_screen->window_x &&
            mouse_x <= char_screen->window_x + char_screen->window_width &&
            mouse_y >= char_screen->window_y &&
            mouse_y <= char_screen->window_y + 35);
}

/**
 * Check whether a mouse position hits the close button.
 *
 * @return      Nonzero on a hit; otherwise zero.
 */
int character_screen_check_close_button(const CharacterScreenState* char_screen,
                                       float mouse_x, float mouse_y) {
    float x = char_screen->close_button_x;
    float y = char_screen->close_button_y;
    float size = char_screen->close_button_size;

    return (mouse_x >= x && mouse_x <= x + size &&
            mouse_y >= y && mouse_y <= y + size);
}

/**
 * Update panel dragging, close handling, and equipment hover state.
 *
 * @param mouse_clicked  Nonzero on a new left-button press.
 * @param mouse_down  Nonzero while the left button is held.
 */
void character_screen_update(CharacterScreenState* char_screen, float mouse_x, float mouse_y,
                            int mouse_clicked, int mouse_down, int right_clicked) {
    (void)right_clicked;
    if (!char_screen->is_open) {
        char_screen->hovered_slot = -1;
        char_screen->tooltip_visible = 0;
        char_screen->is_dragging_window = 0;
        return;
    }

    // Update tooltip position
    char_screen->tooltip_x = mouse_x;
    char_screen->tooltip_y = mouse_y;

    // Handle close button click
    if (mouse_clicked && character_screen_check_close_button(char_screen, mouse_x, mouse_y)) {
        character_screen_toggle(char_screen);
        return;
    }

    // Handle window dragging
    if (mouse_down) {
        if (!char_screen->is_dragging_window) {
            if (mouse_clicked && is_mouse_in_title_bar(char_screen, mouse_x, mouse_y)) {
                if (!character_screen_check_close_button(char_screen, mouse_x, mouse_y)) {
                    char_screen->is_dragging_window = 1;
                    char_screen->drag_offset_x = mouse_x - char_screen->window_x;
                    char_screen->drag_offset_y = mouse_y - char_screen->window_y;
                }
            }
        } else {
            // Continue dragging
            char_screen->window_x = mouse_x - char_screen->drag_offset_x;
            char_screen->window_y = mouse_y - char_screen->drag_offset_y;

            character_screen_place(char_screen, char_screen->window_x, char_screen->window_y);

            char_screen->hovered_slot = -1;
            char_screen->tooltip_visible = 0;
            return;
        }
    } else {
        char_screen->is_dragging_window = 0;
    }

    // Get slot under mouse
    int slot = get_slot_at_position(char_screen, mouse_x, mouse_y);
    char_screen->hovered_slot = slot;

    // Show tooltip for equipped items
    if (slot >= 0) {
        char_screen->tooltip_visible = 1;
    } else {
        char_screen->tooltip_visible = 0;
    }
}

/**
 * Draw one equipment slot with its item rarity or empty label.
 *
 * @param item_id  Item template identifier, or zero for an empty slot.
 */
static void render_equipment_slot(const EquipSlotUI* slot, int is_hovered,
                                  uint32_t item_id, const char* slot_name) {
    float x = slot->x;
    float y = slot->y;
    float size = slot->size;

    // Background
    float r = 0.15f, g = 0.15f, b = 0.2f;
    if (is_hovered) { r += 0.1f; g += 0.1f; b += 0.1f; }

    renderer_draw_rect(x, y, size, size, r, g, b, 1.0f);

    // Border
    renderer_draw_rect(x, y, size, 1, 0.4f, 0.4f, 0.4f, 1.0f);
    renderer_draw_rect(x, y + size - 1, size, 1, 0.4f, 0.4f, 0.4f, 1.0f);
    renderer_draw_rect(x, y, 1, size, 0.4f, 0.4f, 0.4f, 1.0f);
    renderer_draw_rect(x + size - 1, y, 1, size, 0.4f, 0.4f, 0.4f, 1.0f);

    // If item equipped, show it
    if (item_id > 0) {
        const ItemTemplate* item = item_db_get(item_id);
        if (item) {
            // Rarity border
            float rr = 0.5f, gg = 0.5f, bb = 0.5f;
            switch (item->rarity) {
                case ITEM_RARITY_COMMON: rr=0.8f; gg=0.8f; bb=0.8f; break;
                case ITEM_RARITY_UNCOMMON: rr=0.2f; gg=1.0f; bb=0.2f; break;
                case ITEM_RARITY_RARE: rr=0.3f; gg=0.5f; bb=1.0f; break;
                case ITEM_RARITY_EPIC: rr=0.8f; gg=0.3f; bb=1.0f; break;
                case ITEM_RARITY_LEGENDARY: rr=1.0f; gg=0.6f; bb=0.0f; break;
            }

            float bs = 2.0f;
            renderer_draw_rect(x+bs, y+bs, size-bs*2, 1, rr, gg, bb, 1.0f);
            renderer_draw_rect(x+bs, y+size-bs-1, size-bs*2, 1, rr, gg, bb, 1.0f);
            renderer_draw_rect(x+bs, y+bs, 1, size-bs*2, rr, gg, bb, 1.0f);
            renderer_draw_rect(x+size-bs-1, y+bs, 1, size-bs*2, rr, gg, bb, 1.0f);

            // Item icon (colored box based on type)
            float ir = 0.7f, ig = 0.7f, ib = 0.8f;
            if (item->type == ITEM_TYPE_EQUIPMENT) {
                ir = 0.7f; ig = 0.7f; ib = 0.8f;
            }

            float ip = 8.0f;
            renderer_draw_rect(x+ip, y+ip, size-ip*2, size-ip*2, ir, ig, ib, 0.8f);
        }
    } else {
        // Empty slot
        renderer_draw_text(x + 5, y + size / 2, slot_name);
    }
}

/**
 * Draw the hovered equipment slot's bounded tooltip.
 */
static void render_tooltip(const CharacterScreenState* char_screen, const CharacterView* view) {
    if (!char_screen->tooltip_visible || char_screen->hovered_slot < 0) return;
    if (!view->loaded) return;

    // Get item ID based on hovered slot
    uint32_t item_id = 0;
    const char* slot_name = "";

    switch (char_screen->hovered_slot) {
        case SLOT_MAIN_HAND:  
            item_id = view->equipment[EQUIP_MAIN_HAND]; slot_name = "Main Hand"; 
            break;
        case SLOT_OFF_HAND:   
            item_id = view->equipment[EQUIP_SECOND_HAND]; slot_name = "Off Hand"; 
            break;
        case SLOT_HELMET:     
            item_id = view->equipment[EQUIP_HELMET]; slot_name = "Helmet"; 
            break;
        case SLOT_CHEST:     
            item_id = view->equipment[EQUIP_CHEST]; slot_name = "Chest"; 
            break;
        case SLOT_GLOVES:     
            item_id = view->equipment[EQUIP_GLOVES]; slot_name = "Gloves"; 
            break;
        case SLOT_LEGGINGS:   
            item_id = view->equipment[EQUIP_LEGGINGS]; slot_name = "Leggings"; 
            break;
        case SLOT_BOOTS:      
            item_id = view->equipment[EQUIP_BOOTS]; slot_name = "Boots"; 
            break;
        case SLOT_BLESSING:   
            item_id = view->equipment[EQUIP_BLESSING]; slot_name = "Blessing"; 
            break; 
    }

    if (item_id == 0) {
        // Show empty slot tooltip
        float w = 120.0f, h = 40.0f;
        float x = char_screen->tooltip_x + 10;
        float y = char_screen->tooltip_y + 10;

        if (x + w > char_screen->screen_width) 
            x = char_screen->screen_width - w - 10;

        if (y + h > char_screen->screen_height) 
            y = char_screen->screen_height - h - 10;

        renderer_draw_rect(x, y, w, h, 0.05f, 0.05f, 0.1f, 0.95f);
        renderer_draw_rect(x, y, w, 2, 0.6f, 0.6f, 0.6f, 1.0f);

        renderer_draw_text(x + 5, y + 20, slot_name);
        renderer_draw_text(x + 5, y + 35, "(Empty)");
        return;
    }

	const ItemTemplate* item = item_db_get(item_id);
	if(!item)
		return;

	inventory_draw_item_tooltip(item, char_screen->tooltip_x + 10,
	                            char_screen->tooltip_y + 10,
	                            char_screen->screen_width, char_screen->screen_height);
}

static void doll_rarity_color(uint32_t item_id, float* r, float* g, float* b) {
    const ItemTemplate* item = item_db_get(item_id);
    if (!item || item_id == 0) { 
        *r=0.28f; *g=0.28f; *b=0.32f; 
        return; 
    }

    switch (item->rarity) {
        case ITEM_RARITY_COMMON:    
            *r=0.50f; *g=0.50f; *b=0.52f; 
            break;
        case ITEM_RARITY_UNCOMMON:  
            *r=0.15f; *g=0.65f; *b=0.20f; 
            break;
        case ITEM_RARITY_RARE:      
            *r=0.20f; *g=0.40f; *b=0.80f; 
            break;
        case ITEM_RARITY_EPIC:      
            *r=0.55f; *g=0.15f; *b=0.80f; 
            break;
        case ITEM_RARITY_LEGENDARY: 
            *r=0.80f; *g=0.45f; *b=0.00f; 
            break;
        default:                    
            *r=0.28f; *g=0.28f; *b=0.32f; 
            break;
    }
}

/**
 * Pick the doll's base colour for a race.
 *
 * From the race registry the server sent, the same source the nearby-player
 * markers and the party frames use, so one race is one colour everywhere the
 * client draws it.
 *
 * This was an eleven-entry table keyed by race id, with the ids of the shipped
 * races written into the comments. It was a second copy of a list the server
 * already sends: adding a race to races.json meant editing this file too, and
 * forgetting to meant the twelfth race drew in the "unused" grey. Reordering
 * races.json would have silently recoloured all of them.
 */
static void race_base_color(uint32_t race_id, float* r, float* g, float* b) {
    client_race_color(race_id, r, g, b);
}

// Draw a region rect with a 1px dark outline
static void doll_rect(float x, float y, float w, float h, float r, float g, float b) {
    renderer_draw_rect(x - 1, y - 1, w + 2, h + 2, 0.05f, 0.04f, 0.07f, 1.0f);
    renderer_draw_rect(x, y, w, h, r, g, b, 1.0f);
}

/**
 * Draw the character doll using class and equipped-item rarity colors.
 */
static void render_character_doll(const CharacterScreenState* cs, const CharacterView* view) {
    float dx = cs->char_display_x;
    float dy = cs->char_display_y;
    float dw = cs->char_display_width;
    float dh = cs->char_display_height;
    float cx = dx + dw * 0.5f;

    // Panel background
    renderer_draw_rect(dx, dy, dw, dh, 0.10f, 0.08f, 0.13f, 1.0f);

    if (!view->loaded) {
        renderer_draw_text_centered(dx, dy + dh * 0.5f, dw, 0.0f, "Loading...");
        return;
    }

    // Name + level
    char name_label[48];
    snprintf(name_label, sizeof(name_label), "%u - %s",
             (unsigned)view->level, view->name);
    renderer_draw_text_centered(dx, dy + 14.0f, dw, 0.0f, name_label);

    /* Form, dimmed, beneath the name. The race's own name comes from the server's
     * registry and is shown at character select rather than duplicated here. */
    const char* form_label =
        view->form == FORM_ANIMAL ? "Animal Form" : "Human Form";
    renderer_draw_text_colored(cx - renderer_text_width(form_label) * 0.5f, dy + 37.0f,
                               form_label, 0.55f, 0.55f, 0.65f);

    float r, g, b;
    float fy = dy + 42.0f; // figure top

    // HEAD (22×22) — helmet color
    float head_w = 22.0f, head_h = 22.0f;
    float head_x = cx - head_w * 0.5f;
    if (view->equipment[EQUIP_HELMET])
        doll_rarity_color(view->equipment[EQUIP_HELMET], &r, &g, &b);
    else
        race_base_color(view->race_id, &r, &g, &b);
    doll_rect(head_x, fy, head_w, head_h, r, g, b);

    // NECK (8×8) — always class base, darker
    float neck_w = 8.0f, neck_h = 8.0f;
    float neck_y = fy + head_h;
    race_base_color(view->race_id, &r, &g, &b);
    renderer_draw_rect(cx - neck_w * 0.5f, neck_y, neck_w, neck_h, r * 0.6f, g * 0.6f, b * 0.6f, 1.0f);

    // TORSO (48×44) — chest armor color
    float torso_w = 48.0f, torso_h = 44.0f;
    float torso_x = cx - torso_w * 0.5f;
    float torso_y = neck_y + neck_h;
    if (view->equipment[EQUIP_CHEST])
        doll_rarity_color(view->equipment[EQUIP_CHEST], &r, &g, &b);
    else
        race_base_color(view->race_id, &r, &g, &b);
    doll_rect(torso_x, torso_y, torso_w, torso_h, r, g, b);

    // ARMS — upper half class base, lower half gloves color
    float arm_w = 13.0f, arm_upper_h = 22.0f, arm_lower_h = 20.0f;
    float arm_y = torso_y;
    float left_arm_x  = torso_x - arm_w - 2.0f;
    float right_arm_x = torso_x + torso_w + 2.0f;

    race_base_color(view->race_id, &r, &g, &b);
    doll_rect(left_arm_x,  arm_y, arm_w, arm_upper_h, r, g, b);
    doll_rect(right_arm_x, arm_y, arm_w, arm_upper_h, r, g, b);

    float glove_y = arm_y + arm_upper_h;
    if (view->equipment[EQUIP_GLOVES])
        doll_rarity_color(view->equipment[EQUIP_GLOVES], &r, &g, &b);
    else
        race_base_color(view->race_id, &r, &g, &b);
    r *= 0.8f; g *= 0.8f; b *= 0.8f;
    doll_rect(left_arm_x,  glove_y, arm_w, arm_lower_h, r, g, b);
    doll_rect(right_arm_x, glove_y, arm_w, arm_lower_h, r, g, b);

    // LEGS (two 20×38) — leggings color
    float leg_w = 20.0f, leg_h = 38.0f;
    float leg_y = torso_y + torso_h;
    float left_leg_x  = cx - leg_w - 2.0f;
    float right_leg_x = cx + 2.0f;
    if (view->equipment[EQUIP_LEGGINGS])
        doll_rarity_color(view->equipment[EQUIP_LEGGINGS], &r, &g, &b);
    else
        race_base_color(view->race_id, &r, &g, &b);
    doll_rect(left_leg_x,  leg_y, leg_w, leg_h, r, g, b);
    doll_rect(right_leg_x, leg_y, leg_w, leg_h, r, g, b);

    // FEET (boots color, slightly wider than legs)
    float foot_w = 22.0f, foot_h = 11.0f;
    float foot_y = leg_y + leg_h;
    if (view->equipment[EQUIP_BOOTS])
        doll_rarity_color(view->equipment[EQUIP_BOOTS], &r, &g, &b);
    else
        race_base_color(view->race_id, &r, &g, &b);
    doll_rect(left_leg_x  - 1.0f, foot_y, foot_w, foot_h, r, g, b);
    doll_rect(right_leg_x - 1.0f, foot_y, foot_w, foot_h, r, g, b);
}

/**
 * Draw the loaded character's combat-stat grid.
 */
static void render_character_stats(const CharacterScreenState* cs, const CharacterView* view) {
    if (!view->loaded) return;

    float sx = cs->char_display_x;
    float sy = cs->char_display_y + cs->char_display_height + 6.0f;
    float sw = cs->char_display_width;
    float sh = 158.0f;
    float mid = sx + sw * 0.5f;

    // Background + top divider
    renderer_draw_rect(sx, sy, sw, sh, 0.10f, 0.08f, 0.13f, 1.0f);
    renderer_draw_rect(sx, sy, sw, 1.5f, 0.30f, 0.30f, 0.45f, 1.0f);

    // "STATS" header
    renderer_draw_text_centered(sx, sy + 8.0f, sw, 0.0f, "STATS");

    // Center vertical divider
    renderer_draw_rect(mid - 0.75f, sy + 22.0f, 1.5f, sh - 24.0f, 0.22f, 0.22f, 0.32f, 1.0f);

    char buf[16];
    float row_h = 18.0f;
    float lx = sx + 6.0f;
    float rx = mid + 6.0f;
    float ry = sy + 42.0f;

    static const char* const stat_labels[STAT_COUNT] = {
        "STR", "DEX", "VIT", "INT", "FOC", "END", "FER", "STA", "PRE", "FRL", "ARM"
    };

    renderer_draw_text_colored(lx, ry, "ATK", 0.60f, 0.60f, 0.70f);
    snprintf(buf, sizeof(buf), "%d", view->weapon_damage);
    renderer_draw_text(lx + 46.0f, ry, buf);

    /* Split the attributes down the panel's two columns, filling the left first. */
    int left_rows = (STAT_COUNT + 1) / 2;
    for (int i = 0; i < STAT_COUNT; i++) {
        int in_left = (i < left_rows);
        float col_x = in_left ? lx : rx;
        float row_y = ry + ((in_left ? i + 1 : i - left_rows) * row_h);

        renderer_draw_text_colored(col_x, row_y, stat_labels[i], 0.60f, 0.60f, 0.70f);
        snprintf(buf, sizeof(buf), "%d", view->stats[i]);
        renderer_draw_text(col_x + 46.0f, row_y, buf);
    }
}

void character_view_from_self(const GameState* game, CharacterView* out) {
	memset(out, 0, sizeof(*out));
	if(!game->player.info_loaded)
		return;

	out->loaded = 1;
	memcpy(out->name, game->player.info.name, sizeof(out->name) - 1);
	out->level = game->player.info.level;
	out->race_id = game->player.info.race_id;
	out->form = game->playing->player_form;
	out->weapon_damage = game->playing->player_weapon_damage;
	for(int i = 0; i < EQUIP_SLOTS; i++)
		out->equipment[i] = game->player.info.equipment[i].item_id;
	for(int i = 0; i < STAT_COUNT; i++)
		out->stats[i] = game->playing->player_stats[i];
}

/**
 * Render an open character panel in screen space.
 *
 * The function preserves the current OpenGL projection and model-view matrices.
 */
void character_screen_render(const CharacterScreenState* char_screen, const CharacterView* view,
                             const char* title) {
    if (!char_screen->is_open) return;

    renderer_begin_screen_space();

    // Window background
    renderer_draw_rect(char_screen->window_x, char_screen->window_y,
                      char_screen->window_width, char_screen->window_height,
                      0.1f, 0.1f, 0.15f, 0.95f);

    // Title bar
    renderer_draw_rect(char_screen->window_x, char_screen->window_y,
                      char_screen->window_width, 35,
                      0.15f, 0.1f, 0.2f, 1.0f);

    // Window border
    float b = 2.0f;
    renderer_draw_rect(char_screen->window_x, char_screen->window_y, char_screen->window_width, b, 0.6f, 0.6f, 0.6f, 1.0f);
    renderer_draw_rect(char_screen->window_x, char_screen->window_y + char_screen->window_height - b, char_screen->window_width, b, 0.6f, 0.6f, 0.6f, 1.0f);
    renderer_draw_rect(char_screen->window_x, char_screen->window_y, b, char_screen->window_height, 0.6f, 0.6f, 0.6f, 1.0f);
    renderer_draw_rect(char_screen->window_x + char_screen->window_width - b, char_screen->window_y, b, char_screen->window_height, 0.6f, 0.6f, 0.6f, 1.0f);

    // Title text
    renderer_draw_text(char_screen->window_x + 10, char_screen->window_y + 20, title);

    // Close button (X)
    float close_x = char_screen->close_button_x;
    float close_y = char_screen->close_button_y;
    float close_size = char_screen->close_button_size;

    renderer_draw_rect(close_x, close_y, close_size, close_size, 0.3f, 0.1f, 0.1f, 1.0f);
    renderer_draw_rect(close_x, close_y, close_size, 1, 0.6f, 0.2f, 0.2f, 1.0f);
    renderer_draw_rect(close_x, close_y + close_size - 1, close_size, 1, 0.6f, 0.2f, 0.2f, 1.0f);
    renderer_draw_rect(close_x, close_y, 1, close_size, 0.6f, 0.2f, 0.2f, 1.0f);
    renderer_draw_rect(close_x + close_size - 1, close_y, 1, close_size, 0.6f, 0.2f, 0.2f, 1.0f);
    renderer_draw_text(close_x + 7, close_y + 18, "X");

    // Character doll + stats
    render_character_doll(char_screen, view);
    render_character_stats(char_screen, view);

    // Section labels
    renderer_draw_text(char_screen->window_x + 10, char_screen->window_y + 60, "Weapons");
    renderer_draw_text(char_screen->window_x + char_screen->window_width - 70,
                      char_screen->window_y + 60, "Armor");

    // Render equipment slots
    if (view->loaded) {
        render_equipment_slot(&char_screen->main_hand_slot,
                             char_screen->hovered_slot == SLOT_MAIN_HAND,
                             view->equipment[EQUIP_MAIN_HAND], "Main");

        render_equipment_slot(&char_screen->off_hand_slot,
                             char_screen->hovered_slot == SLOT_OFF_HAND,
                             view->equipment[EQUIP_SECOND_HAND], "Off");

        render_equipment_slot(&char_screen->helmet_slot,
                             char_screen->hovered_slot == SLOT_HELMET,
                             view->equipment[EQUIP_HELMET], "Head");

        render_equipment_slot(&char_screen->chest_slot,
                             char_screen->hovered_slot == SLOT_CHEST,
                             view->equipment[EQUIP_CHEST], "Chest");

        render_equipment_slot(&char_screen->gloves_slot,
                             char_screen->hovered_slot == SLOT_GLOVES,
                             view->equipment[EQUIP_GLOVES], "Hands");

        render_equipment_slot(&char_screen->leggings_slot,
                             char_screen->hovered_slot == SLOT_LEGGINGS,
                             view->equipment[EQUIP_LEGGINGS], "Legs");

        render_equipment_slot(&char_screen->boots_slot,
                             char_screen->hovered_slot == SLOT_BOOTS,
                             view->equipment[EQUIP_BOOTS], "Feet");

        // NEW: Render blessing slot
        render_equipment_slot(&char_screen->blessing_slot,
                             char_screen->hovered_slot == SLOT_BLESSING,
                             view->equipment[EQUIP_BLESSING], "Blessing");
    }

    // Render tooltip
    render_tooltip(char_screen, view);

    renderer_end_screen_space();
}
