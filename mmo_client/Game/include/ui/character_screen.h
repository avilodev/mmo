#ifndef CHARACTER_SCREEN_H
#define CHARACTER_SCREEN_H

#include "core/game_types.h"

// Equipment slot positions
typedef struct {
    float x, y;
    float size;
} EquipSlotUI;

struct CharacterScreenState {
    // Window state
    int is_open;
    float window_x;
    float window_y;
    float window_width;
    float window_height;
    
    // Dragging state
    int is_dragging_window;
    float drag_offset_x;
    float drag_offset_y;
    
    // Screen dimensions
    float screen_width;
    float screen_height;
    
    // Equipment slots (left side - weapons)
    EquipSlotUI main_hand_slot;
    EquipSlotUI off_hand_slot;
    
    // Equipment slots (right side - armor)
    EquipSlotUI helmet_slot;
    EquipSlotUI chest_slot;
    EquipSlotUI gloves_slot;
    EquipSlotUI leggings_slot;
    EquipSlotUI boots_slot;
    EquipSlotUI blessing_slot;  // NEW: Blessing slot at bottom of armor
    
    // Character display area (center)
    float char_display_x;
    float char_display_y;
    float char_display_width;
    float char_display_height;
    
    // Close button
    float close_button_x;
    float close_button_y;
    float close_button_size;
    
    // Interaction state
    int hovered_slot;  // -1 = none, 0-7 = equipment slots (added blessing)
    int tooltip_visible;
    float tooltip_x;
    float tooltip_y;
    
    // Slot size
    float slot_size;
    float slot_padding;
};

// Initialize character screen
void character_screen_init(CharacterScreenState* char_screen, float screen_width, float screen_height);

// Toggle character screen visibility
void character_screen_toggle(CharacterScreenState* char_screen);

// Update character screen (handle mouse input)
void character_screen_update(CharacterScreenState* char_screen, float mouse_x, float mouse_y,
                            int mouse_clicked, int mouse_down, int right_clicked);

// Render character screen
void character_screen_render(const CharacterScreenState* char_screen, const GameState* game);

// Check if close button was clicked
int character_screen_check_close_button(const CharacterScreenState* char_screen, 
                                       float mouse_x, float mouse_y);

#endif // CHARACTER_SCREEN_H