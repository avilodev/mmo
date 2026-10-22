/**
 * @file
 * Map configurable key names to GLFW codes and persist client bindings.
 */

#include "core/keybinds.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <GLFW/glfw3.h>
#include "core/client_log.h"

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
    {"HOME",      GLFW_KEY_HOME},
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
	// Keypad operators
	{"KP_DECIMAL", GLFW_KEY_KP_DECIMAL}, {"KP_DIVIDE", GLFW_KEY_KP_DIVIDE},
	{"KP_MULTIPLY", GLFW_KEY_KP_MULTIPLY}, {"KP_SUBTRACT", GLFW_KEY_KP_SUBTRACT},
	{"KP_ADD", GLFW_KEY_KP_ADD}, {"KP_ENTER", GLFW_KEY_KP_ENTER},
	// Punctuation
	{"MINUS", GLFW_KEY_MINUS}, {"EQUAL", GLFW_KEY_EQUAL},
	{"COMMA", GLFW_KEY_COMMA}, {"PERIOD", GLFW_KEY_PERIOD},
	{"SEMICOLON", GLFW_KEY_SEMICOLON}, {"APOSTROPHE", GLFW_KEY_APOSTROPHE},
	{"LBRACKET", GLFW_KEY_LEFT_BRACKET}, {"RBRACKET", GLFW_KEY_RIGHT_BRACKET},
	{"BACKSLASH", GLFW_KEY_BACKSLASH}, {"SLASH", GLFW_KEY_SLASH},
	{"GRAVE", GLFW_KEY_GRAVE_ACCENT},
	// Navigation and editing
	{"PAGEUP", GLFW_KEY_PAGE_UP}, {"PAGEDOWN", GLFW_KEY_PAGE_DOWN},
	{"INSERT", GLFW_KEY_INSERT}, {"DELETE", GLFW_KEY_DELETE},
	{"END", GLFW_KEY_END}, {"CAPSLOCK", GLFW_KEY_CAPS_LOCK},
	// Mouse buttons
	{"MOUSE1", KEYBIND_MOUSE_LEFT}, {"MOUSE2", KEYBIND_MOUSE_RIGHT},
	{"MOUSE3", KEYBIND_MOUSE_MIDDLE}, {"MOUSE4", KEYBIND_MOUSE_BASE + GLFW_MOUSE_BUTTON_4},
	{"MOUSE5", KEYBIND_MOUSE_BASE + GLFW_MOUSE_BUTTON_5},
    {NULL, 0}
};

static int key_from_name(const char* name) {
    for (int i = 0; KEY_TABLE[i].name; i++) {
        if (strcmp(KEY_TABLE[i].name, name) == 0)
            return KEY_TABLE[i].key;
    }
    return GLFW_KEY_UNKNOWN; 
}

/**
 * Write the factory default bindings into a table.
 */
static void fill_defaults(KeyBinds* kb) {
    kb->move_up          = GLFW_KEY_W; 
    kb->move_down        = GLFW_KEY_S;
    kb->move_left        = GLFW_KEY_A;
    kb->move_right       = GLFW_KEY_D;
    kb->toggle_walk      = GLFW_KEY_LEFT_CONTROL;
	kb->sprint = GLFW_KEY_LEFT_SHIFT;
	kb->interact = KEYBIND_MOUSE_LEFT;
	kb->toggle_chat = GLFW_KEY_T;
    kb->toggle_channel = GLFW_KEY_TAB;
    kb->toggle_inventory = GLFW_KEY_I;
    kb->toggle_character = GLFW_KEY_C;
	kb->inspect_target = GLFW_KEY_K;
    kb->toggle_quest_log = GLFW_KEY_J;
    kb->toggle_friends   = GLFW_KEY_U;
    kb->toggle_currency  = GLFW_KEY_G;
    kb->party_leave      = GLFW_KEY_P;
	kb->toggle_session_panel = GLFW_KEY_O;
	kb->toggle_map = GLFW_KEY_M;
	kb->toggle_emote_hub = GLFW_KEY_H;
	kb->toggle_weather = GLFW_KEY_Y;
	kb->emote[0] = GLFW_KEY_Z;
	kb->emote[1] = GLFW_KEY_X;
	kb->emote[2] = GLFW_KEY_V;
	kb->basic_attack = KEYBIND_MOUSE_RIGHT;
    kb->swap_form        = GLFW_KEY_F;
    kb->ability[0]       = GLFW_KEY_1;
    kb->ability[1]       = GLFW_KEY_2;
    kb->ability[2]       = GLFW_KEY_3;
    kb->ability[3]       = GLFW_KEY_4;
    kb->ability[4]       = GLFW_KEY_5;
    kb->ability[5]       = GLFW_KEY_6;
    kb->ability[6]       = GLFW_KEY_7;
    kb->ability[7]       = GLFW_KEY_8;
    kb->ability[8]       = GLFW_KEY_9;
    kb->ability[9]       = GLFW_KEY_0;
    kb->ability[10]       = GLFW_KEY_MINUS;
    kb->ability[11]       = GLFW_KEY_EQUAL;
    kb->camera_rotate_left  = GLFW_KEY_Q;
    kb->camera_rotate_right = GLFW_KEY_E;
    kb->camera_reset        = GLFW_KEY_HOME;
}

/** Restore all client key bindings to factory defaults. */
void keybinds_defaults(void) {
	fill_defaults(&g_keybinds);
}

typedef struct {
	keybind_action info;
	size_t offset;       // of the binding inside KeyBinds
} action_entry;

#define ACTION(field, id, label, group) { { id, label, group }, offsetof(KeyBinds, field) }

static const action_entry ACTION_TABLE[] = {
	ACTION(move_up,        "move_up",        "Move Up",       "Movement"),
	ACTION(move_down,      "move_down",      "Move Down",     "Movement"),
	ACTION(move_left,      "move_left",      "Move Left",     "Movement"),
	ACTION(move_right,     "move_right",     "Move Right",    "Movement"),
	ACTION(toggle_walk,    "toggle_walk",    "Toggle Walk",   "Movement"),
	ACTION(sprint,         "sprint",         "Sprint",        "Movement"),
	ACTION(toggle_chat,    "toggle_chat",    "Open Chat",     "Chat"),
	ACTION(toggle_channel, "toggle_channel", "Cycle Channel", "Chat"),
	ACTION(basic_attack,   "basic_attack",   "Basic Attack",  "Combat"),
	ACTION(swap_form,      "swap_form",      "Swap Form",     "Combat"),
	ACTION(ability[0],     "ability_1",      "Ability 1",     "Combat"),
	ACTION(ability[1],     "ability_2",      "Ability 2",     "Combat"),
	ACTION(ability[2],     "ability_3",      "Ability 3",     "Combat"),
	ACTION(ability[3],     "ability_4",      "Ability 4",     "Combat"),
	ACTION(ability[4],     "ability_5",      "Ability 5",     "Combat"),
	ACTION(ability[5],     "ability_6",      "Ability 6",     "Combat"),
	ACTION(ability[6],     "ability_7",      "Ability 7",     "Combat"),
	ACTION(ability[7],     "ability_8",      "Ability 8",     "Combat"),
	ACTION(ability[8],     "ability_9",      "Ability 9",     "Combat"),
	ACTION(ability[9],     "ability_10",     "Ability 10",    "Combat"),
	ACTION(ability[10],    "ability_11",     "Ability 11",    "Combat"),
	ACTION(ability[11],    "ability_12",     "Ability 12",    "Combat"),
	ACTION(interact,       "interact",       "Interact",      "Interface"),
	ACTION(toggle_inventory,     "toggle_inventory",     "Inventory",     "Interface"),
	ACTION(toggle_character,     "toggle_character",     "Character",     "Interface"),
	ACTION(inspect_target,       "inspect_target",       "Inspect Target", "Interface"),
	ACTION(toggle_quest_log,     "toggle_quest_log",     "Quest Log",     "Interface"),
	ACTION(toggle_friends,       "toggle_friends",       "Friends",       "Interface"),
	ACTION(toggle_currency,      "toggle_currency",      "Currency",      "Interface"),
	ACTION(party_leave,          "party_leave",          "Leave Party",   "Interface"),
	ACTION(toggle_session_panel, "toggle_session_panel", "Session Panel", "Interface"),
	ACTION(toggle_map,           "toggle_map",           "Map",           "Interface"),
	ACTION(toggle_emote_hub,     "toggle_emote_hub",     "Emote Hub",     "Interface"),
	ACTION(toggle_weather,       "toggle_weather",       "Weather",       "Interface"),
	ACTION(emote[0],             "emote_1",              "Emote 1",       "Emotes"),
	ACTION(emote[1],             "emote_2",              "Emote 2",       "Emotes"),
	ACTION(emote[2],             "emote_3",              "Emote 3",       "Emotes"),
	ACTION(camera_rotate_left,   "camera_rotate_left",   "Rotate Left",   "Camera"),
	ACTION(camera_rotate_right,  "camera_rotate_right",  "Rotate Right",  "Camera"),
	ACTION(camera_reset,         "camera_reset",         "Reset Camera",  "Camera"),
};

#define ACTION_COUNT ((int)(sizeof(ACTION_TABLE) / sizeof(ACTION_TABLE[0])))

// What the menu shows as default: the compiled ones, then default_keybinds.cfg
static KeyBinds s_defaults;
static int s_defaults_ready;
static char s_player_path[256];

static int* binding_at(KeyBinds* kb, int index) {
	return (int*)((char*)kb + ACTION_TABLE[index].offset);
}

int keybinds_action_count(void) {
	return ACTION_COUNT;
}

const keybind_action* keybinds_action_at(int index) {
	if(index < 0 || index >= ACTION_COUNT)
		return NULL;

	return &ACTION_TABLE[index].info;
}

int keybinds_get(int index) {
	if(index < 0 || index >= ACTION_COUNT)
		return GLFW_KEY_UNKNOWN;

	return *binding_at(&g_keybinds, index);
}

int keybinds_default_key(int index) {
	if(index < 0 || index >= ACTION_COUNT)
		return GLFW_KEY_UNKNOWN;

	if(!s_defaults_ready) {
		fill_defaults(&s_defaults);
		s_defaults_ready = 1;
	}

	return *binding_at(&s_defaults, index);
}

void keybinds_reset_all(void) {
	if(!s_defaults_ready) {
		fill_defaults(&s_defaults);
		s_defaults_ready = 1;
	}

	g_keybinds = s_defaults;
	keybinds_save();
}

void keybinds_set(int index, int key) {
	if(index < 0 || index >= ACTION_COUNT || !keybinds_key_name(key))
		return;

	int old_key = *binding_at(&g_keybinds, index);

	for(int i = 0; i < ACTION_COUNT; i++) {
		if(i != index && *binding_at(&g_keybinds, i) == key)
			*binding_at(&g_keybinds, i) = old_key;
	}

	*binding_at(&g_keybinds, index) = key;
	keybinds_save();
}

const char* keybinds_key_name(int key) {
	for(int i = 0; KEY_TABLE[i].name; i++) {
		if(KEY_TABLE[i].key == key)
			return KEY_TABLE[i].name;
	}

	return NULL;
}

int keybinds_nameable_key(int n) {
	if(n < 0)
		return -1;

	for(int i = 0; i <= n; i++) {
		if(!KEY_TABLE[i].name)
			return -1;
	}

	return KEY_TABLE[n].key;
}

/** Read name=KEY lines from a file into a table; false if it cannot be opened. */
static int parse_file(const char* path, KeyBinds* kb) {
	char line[128];
	FILE* f = fopen(path, "r");

	if(!f)
		return 0;

	while(fgets(line, sizeof(line), f)) {
		char* end = line + strcspn(line, "\r\n");
		char* eq = strchr(line, '=');
		int key;
		int i;

		*end = '\0';

		// Skip comments and blank lines
		if(line[0] == '#' || line[0] == '\0' || !eq)
			continue;

		*eq = '\0';
		key = key_from_name(eq + 1);

		if(key == GLFW_KEY_UNKNOWN) {
			CLOG_WARN("[KEYBINDS] Unknown key name '%s' for binding '%s'", eq + 1, line);
			continue;
		}

		for(i = 0; i < ACTION_COUNT; i++) {
			if(strcmp(line, ACTION_TABLE[i].info.id) == 0) {
				*binding_at(kb, i) = key;
				break;
			}
		}
	}

	fclose(f);

	return 1;
}

// Copy a file byte for byte; false if either end cannot be opened
static int copy_file(const char* from, const char* to) {
	char buf[1024];
	size_t n;
	FILE* in = fopen(from, "rb");
	FILE* out;

	if(!in)
		return 0;

	out = fopen(to, "wb");

	if(!out) {
		fclose(in);
		return 0;
	}

	while((n = fread(buf, 1, sizeof(buf), in)) > 0)
		fwrite(buf, 1, n, out);

	fclose(in);

	return fclose(out) == 0;
}

/**
 * Load the bindings.
 *
 * The default file is only ever read. The player file is made from a copy of
 * it the first time, and is the one the game reads and rewrites after that.
 *
 * @param default_path  Shipped defaults, never written.
 * @param player_path   The player's bindings, created from the defaults if absent.
 */
void keybinds_load(const char* default_path, const char* player_path) {
	FILE* probe;

	fill_defaults(&s_defaults);
	s_defaults_ready = 1;

	if(!parse_file(default_path, &s_defaults))
		CLOG_INFO("[KEYBINDS] %s not found, using built-in defaults", default_path);

	snprintf(s_player_path, sizeof(s_player_path), "%s", player_path);
	probe = fopen(player_path, "r");

	if(probe)
		fclose(probe);
	else if(copy_file(default_path, player_path))
		CLOG_INFO("[KEYBINDS] Created %s from %s", player_path, default_path);

	g_keybinds = s_defaults;
	parse_file(player_path, &g_keybinds);
	CLOG_INFO("[KEYBINDS] Loaded from %s", player_path);
}

/** Write the current bindings to the player file; the default file is untouched. */
void keybinds_save(void) {
	FILE* f;

	if(!s_player_path[0])
		return;

	f = fopen(s_player_path, "w");

	if(!f) {
		CLOG_WARN("[KEYBINDS] Could not write %s", s_player_path);
		return;
	}

	fprintf(f, "# Your key bindings, rewritten by the game. Defaults: default_keybinds.cfg\n");

	for(int i = 0; i < ACTION_COUNT; i++)
		fprintf(f, "%s=%s\n", ACTION_TABLE[i].info.id, keybinds_key_name(*binding_at(&g_keybinds, i)));

	if(fclose(f) != 0)
		CLOG_WARN("[KEYBINDS] Could not write %s", s_player_path);
}
