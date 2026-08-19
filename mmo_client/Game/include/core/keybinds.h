#ifndef KEYBINDS_H
#define KEYBINDS_H

/**
 * @file
 * Declare configurable gameplay key bindings loaded from Game/data/keybinds.cfg.
 */

/** Store GLFW key codes for movement, UI, party, and combat actions. */
typedef struct {
    int move_up;
    int move_down;
    int move_left;
    int move_right;

    int toggle_inventory;
    int toggle_character;
    int toggle_quest_log;
    int party_leave;

    int basic_attack;
    int swap_form;          /**< Toggle between Human and Animal Form. */
    int ability[5];         /**< The five hotbar slots, whichever form fills them. */
} KeyBinds;

extern KeyBinds g_keybinds;

void keybinds_defaults(void);

/** Load key-value bindings while retaining defaults for missing entries. */
void keybinds_load(const char* path);

void keybinds_save(const char* path);

#endif // KEYBINDS_H
