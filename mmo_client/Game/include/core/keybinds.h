#ifndef KEYBINDS_H
#define KEYBINDS_H

/**
 * @file
 * Declare configurable gameplay key bindings loaded from Game/data/player_keybinds.cfg.
 */

#include <GLFW/glfw3.h>

// Mouse buttons are bound like keys, with codes after the last GLFW key
#define KEYBIND_MOUSE_BASE   (GLFW_KEY_LAST + 1)
#define KEYBIND_MOUSE_LEFT   (KEYBIND_MOUSE_BASE + GLFW_MOUSE_BUTTON_LEFT)
#define KEYBIND_MOUSE_RIGHT  (KEYBIND_MOUSE_BASE + GLFW_MOUSE_BUTTON_RIGHT)
#define KEYBIND_MOUSE_MIDDLE (KEYBIND_MOUSE_BASE + GLFW_MOUSE_BUTTON_MIDDLE)

/** Store GLFW key codes for movement, UI, party, and combat actions. */
typedef struct {
    int move_up;
    int move_down; 
    int move_left;
    int move_right;
    int toggle_walk;        /**< Switch between walking and running (Ctrl). */
	int sprint;             // Sprint, held or toggled per the settings (Left Shift)
	int interact;           // Talk to or use what is under the cursor (left click)

    int toggle_chat;         /**< Open the chat input (Default T). */
    int toggle_channel;       /**< Cycle chat channels (Default Tab). */

    int toggle_inventory;
    int toggle_character;
	int inspect_target; // Inspect the targeted player (K)
    int toggle_quest_log;
    int toggle_currency;
    int toggle_friends;
    int party_leave;
	int toggle_session_panel; // Session panel (O)
	int toggle_map;          // Full map (M)
	int toggle_emote_hub;    // The emote hub (H)
	int toggle_weather;      // The weather forecast (Y)
	int emote[3];            // Emotes 1 to 3 (Z X V)

	int basic_attack;       // Right click
    int swap_form;          /**< Toggle between Human and Animal Form. */
    int ability[12];         /**< The twelve hotbar slots, whichever form fills them. */

    int camera_rotate_left;   /**< Held: turn the camera one way (Q). */
    int camera_rotate_right;  /**< Held: turn it the other (E). */
    int camera_reset;         /**< Back to north-up, default zoom (Home). */
} KeyBinds;

extern KeyBinds g_keybinds;

void keybinds_defaults(void);

/** Load the player's bindings, first copying them from the default file. */
void keybinds_load(const char* default_path, const char* player_path);

// Write the current bindings to the player file
void keybinds_save(void);

// One rebindable action, as the controls menu lists it
typedef struct {
	const char* id;      // its name in the cfg files
	const char* label;   // shown in the menu
	const char* group;   // the heading it is listed under
} keybind_action;

int keybinds_action_count(void);
const keybind_action* keybinds_action_at(int index);
int keybinds_get(int index);
int keybinds_default_key(int index);

// Put every binding back to its default and save
void keybinds_reset_all(void);

// Bind an action to a key; any action holding that key gets the old one
void keybinds_set(int index, int key);

// Name of a key as the menu and the cfg files write it, or NULL if none
const char* keybinds_key_name(int key);

// The nth key a binding can use, or -1 past the last
int keybinds_nameable_key(int n);

#endif // KEYBINDS_H
