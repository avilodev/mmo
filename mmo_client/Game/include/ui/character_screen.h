#ifndef CHARACTER_SCREEN_H
#define CHARACTER_SCREEN_H

#include "core/game_types.h"

/** Store one square equipment slot in screen coordinates. */
typedef struct {
    float x, y;
    float size;
} EquipSlotUI;

/** Track character-equipment window layout and interaction state. */
struct CharacterScreenState {
    int is_open;
    float window_x;
    float window_y;
    float window_width;
    float window_height;
    
    int is_dragging_window;
    float drag_offset_x;
    float drag_offset_y;
    
    float screen_width;
    float screen_height;
    
    EquipSlotUI main_hand_slot;
    EquipSlotUI off_hand_slot;
    
    EquipSlotUI helmet_slot;
    EquipSlotUI chest_slot;
    EquipSlotUI gloves_slot;
    EquipSlotUI leggings_slot;
    EquipSlotUI boots_slot;
    EquipSlotUI blessing_slot;
    
    float char_display_x;
    float char_display_y;
    float char_display_width;
    float char_display_height;
    
    float close_button_x;
    float close_button_y;
    float close_button_size;
    
    int hovered_slot;  /**< Equipment slot index from 0 through 7, or -1. */
    int tooltip_visible;
    float tooltip_x;
    float tooltip_y;
    
    float slot_size;
    float slot_padding;
};

void character_screen_init(CharacterScreenState* char_screen, float screen_width, float screen_height);

void character_screen_toggle(CharacterScreenState* char_screen);

void character_screen_update(CharacterScreenState* char_screen, float mouse_x, float mouse_y,
                            int mouse_clicked, int mouse_down, int right_clicked);

void character_screen_render(const CharacterScreenState* char_screen, const GameState* game);

int character_screen_check_close_button(const CharacterScreenState* char_screen, 
                                       float mouse_x, float mouse_y);

#endif // CHARACTER_SCREEN_H
