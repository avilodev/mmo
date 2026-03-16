#ifndef GAME_TYPES_H
#define GAME_TYPES_H

#include <stdint.h>
#include <GLFW/glfw3.h>

#include "protocol.h"
#include "ability_bar.h"
#include "camera.h"
#include "combat_state.h"
#include "ui/quest_log.h"
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
#define MAX_VISIBLE_NPCS        64
#define MAX_DAMAGE_NUMBERS      10
#define MAX_REWARD_POPUPS       5
#define MAX_WORLDS              10
#define MAX_VISIBLE_PROJECTILES 64
#define MAX_TELEGRAPHS          16
#define MAX_ZONES               32
#define MAX_GROUND_ITEMS        64
#define MAX_CHAT_LINES          50
#define MAX_CHAT_INPUT_LEN      256

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
    float target_x;          // Server target position for interpolation
    float target_y;
    float prev_x;            // Previous position for interpolation
    float prev_y;
    float interp_t;          // Interpolation progress (0.0 to 1.0)
    uint32_t health;
    uint32_t max_health;
    uint8_t is_alive;
    uint8_t category;        // 0=passive, 1=hostile, 2=quest
    uint8_t is_interactable; // 1 if player can interact (talk)
    uint8_t npc_type_id;     // NPC type for client display
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
// NEARBY PLAYERS
// ============================================================================

typedef struct {
    uint32_t player_id;
    float pos_x, pos_y;
    int32_t health, max_health;
    uint8_t player_class;
    uint8_t is_dead;
    char name[32];
} NearbyPlayer;

// ============================================================================
// PROJECTILES
// ============================================================================

typedef struct {
    uint32_t id;
    float pos_x, pos_y;
    float dir_x, dir_y;
    float speed;
    uint8_t active;
} VisibleProjectile;

// ============================================================================
// NPC TELEGRAPHS
// ============================================================================

typedef struct {
    uint32_t npc_id;
    uint8_t shape;          // 0=circle, 1=cone, 2=rectangle, 3=line
    float pos_x, pos_y;
    float dir_x, dir_y;
    float radius, angle, width, length;
    float cast_time;
    float elapsed;
    uint8_t active;
} VisibleTelegraph;

// ============================================================================
// ZONES (AoE areas)
// ============================================================================

typedef struct {
    uint32_t zone_id;
    float pos_x, pos_y;
    float radius;
    float duration;
    float elapsed;
    uint8_t active;
} VisibleZone;

// ============================================================================
// GROUND LOOT
// ============================================================================

typedef struct {
    uint32_t ground_item_id;
    uint32_t item_id;
    uint8_t quantity;
    float pos_x, pos_y;
    uint8_t active;
} GroundItem;

// ============================================================================
// CHAT STATE
// ============================================================================

typedef struct {
    char sender[32];
    char text[256];
    uint8_t channel;
} ChatLine;

typedef struct {
    ChatLine lines[MAX_CHAT_LINES];
    int line_count;
    int is_typing;
    char input_buf[MAX_CHAT_INPUT_LEN];
    int input_len;
    uint8_t active_channel;
    float backspace_timer;
    int   backspace_first;
} ChatState;

// ============================================================================
// PARTY STATE
// ============================================================================

typedef struct {
    uint32_t id;
    char name[32];
    uint8_t level;
    uint8_t player_class;
    int32_t health, max_health;
    int32_t mana, max_mana;
} PartyMember;

typedef struct {
    uint32_t party_id;
    uint32_t leader_id;
    uint8_t member_count;
    PartyMember members[MAX_PARTY_SIZE];
    int has_party;
    // Pending invite
    int has_pending_invite;
    uint32_t invite_from_id;
    char invite_from_name[32];
    float invite_timer;
} PartyState;

// ============================================================================
// SETTINGS
// ============================================================================

typedef struct {
    float master_volume;   // 0.0 - 1.0
    float music_volume;    // 0.0 - 1.0
    float sfx_volume;      // 0.0 - 1.0
    int   show_fps;        // 0 = off, 1 = on
} GameSettings;

// ============================================================================
// REWARD NOTIFICATIONS
// ============================================================================

typedef struct {
    uint32_t xp_gained;
    uint32_t gold_gained;
    float age;              // Seconds since notification
    int active;
} RewardNotification;

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
    int32_t     player_vitality;
    int32_t     player_luck;
    float       player_move_speed;      // Server-authoritative speed
    int32_t     player_weapon_damage;
    uint64_t    player_xp_for_next;     // XP needed for next level

    // --- Level-up notification ---
    int         show_level_up;          // 1 = show notification
    float       level_up_timer;         // Auto-dismiss timer
    int         level_up_new_level;     // The level reached

    // --- Death state ---
    int         is_dead;                // 1 = player is dead
    float       death_timer;            // Time since death (for UI)

    // --- Pause state ---
    int         is_paused;              // 1 = game is paused (overlay shown)

    EnterWorldResponsePacket enter_world_response;

    // --- Nearby Players ---
    NearbyPlayer nearby_players[MAX_NEARBY_PLAYERS];
    int nearby_player_count;

    // --- Projectiles ---
    VisibleProjectile projectiles[MAX_VISIBLE_PROJECTILES];
    int projectile_count;

    // --- NPC Telegraphs ---
    VisibleTelegraph telegraphs[MAX_TELEGRAPHS];

    // --- Zones ---
    VisibleZone zones[MAX_ZONES];

    // --- Ground Loot ---
    GroundItem ground_items[MAX_GROUND_ITEMS];

    // --- Chat ---
    ChatState chat;

    // --- Party ---
    PartyState party;

    // --- Reward Notifications ---
    RewardNotification reward_notifications[MAX_REWARD_POPUPS];

    // --- Settings ---
    GameSettings settings;
    int          show_settings;  // 1 = in-game settings overlay visible

    // --- Quest Log ---
    QuestLogState quest_log;
};

#endif // GAME_TYPES_H