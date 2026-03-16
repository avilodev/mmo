#ifndef KEYBINDS_H
#define KEYBINDS_H

// ============================================================================
// KEYBINDS
// All gameplay key bindings. Loaded from Game/data/keybinds.cfg at startup.
// Edit that file to remap any key. If the file is missing, defaults are used.
// ============================================================================

typedef struct {
    // Movement
    int move_up;
    int move_down;
    int move_left;
    int move_right;

    // UI toggles
    int toggle_inventory;
    int toggle_character;
    int toggle_quest_log;
    int party_leave;

    // Combat
    int basic_attack;
    int ability[5];         // ability[0..4] = slots 1..5
} KeyBinds;

// Global keybind table — set by keybinds_load() or keybinds_defaults().
extern KeyBinds g_keybinds;

// Fill g_keybinds with factory defaults (WASD, 1-5, etc.).
void keybinds_defaults(void);

// Load from a key=value file. Falls back to defaults for missing entries.
void keybinds_load(const char* path);

// Write current bindings back to file.
void keybinds_save(const char* path);

#endif // KEYBINDS_H
