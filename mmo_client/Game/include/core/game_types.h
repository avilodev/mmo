#ifndef GAME_TYPES_H
#define GAME_TYPES_H

#include <stdint.h>
#include <GLFW/glfw3.h>

#include "protocol.h"
#include "ability_bar.h"
#include "camera.h"
#include "combat_state.h"
// world.h moved below - needs GameTextures to be defined first

// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================
typedef struct GameState GameState;
typedef struct CharacterScreenState CharacterScreenState;
typedef struct WorldState WorldState;  // Fully defined in world.h (included below)

// ============================================================================
// CONSTANTS
// ============================================================================
#define MAX_VISIBLE_NPCS    64
#define MAX_DAMAGE_NUMBERS  10
#define MAX_WORLDS          10

#define INVENTORY_SIZE 150
#define INVENTORY_COLS 10
#define INVENTORY_ROWS 15

// ============================================================================
// ENUMS
// ============================================================================

typedef enum {
    GAME_MODE_MAIN_MENU,
    GAME_MODE_SERVER_LIST,
    GAME_MODE_CHARACTER_SELECT,
    GAME_MODE_PLAYING,
    GAME_MODE_PAUSED,
    GAME_MODE_SETTINGS
} GameMode;

typedef enum {
    NET_STATE_IDLE,
    NET_STATE_WAITING_FOR_WORLDS,
    NET_STATE_WAITING_FOR_CHARACTERS,
    NET_STATE_CREATING_CHARACTER,
    NET_STATE_WAITING_FOR_ENTER_WORLD,
    NET_STATE_WAITING_FOR_CHARACTER_DATA
} NetworkState;

// ============================================================================
// ITEM TYPES (for inventory)
// ============================================================================

typedef enum {
    ITEM_TYPE_NONE = 0,
    ITEM_TYPE_CONSUMABLE,
    ITEM_TYPE_EQUIPMENT,
    ITEM_TYPE_QUEST,
    ITEM_TYPE_CURRENCY,
    ITEM_TYPE_MATERIAL,
    ITEM_TYPE_KEY
} ItemType;

typedef enum {
    EQUIP_SLOT_NONE = 0,
    EQUIP_SLOT_HELMET,
    EQUIP_SLOT_CHEST,
    EQUIP_SLOT_GLOVES,
    EQUIP_SLOT_LEGGINGS,
    EQUIP_SLOT_BOOTS,
    EQUIP_SLOT_MAIN_HAND,
    EQUIP_SLOT_SECOND_HAND,
    EQUIP_SLOT_COUNT
} EquipSlot;

typedef enum {
    ITEM_RARITY_COMMON = 0,
    ITEM_RARITY_UNCOMMON,
    ITEM_RARITY_RARE,
    ITEM_RARITY_EPIC,
    ITEM_RARITY_LEGENDARY
} ItemRarity;

typedef struct {
    uint32_t id;
    char name[32];
    char description[128];
    ItemType type;
    ItemRarity rarity;
    uint16_t max_stack;
    int16_t hp_restore;
    int16_t mp_restore;
    EquipSlot equip_slot;
    int16_t damage;
    int16_t defense;
    int16_t strength;
    int16_t dexterity;
    int16_t intelligence;
    uint32_t vendor_price;
    uint16_t sprite_id;
} ItemTemplate;

typedef struct {
    uint32_t template_id;
    uint16_t quantity;
} ItemSlot;

// ============================================================================
// INVENTORY STATE (FULL DEFINITION)
// ============================================================================

typedef struct {
    ItemSlot slots[INVENTORY_SIZE];
    int is_open;
    int hovered_slot;
    int selected_slot;
    float tooltip_x;
    float tooltip_y;
    int tooltip_visible;
    float window_x;
    float window_y;
    float window_width;
    float window_height;
    float slot_size;
    float slot_padding;

    float screen_width; 
    float screen_height;

    int is_dragging_window;   
    float drag_offset_x;     
    float drag_offset_y;
    
    // Close button
    float close_button_x;
    float close_button_y;
    float close_button_size;
} InventoryState;

// ============================================================================
// INPUT STATE
// ============================================================================

typedef struct {
    float mouse_x;
    float mouse_y;
    int mouse_left_clicked;      // Just pressed this frame
    int mouse_left_down;          // NEW: Held down
    int mouse_right_clicked;
    int keys_pressed[GLFW_KEY_LAST + 1];
    int keys_just_pressed[GLFW_KEY_LAST + 1];
} InputState;

// ============================================================================
// PLAYER DATA
// ============================================================================

typedef struct {
    float x;
    float y;
    float vel_x;
    float vel_y;
    float speed;
    int needs_position_reset;
    CharacterInfo info;
    int info_loaded;
} PlayerState;

// ============================================================================
// VISIBLE NPC
// ============================================================================

typedef struct {
    uint32_t npc_id;
    float pos_x;
    float pos_y;
    uint32_t health;
    uint32_t max_health;
    uint8_t is_alive;
    char name[32];
} VisibleNPC;

// ============================================================================
// WORLD STATE - REMOVED! Now defined in world.h
// ============================================================================
// OLD (REMOVED):
// typedef struct {
//     int* tiles;
//     uint8_t* collision;
//     int width;
//     int height;
//     int tile_size;
// } WorldState;
//
// NEW: WorldState is forward-declared above and fully defined in world.h

// ============================================================================
// MENU STATE
// ============================================================================

typedef struct {
    int selected_button;
    int hovered_button;
    float animation_time;
} MenuState;

// ============================================================================
// SERVER LIST STATE
// ============================================================================

typedef struct {
    WorldListResponsePacket list;
    int loaded;
    int selected_index;
    int hovered_index;
} ServerListState;

// ============================================================================
// CHARACTER SELECT STATE
// ============================================================================

typedef struct {
    CharacterListResponsePacket list;
    int loaded;
    int selected_index;
    int hovered_index;
    int show_creation;
    char new_name[32];
    int selected_class;
    int selected_race;
    char error_message[128];
    int pending_create;
} CharacterSelectState;

// ============================================================================
// TEXTURES
// ============================================================================

typedef struct {
    unsigned int player;
    unsigned int grass;
    unsigned int water;
    unsigned int rock;
    unsigned int background;
    unsigned int tree1;  
    unsigned int shrub1;  
} GameTextures;

// Include world.h here - it needs Camera (already defined) and GameTextures (just defined)
#include "world.h"

// ============================================================================
// HUD LAYOUT (FULL DEFINITION - Don't redefine in hud.h)
// ============================================================================

typedef struct {
    int screen_width;
    int screen_height;
    float minimap_x;
    float minimap_y;
    float minimap_size;
    float health_bar_x;
    float health_bar_y;
    float health_bar_width;
    float health_bar_height;
    float mana_bar_x;
    float mana_bar_y;
    float mana_bar_width;
    float mana_bar_height;
    float exp_bar_x;
    float exp_bar_y;
    float exp_bar_width;
    float exp_bar_height;
    float level_x;
    float level_y;
    float inv_button_x;
    float inv_button_y;
    float inv_button_size;
    // Character button (bottom right, left of inventory)
    float char_button_x;
    float char_button_y;
    float char_button_size;
    float currency_x;
    float currency_y;
    float currency_spacing;
} HUDLayout;

// ============================================================================
// MAIN GAME STATE
// ============================================================================

struct GameState {
    int is_running;
    GameMode mode;
    NetworkState net_state;
    InputState input;
    PlayerState player;
    WorldState world;           // This is now the chunked WorldState from world.h
    Camera camera;
    CombatState combat;
    MenuState main_menu;
    ServerListState server_list;
    CharacterSelectState char_select;
    AbilityBarState ability_bar;

    VisibleNPC visible_npcs[MAX_VISIBLE_NPCS];
    int visible_npc_count;

    GameTextures textures;
    int background_width;
    int background_height;

    HUDLayout hud;
    InventoryState* inventory;
    CharacterScreenState* character_screen;

    char session_key[65];
    uint32_t account_id;
    char username[32];
    char realm_ip[16];
    int realm_port;
    int network_connected;
    char network_status[128];

    // --- Player Stats (from server) ---
    int32_t     player_strength;
    int32_t     player_agility;
    int32_t     player_intelligence;
    int32_t     player_wisdom;
    int32_t     player_defense;
    int32_t     player_evasion;
    float       player_move_speed;      // Server-authoritative speed
    uint64_t    player_xp_for_next;     // XP needed for next level

    // --- Level-up notification ---
    int         show_level_up;          // 1 = show notification
    float       level_up_timer;         // Auto-dismiss timer
    int         level_up_new_level;     // The level reached

    EnterWorldResponsePacket enter_world_response;
};

#endif // GAME_TYPES_H