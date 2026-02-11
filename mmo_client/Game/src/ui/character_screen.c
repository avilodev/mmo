#include "character_screen.h"
#include "renderer.h"
#include "inventory.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

// Equipment slot indices for hovered_slot
#define SLOT_MAIN_HAND  0
#define SLOT_OFF_HAND   1
#define SLOT_HELMET     2
#define SLOT_CHEST      3
#define SLOT_GLOVES     4
#define SLOT_LEGGINGS   5
#define SLOT_BOOTS      6
#define SLOT_BLESSING   7  // NEW

void character_screen_init(CharacterScreenState* char_screen, float screen_width, float screen_height) {
    memset(char_screen, 0, sizeof(CharacterScreenState));
    
    char_screen->slot_size = 50.0f;
    char_screen->slot_padding = 10.0f;
    
    // Window size: weapons (left) + character (center) + armor (right) + padding
    float weapons_width = char_screen->slot_size + char_screen->slot_padding * 2;
    float armor_width = char_screen->slot_size + char_screen->slot_padding * 2;
    float char_display_width = 150.0f;
    
    char_screen->window_width = weapons_width + char_display_width + armor_width + char_screen->slot_padding * 4;
    char_screen->window_height = 550.0f;  // Increased for blessing slot
    
    // Center window on screen
    char_screen->window_x = (screen_width - char_screen->window_width) / 2.0f;
    char_screen->window_y = (screen_height - char_screen->window_height) / 2.0f;
    
    char_screen->screen_width = screen_width;
    char_screen->screen_height = screen_height;
    
    // Character display area (center)
    char_screen->char_display_width = char_display_width;
    char_screen->char_display_height = 200.0f;
    char_screen->char_display_x = char_screen->window_x + weapons_width + char_screen->slot_padding * 2;
    char_screen->char_display_y = char_screen->window_y + 50.0f;
    
    // Left side - Weapons (vertical)
    float weapon_x = char_screen->window_x + char_screen->slot_padding;
    float weapon_start_y = char_screen->window_y + 80.0f;
    
    char_screen->main_hand_slot.x = weapon_x;
    char_screen->main_hand_slot.y = weapon_start_y;
    char_screen->main_hand_slot.size = char_screen->slot_size;
    
    char_screen->off_hand_slot.x = weapon_x;
    char_screen->off_hand_slot.y = weapon_start_y + char_screen->slot_size + char_screen->slot_padding;
    char_screen->off_hand_slot.size = char_screen->slot_size;
    
    // Right side - Armor (vertical) - 6 slots now including blessing
    float armor_x = char_screen->window_x + weapons_width + char_display_width + char_screen->slot_padding * 3;
    float armor_start_y = char_screen->window_y + 80.0f;
    float armor_spacing = char_screen->slot_size + char_screen->slot_padding;
    
    char_screen->helmet_slot.x = armor_x;
    char_screen->helmet_slot.y = armor_start_y;
    char_screen->helmet_slot.size = char_screen->slot_size;
    
    char_screen->chest_slot.x = armor_x;
    char_screen->chest_slot.y = armor_start_y + armor_spacing;
    char_screen->chest_slot.size = char_screen->slot_size;
    
    char_screen->gloves_slot.x = armor_x;
    char_screen->gloves_slot.y = armor_start_y + armor_spacing * 2;
    char_screen->gloves_slot.size = char_screen->slot_size;
    
    char_screen->leggings_slot.x = armor_x;
    char_screen->leggings_slot.y = armor_start_y + armor_spacing * 3;
    char_screen->leggings_slot.size = char_screen->slot_size;
    
    char_screen->boots_slot.x = armor_x;
    char_screen->boots_slot.y = armor_start_y + armor_spacing * 4;
    char_screen->boots_slot.size = char_screen->slot_size;
    
    // NEW: Blessing slot (6th armor slot)
    char_screen->blessing_slot.x = armor_x;
    char_screen->blessing_slot.y = armor_start_y + armor_spacing * 5;
    char_screen->blessing_slot.size = char_screen->slot_size;
    
    // Close button (top right corner)
    char_screen->close_button_size = 25.0f;
    char_screen->close_button_x = char_screen->window_x + char_screen->window_width - char_screen->close_button_size - 5.0f;
    char_screen->close_button_y = char_screen->window_y + 5.0f;
    
    char_screen->hovered_slot = -1;
    char_screen->is_open = 0;
    
    printf("[CHAR_SCREEN] Initialized %.0fx%.0f window with blessing slot\n", 
           char_screen->window_width, char_screen->window_height);
}

void character_screen_toggle(CharacterScreenState* char_screen) {
    char_screen->is_open = !char_screen->is_open;
    if (!char_screen->is_open) {
        char_screen->hovered_slot = -1;
        char_screen->tooltip_visible = 0;
        char_screen->is_dragging_window = 0;
    }
}

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
        &char_screen->blessing_slot     // 7 - NEW
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

int character_screen_check_close_button(const CharacterScreenState* char_screen, 
                                       float mouse_x, float mouse_y) {
    float x = char_screen->close_button_x;
    float y = char_screen->close_button_y;
    float size = char_screen->close_button_size;
    
    return (mouse_x >= x && mouse_x <= x + size &&
            mouse_y >= y && mouse_y <= y + size);
}

void character_screen_update(CharacterScreenState* char_screen, float mouse_x, float mouse_y,
                            int mouse_clicked, int mouse_down, int right_clicked) {
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
            
            // Clamp to screen bounds
            if (char_screen->window_x < 0) char_screen->window_x = 0;
            if (char_screen->window_y < 0) char_screen->window_y = 0;
            if (char_screen->window_x + char_screen->window_width > char_screen->screen_width) {
                char_screen->window_x = char_screen->screen_width - char_screen->window_width;
            }
            if (char_screen->window_y + char_screen->window_height > char_screen->screen_height) {
                char_screen->window_y = char_screen->screen_height - char_screen->window_height;
            }
            
            // Update all slot positions when window moves
            float weapons_width = char_screen->slot_size + char_screen->slot_padding * 2;
            
            // Recalculate positions
            float weapon_x = char_screen->window_x + char_screen->slot_padding;
            float weapon_start_y = char_screen->window_y + 80.0f;
            
            char_screen->main_hand_slot.x = weapon_x;
            char_screen->main_hand_slot.y = weapon_start_y;
            
            char_screen->off_hand_slot.x = weapon_x;
            char_screen->off_hand_slot.y = weapon_start_y + char_screen->slot_size + char_screen->slot_padding;
            
            float armor_x = char_screen->window_x + weapons_width + char_screen->char_display_width + char_screen->slot_padding * 3;
            float armor_start_y = char_screen->window_y + 80.0f;
            float armor_spacing = char_screen->slot_size + char_screen->slot_padding;
            
            char_screen->helmet_slot.x = armor_x;
            char_screen->helmet_slot.y = armor_start_y;
            
            char_screen->chest_slot.x = armor_x;
            char_screen->chest_slot.y = armor_start_y + armor_spacing;
            
            char_screen->gloves_slot.x = armor_x;
            char_screen->gloves_slot.y = armor_start_y + armor_spacing * 2;
            
            char_screen->leggings_slot.x = armor_x;
            char_screen->leggings_slot.y = armor_start_y + armor_spacing * 3;
            
            char_screen->boots_slot.x = armor_x;
            char_screen->boots_slot.y = armor_start_y + armor_spacing * 4;
            
            // NEW: Update blessing slot position
            char_screen->blessing_slot.x = armor_x;
            char_screen->blessing_slot.y = armor_start_y + armor_spacing * 5;
            
            char_screen->char_display_x = char_screen->window_x + weapons_width + char_screen->slot_padding * 2;
            char_screen->char_display_y = char_screen->window_y + 50.0f;
            
            char_screen->close_button_x = char_screen->window_x + char_screen->window_width - char_screen->close_button_size - 5.0f;
            char_screen->close_button_y = char_screen->window_y + 5.0f;
            
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
        // Empty slot - show slot name
        renderer_draw_text(x + 5, y + size / 2, slot_name);
    }
}

static void render_tooltip(const CharacterScreenState* char_screen, const GameState* game) {
    if (!char_screen->tooltip_visible || char_screen->hovered_slot < 0) return;
    if (!game->player.info_loaded) return;
    
    // Get item ID based on hovered slot
    uint32_t item_id = 0;
    const char* slot_name = "";
    
    switch (char_screen->hovered_slot) {
        case SLOT_MAIN_HAND:  item_id = game->player.info.main_hand; slot_name = "Main Hand"; break;
        case SLOT_OFF_HAND:   item_id = game->player.info.second_hand; slot_name = "Off Hand"; break;
        case SLOT_HELMET:     item_id = game->player.info.helmet; slot_name = "Helmet"; break;
        case SLOT_CHEST:      item_id = game->player.info.chest_armor; slot_name = "Chest"; break;
        case SLOT_GLOVES:     item_id = game->player.info.gloves; slot_name = "Gloves"; break;
        case SLOT_LEGGINGS:   item_id = game->player.info.leggings; slot_name = "Leggings"; break;
        case SLOT_BOOTS:      item_id = game->player.info.boots; slot_name = "Boots"; break;
        case SLOT_BLESSING:   item_id = game->player.info.blessing; slot_name = "Blessing"; break;  // NEW
    }
    
    if (item_id == 0) {
        // Show empty slot tooltip
        float w = 120.0f, h = 40.0f;
        float x = char_screen->tooltip_x + 10;
        float y = char_screen->tooltip_y + 10;
        
        if (x + w > char_screen->screen_width) x = char_screen->screen_width - w - 10;
        if (y + h > char_screen->screen_height) y = char_screen->screen_height - h - 10;
        
        renderer_draw_rect(x, y, w, h, 0.05f, 0.05f, 0.1f, 0.95f);
        renderer_draw_rect(x, y, w, 2, 0.6f, 0.6f, 0.6f, 1.0f);
        
        renderer_draw_text(x + 5, y + 20, slot_name);
        renderer_draw_text(x + 5, y + 35, "(Empty)");
        return;
    }
    
    const ItemTemplate* item = item_db_get(item_id);
    if (!item) return;
    
    // Show item tooltip
    float w = 220.0f, h = 120.0f;
    float x = char_screen->tooltip_x + 10;
    float y = char_screen->tooltip_y + 10;
    
    if (x + w > char_screen->screen_width) x = char_screen->screen_width - w - 10;
    if (y + h > char_screen->screen_height) y = char_screen->screen_height - h - 10;
    
    renderer_draw_rect(x, y, w, h, 0.05f, 0.05f, 0.1f, 0.95f);
    
    // Rarity border
    float r = 0.5f, g = 0.5f, b = 0.5f;
    switch (item->rarity) {
        case ITEM_RARITY_COMMON: r=0.8f; g=0.8f; b=0.8f; break;
        case ITEM_RARITY_UNCOMMON: r=0.2f; g=1.0f; b=0.2f; break;
        case ITEM_RARITY_RARE: r=0.3f; g=0.5f; b=1.0f; break;
        case ITEM_RARITY_EPIC: r=0.8f; g=0.3f; b=1.0f; break;
        case ITEM_RARITY_LEGENDARY: r=1.0f; g=0.6f; b=0.0f; break;
    }
    
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
}

void character_screen_render(const CharacterScreenState* char_screen, const GameState* game) {
    if (!char_screen->is_open) return;
    
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0, char_screen->screen_width, char_screen->screen_height, 0, -1, 1);
    
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    
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
    renderer_draw_text(char_screen->window_x + 10, char_screen->window_y + 20, "Character");
    
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
    
    // Character display area (center)
    renderer_draw_rect(char_screen->char_display_x, char_screen->char_display_y,
                      char_screen->char_display_width, char_screen->char_display_height,
                      0.2f, 0.15f, 0.25f, 0.8f);
    renderer_draw_text(char_screen->char_display_x + 30, 
                      char_screen->char_display_y + char_screen->char_display_height / 2,
                      "Character\nPlaceholder");
    
    // Section labels
    renderer_draw_text(char_screen->window_x + 10, char_screen->window_y + 60, "Weapons");
    renderer_draw_text(char_screen->window_x + char_screen->window_width - 70, 
                      char_screen->window_y + 60, "Armor");
    
    // Render equipment slots
    if (game->player.info_loaded) {
        render_equipment_slot(&char_screen->main_hand_slot, 
                             char_screen->hovered_slot == SLOT_MAIN_HAND,
                             game->player.info.main_hand, "Main");
        
        render_equipment_slot(&char_screen->off_hand_slot,
                             char_screen->hovered_slot == SLOT_OFF_HAND,
                             game->player.info.second_hand, "Off");
        
        render_equipment_slot(&char_screen->helmet_slot,
                             char_screen->hovered_slot == SLOT_HELMET,
                             game->player.info.helmet, "Head");
        
        render_equipment_slot(&char_screen->chest_slot,
                             char_screen->hovered_slot == SLOT_CHEST,
                             game->player.info.chest_armor, "Chest");
        
        render_equipment_slot(&char_screen->gloves_slot,
                             char_screen->hovered_slot == SLOT_GLOVES,
                             game->player.info.gloves, "Hands");
        
        render_equipment_slot(&char_screen->leggings_slot,
                             char_screen->hovered_slot == SLOT_LEGGINGS,
                             game->player.info.leggings, "Legs");
        
        render_equipment_slot(&char_screen->boots_slot,
                             char_screen->hovered_slot == SLOT_BOOTS,
                             game->player.info.boots, "Feet");
        
        // NEW: Render blessing slot
        render_equipment_slot(&char_screen->blessing_slot,
                             char_screen->hovered_slot == SLOT_BLESSING,
                             game->player.info.blessing, "Blessing");
    }
    
    // Render tooltip
    render_tooltip(char_screen, game);
    
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
}