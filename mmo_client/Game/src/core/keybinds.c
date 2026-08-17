/**
 * @file
 * Map configurable key names to GLFW codes and persist client bindings.
 */

#include "core/keybinds.h"

#include <stdio.h>
#include <string.h>
#include <GLFW/glfw3.h>

KeyBinds g_keybinds;

/** Associate a configuration key name with its GLFW key code. */
typedef struct { const char* name; int key; } KeyEntry;

static const KeyEntry KEY_TABLE[] = {
    // Letters
    {"A", GLFW_KEY_A}, {"B", GLFW_KEY_B}, {"C", GLFW_KEY_C}, {"D", GLFW_KEY_D},
    {"E", GLFW_KEY_E}, {"F", GLFW_KEY_F}, {"G", GLFW_KEY_G}, {"H", GLFW_KEY_H},
    {"I", GLFW_KEY_I}, {"J", GLFW_KEY_J}, {"K", GLFW_KEY_K}, {"L", GLFW_KEY_L},
    {"M", GLFW_KEY_M}, {"N", GLFW_KEY_N}, {"O", GLFW_KEY_O}, {"P", GLFW_KEY_P},
    {"Q", GLFW_KEY_Q}, {"R", GLFW_KEY_R}, {"S", GLFW_KEY_S}, {"T", GLFW_KEY_T},
    {"U", GLFW_KEY_U}, {"V", GLFW_KEY_V}, {"W", GLFW_KEY_W}, {"X", GLFW_KEY_X},
    {"Y", GLFW_KEY_Y}, {"Z", GLFW_KEY_Z},
    // Digits (top row)
    {"0", GLFW_KEY_0}, {"1", GLFW_KEY_1}, {"2", GLFW_KEY_2}, {"3", GLFW_KEY_3},
    {"4", GLFW_KEY_4}, {"5", GLFW_KEY_5}, {"6", GLFW_KEY_6}, {"7", GLFW_KEY_7},
    {"8", GLFW_KEY_8}, {"9", GLFW_KEY_9},
    // Function keys
    {"F1",  GLFW_KEY_F1},  {"F2",  GLFW_KEY_F2},  {"F3",  GLFW_KEY_F3},
    {"F4",  GLFW_KEY_F4},  {"F5",  GLFW_KEY_F5},  {"F6",  GLFW_KEY_F6},
    {"F7",  GLFW_KEY_F7},  {"F8",  GLFW_KEY_F8},  {"F9",  GLFW_KEY_F9},
    {"F10", GLFW_KEY_F10}, {"F11", GLFW_KEY_F11}, {"F12", GLFW_KEY_F12},
    // Special keys
    {"SPACE",     GLFW_KEY_SPACE},
    {"ENTER",     GLFW_KEY_ENTER},
    {"TAB",       GLFW_KEY_TAB},
    {"BACKSPACE", GLFW_KEY_BACKSPACE},
    {"ESCAPE",    GLFW_KEY_ESCAPE},
    {"LEFT",      GLFW_KEY_LEFT},
    {"RIGHT",     GLFW_KEY_RIGHT},
    {"UP",        GLFW_KEY_UP},
    {"DOWN",      GLFW_KEY_DOWN},
    {"LSHIFT",    GLFW_KEY_LEFT_SHIFT},
    {"RSHIFT",    GLFW_KEY_RIGHT_SHIFT},
    {"LCTRL",     GLFW_KEY_LEFT_CONTROL},
    {"RCTRL",     GLFW_KEY_RIGHT_CONTROL},
    {"LALT",      GLFW_KEY_LEFT_ALT},
    {"RALT",      GLFW_KEY_RIGHT_ALT},
    {"KP0", GLFW_KEY_KP_0}, {"KP1", GLFW_KEY_KP_1}, {"KP2", GLFW_KEY_KP_2},
    {"KP3", GLFW_KEY_KP_3}, {"KP4", GLFW_KEY_KP_4}, {"KP5", GLFW_KEY_KP_5},
    {"KP6", GLFW_KEY_KP_6}, {"KP7", GLFW_KEY_KP_7}, {"KP8", GLFW_KEY_KP_8},
    {"KP9", GLFW_KEY_KP_9},
    {NULL, 0}
};

static int key_from_name(const char* name) {
    for (int i = 0; KEY_TABLE[i].name; i++) {
        if (strcmp(KEY_TABLE[i].name, name) == 0)
            return KEY_TABLE[i].key;
    }
    return GLFW_KEY_UNKNOWN;
}

static const char* name_from_key(int key) {
    for (int i = 0; KEY_TABLE[i].name; i++) {
        if (KEY_TABLE[i].key == key)
            return KEY_TABLE[i].name;
    }
    return "UNKNOWN";
}

/**
 * Restore all client key bindings to factory defaults.
 */
void keybinds_defaults(void) {
    g_keybinds.move_up          = GLFW_KEY_W;
    g_keybinds.move_down        = GLFW_KEY_S;
    g_keybinds.move_left        = GLFW_KEY_A;
    g_keybinds.move_right       = GLFW_KEY_D;
    g_keybinds.toggle_inventory = GLFW_KEY_I;
    g_keybinds.toggle_character = GLFW_KEY_C;
    g_keybinds.toggle_quest_log = GLFW_KEY_J;
    g_keybinds.party_leave      = GLFW_KEY_P;
    g_keybinds.basic_attack     = GLFW_KEY_SPACE;
    g_keybinds.ability[0]       = GLFW_KEY_1;
    g_keybinds.ability[1]       = GLFW_KEY_2;
    g_keybinds.ability[2]       = GLFW_KEY_3;
    g_keybinds.ability[3]       = GLFW_KEY_4;
    g_keybinds.ability[4]       = GLFW_KEY_5;
}

/**
 * Load recognized key bindings from a text configuration file.
 *
 * Defaults are installed first, so missing files and entries retain default bindings.
 *
 * @param path  Configuration file path to read.
 */
void keybinds_load(const char* path) {
    keybinds_defaults();

    FILE* f = fopen(path, "r");
    if (!f) {
        printf("[KEYBINDS] %s not found, using defaults\n", path);
        return;
    }

    char line[128];
    while (fgets(line, sizeof(line), f)) {
        // Strip trailing newline
        char* nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        // Skip comments and blank lines
        if (line[0] == '#' || line[0] == '\0') continue;

        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char* key_str  = line;
        const char* val_str  = eq + 1;

        int key = key_from_name(val_str);
        if (key == GLFW_KEY_UNKNOWN) {
            printf("[KEYBINDS] Unknown key name '%s' for binding '%s'\n", val_str, key_str);
            continue;
        }

        if      (strcmp(key_str, "move_up")          == 0) g_keybinds.move_up          = key;
        else if (strcmp(key_str, "move_down")        == 0) g_keybinds.move_down        = key;
        else if (strcmp(key_str, "move_left")        == 0) g_keybinds.move_left        = key;
        else if (strcmp(key_str, "move_right")       == 0) g_keybinds.move_right       = key;
        else if (strcmp(key_str, "toggle_inventory") == 0) g_keybinds.toggle_inventory = key;
        else if (strcmp(key_str, "toggle_character") == 0) g_keybinds.toggle_character = key;
        else if (strcmp(key_str, "toggle_quest_log") == 0) g_keybinds.toggle_quest_log = key;
        else if (strcmp(key_str, "party_leave")      == 0) g_keybinds.party_leave      = key;
        else if (strcmp(key_str, "basic_attack")     == 0) g_keybinds.basic_attack     = key;
        else if (strcmp(key_str, "ability_1")        == 0) g_keybinds.ability[0]       = key;
        else if (strcmp(key_str, "ability_2")        == 0) g_keybinds.ability[1]       = key;
        else if (strcmp(key_str, "ability_3")        == 0) g_keybinds.ability[2]       = key;
        else if (strcmp(key_str, "ability_4")        == 0) g_keybinds.ability[3]       = key;
        else if (strcmp(key_str, "ability_5")        == 0) g_keybinds.ability[4]       = key;
    }

    fclose(f);
    printf("[KEYBINDS] Loaded from %s\n", path);
}

/**
 * Write current key bindings to a text configuration file.
 *
 * @param path  Destination file path, which is replaced when writable.
 */
void keybinds_save(const char* path) {
    FILE* f = fopen(path, "w");
    if (!f) {
        printf("[KEYBINDS] Could not write %s\n", path);
        return;
    }

    fprintf(f, "# Game keybinds — edit and restart to apply\n");
    fprintf(f, "# Valid key names: A-Z, 0-9, F1-F12, SPACE, ENTER, TAB, ESCAPE,\n");
    fprintf(f, "#   LEFT, RIGHT, UP, DOWN, LSHIFT, RSHIFT, LCTRL, RCTRL, LALT, RALT, KP0-KP9\n\n");
    fprintf(f, "move_up=%s\n",          name_from_key(g_keybinds.move_up));
    fprintf(f, "move_down=%s\n",        name_from_key(g_keybinds.move_down));
    fprintf(f, "move_left=%s\n",        name_from_key(g_keybinds.move_left));
    fprintf(f, "move_right=%s\n",       name_from_key(g_keybinds.move_right));
    fprintf(f, "toggle_inventory=%s\n", name_from_key(g_keybinds.toggle_inventory));
    fprintf(f, "toggle_character=%s\n", name_from_key(g_keybinds.toggle_character));
    fprintf(f, "toggle_quest_log=%s\n", name_from_key(g_keybinds.toggle_quest_log));
    fprintf(f, "party_leave=%s\n",      name_from_key(g_keybinds.party_leave));
    fprintf(f, "basic_attack=%s\n",     name_from_key(g_keybinds.basic_attack));
    fprintf(f, "ability_1=%s\n",        name_from_key(g_keybinds.ability[0]));
    fprintf(f, "ability_2=%s\n",        name_from_key(g_keybinds.ability[1]));
    fprintf(f, "ability_3=%s\n",        name_from_key(g_keybinds.ability[2]));
    fprintf(f, "ability_4=%s\n",        name_from_key(g_keybinds.ability[3]));
    fprintf(f, "ability_5=%s\n",        name_from_key(g_keybinds.ability[4]));

    fclose(f);
    printf("[KEYBINDS] Saved to %s\n", path);
}
