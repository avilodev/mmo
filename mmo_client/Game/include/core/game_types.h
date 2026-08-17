#ifndef GAME_TYPES_H
#define GAME_TYPES_H

#include <stdint.h>
#include <GLFW/glfw3.h>

#include "protocol.h"
#include "ability_bar.h"
#include "camera.h"
#include "combat_state.h"
#include "ui/quest_log.h"
typedef struct GameState GameState;
typedef struct CharacterScreenState CharacterScreenState;
typedef struct WorldState WorldState;

/** Bound fixed-capacity client presentation collections. */
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
#define MAX_HEAL_VFXS           8

#define INVENTORY_SIZE 150
#define INVENTORY_COLS 10
#define INVENTORY_ROWS 15

/** Select the client's active top-level screen. */
typedef enum {
    GAME_MODE_MAIN_MENU,
    GAME_MODE_SERVER_LIST,
    GAME_MODE_CHARACTER_SELECT,
    GAME_MODE_PLAYING,
    GAME_MODE_SETTINGS
} GameMode;

/** Track the outstanding realm or world request expected by the UI. */
typedef enum {
    NET_STATE_IDLE,
    NET_STATE_WAITING_FOR_WORLDS,
    NET_STATE_WAITING_FOR_CHARACTERS,
    NET_STATE_CREATING_CHARACTER,
    NET_STATE_DELETING_CHARACTER,
    NET_STATE_WAITING_FOR_ENTER_WORLD,
    NET_STATE_WAITING_FOR_CHARACTER_DATA
} NetworkState;

/** Maximum wait for one server response, in seconds. */
#define NET_REQUEST_TIMEOUT_SECONDS 10.0f

/** Classify inventory item behavior. */
typedef enum {
    ITEM_TYPE_NONE = 0,
    ITEM_TYPE_CONSUMABLE,
    ITEM_TYPE_EQUIPMENT,
    ITEM_TYPE_QUEST,
    ITEM_TYPE_CURRENCY,
    ITEM_TYPE_MATERIAL,
    ITEM_TYPE_KEY
} ItemType;

/** Identify client equipment array positions. */
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

/** Classify item rarity for presentation and fallback pricing. */
typedef enum {
    ITEM_RARITY_COMMON = 0,
    ITEM_RARITY_UNCOMMON,
    ITEM_RARITY_RARE,
    ITEM_RARITY_EPIC,
    ITEM_RARITY_LEGENDARY
} ItemRarity;

/** Describe immutable item metadata used by the client inventory. */
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

/** Store one inventory item instance and quantity. */
typedef struct {
    uint32_t template_id;
    uint16_t quantity;

    uint64_t instance_id;   /**< Server-assigned instance identifier, or 0 for an empty slot. */
    uint8_t  is_bound;      /**< Nonzero when the item cannot be traded. */
} ItemSlot;

/** Track inventory contents, interaction, tooltip, and window layout. */
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
    
    float close_button_x;
    float close_button_y;
    float close_button_size;
} InventoryState;

/** Capture mouse and keyboard state for one client frame. */
typedef struct {
    float mouse_x;
    float mouse_y;
    int mouse_left_clicked;      /**< Nonzero only on the press frame. */
    int mouse_left_down;         /**< Nonzero while held. */
    int mouse_right_clicked;
    int keys_pressed[GLFW_KEY_LAST + 1];
    int keys_just_pressed[GLFW_KEY_LAST + 1];
} InputState;

/** Track local player movement and server character data. */
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

/** Track one server-visible NPC and its interpolation state. */
typedef struct {
    uint32_t npc_id;
    float pos_x;
    float pos_y;
    float target_x;          /**< Server target position for interpolation. */
    float target_y;
    float prev_x;            /**< Previous interpolation position. */
    float prev_y;
    float interp_t;          /**< Interpolation progress from 0.0 to 1.0. */
    uint32_t health;
    uint32_t max_health;
    uint8_t is_alive;
    uint8_t category;        /**< 0 passive, 1 hostile, or 2 quest. */
    uint8_t is_interactable;
    uint8_t npc_type_id;
    char name[32];
    float visual_y_offset;   /**< Client-only vertical animation offset. */
} VisibleNPC;

/** Track main-menu selection and animation state. */
typedef struct {
    int selected_button;
    int hovered_button;
    float animation_time;
} MenuState;

/** Track the received world list and its current UI selection. */
typedef struct {
    WorldListResponsePacket list;
    int loaded;
    int selected_index;
    int hovered_index;
} ServerListState;

/** Track character-list selection, creation, deletion, and errors. */
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
    int pending_delete;
    int pending_delete_index;
} CharacterSelectState;

/** Own persistent OpenGL textures shared across client screens. */
typedef struct {
    unsigned int player;
    unsigned int background;
    unsigned int session_panel_bg;
    unsigned int session_entry_bg;
} GameTextures;

/** Mirror one SessionPlayerEntry for the session panel. */
typedef struct {
    uint32_t player_id;
    char     name[32];
    uint8_t  level;
    uint8_t  player_class;
    uint8_t  player_race;
    uint16_t ping_ms;
} SessionPlayer;

#include "world.h"

/** Store screen-space HUD geometry. */
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
    float char_button_x;
    float char_button_y;
    float char_button_size;
    float currency_x;
    float currency_y;
    float currency_spacing;
} HUDLayout;

/** Track one nearby player received from interest broadcasts. */
typedef struct {
    uint32_t player_id;
    float pos_x, pos_y;
    int32_t health, max_health;
    uint8_t player_class;
    uint8_t player_race;
    uint8_t level;
    uint8_t is_dead;
    uint16_t ping_ms;
    char name[32];
} NearbyPlayer;

/** Track one visible server projectile. */
typedef struct {
    uint32_t id;
    float pos_x, pos_y;
    float dir_x, dir_y;
    float speed;
    uint8_t active;
} VisibleProjectile;

/** Track one visible NPC attack telegraph. */
typedef struct {
    uint32_t npc_id;
    uint8_t shape;          /**< 0 circle, 1 cone, 2 rectangle, or 3 line. */
    float pos_x, pos_y;
    float dir_x, dir_y;
    float radius, angle, width, length;
    float cast_time;
    float elapsed;
    uint8_t active;
} VisibleTelegraph;

/** Track one visible timed area-of-effect zone. */
typedef struct {
    uint32_t zone_id;
    float pos_x, pos_y;
    float radius;
    float duration;
    float elapsed;
    uint8_t active;
} VisibleZone;

/** Track one visible ground-item stack. */
typedef struct {
    uint32_t ground_item_id;
    uint32_t item_id;
    uint8_t quantity;
    float pos_x, pos_y;
    uint8_t active;
} GroundItem;

/** Store one rendered chat line. */
typedef struct {
    char sender[32];
    char text[256];
    uint8_t channel;
} ChatLine;

/** Track chat history, input, channel, and whisper reply state. */
typedef struct {
    ChatLine lines[MAX_CHAT_LINES];
    int line_count;
    int is_typing;
    char input_buf[MAX_CHAT_INPUT_LEN];
    int input_len;
    uint8_t active_channel;
    float backspace_timer;
    int   backspace_first;
    char whisper_reply_target[32];  /**< Last whisper sender used by /r. */
} ChatState;

/** Track one client-only expanding heal ring. */
typedef struct {
    float pos_x, pos_y;
    float age;
    float duration;
    float max_radius;
    int   active;
} HealVFX;

/** Store one shop item and its server price. */
typedef struct {
    uint32_t item_id;
    uint32_t buy_price;
} ShopItemEntry;

/** Track shop inventory, active tab, and selection state. */
typedef struct {
    int          is_open;
    uint32_t     shop_id;
    char         shop_name[32];
    uint8_t      item_count;
    ShopItemEntry items[MAX_SHOP_ITEMS];
    int          sell_tab;       /**< 0 for buy or 1 for sell. */
    int          hovered_slot;
} ShopState;

/** Store one party member's displayed status. */
typedef struct {
    uint32_t id;
    char name[32];
    uint8_t level;
    uint8_t player_class;
    int32_t health, max_health;
    int32_t mana, max_mana;
} PartyMember;

/** Track party membership and a pending invitation. */
typedef struct {
    uint32_t party_id;
    uint32_t leader_id;
    uint8_t member_count;
    PartyMember members[MAX_PARTY_SIZE];
    int has_party;
    int has_pending_invite;
    uint32_t invite_from_id;
    char invite_from_name[32];
    float invite_timer;
} PartyState;

/** Store persisted audio, display, and UI preferences. */
typedef struct {
    float master_volume;   /**< Range 0.0 to 1.0. */
    float music_volume;    /**< Range 0.0 to 1.0. */
    float sfx_volume;      /**< Range 0.0 to 1.0. */
    int   show_fps;
    int   fullscreen;
    float ui_scale;        /**< Range 0.75 to 1.5. */
} GameSettings;

/** Track one timed XP and gold notification. */
typedef struct {
    uint32_t xp_gained;
    uint32_t gold_gained;
    float age;              /**< Seconds since receipt. */
    int active;
} RewardNotification;

/** Aggregate heap-owned gameplay state while GAME_MODE_PLAYING is active. */
typedef struct {
    /** Fixed-capacity visible entity collections. */
    VisibleNPC        visible_npcs[MAX_VISIBLE_NPCS];
    int               visible_npc_count;
    uint32_t          target_npc_id;

    NearbyPlayer      nearby_players[MAX_NEARBY_PLAYERS];
    int               nearby_player_count;

    VisibleProjectile projectiles[MAX_VISIBLE_PROJECTILES];
    int               projectile_count;

    VisibleTelegraph  telegraphs[MAX_TELEGRAPHS];
    VisibleZone       zones[MAX_ZONES];
    GroundItem        ground_items[MAX_GROUND_ITEMS];
    HealVFX           heal_vfxs[MAX_HEAL_VFXS];

    /** Gameplay UI subsystem state. */
    ChatState         chat;
    PartyState        party;
    ShopState         shop;
    QuestLogState     quest_log;
    HUDLayout         hud;
    CombatState       combat;
    AbilityBarState   ability_bar;

    /** Session-panel pagination and rows. */
    int               show_session_panel;
    SessionPlayer     session_list[SESSION_LIST_PAGE_SIZE];
    int               session_list_count;
    uint32_t          session_total_players;
    uint16_t          session_current_page;
    uint16_t          session_total_pages;

    RewardNotification reward_notifications[MAX_REWARD_POPUPS];

    int               is_dead;
    float             death_timer;

    int               show_level_up;
    float             level_up_timer;
    int               level_up_new_level;

    /** Server-authoritative derived player stats. */
    int32_t           player_strength;
    int32_t           player_agility;
    int32_t           player_intelligence;
    int32_t           player_wisdom;
    int32_t           player_defense;
    int32_t           player_evasion;
    int32_t           player_vitality;
    int32_t           player_luck;
    float             player_move_speed;
    int32_t           player_weapon_damage;
    uint64_t          player_xp_for_next;

    EnterWorldResponsePacket enter_world_response;

    int               is_paused;

    int               show_map;
    float             map_zoom;

    uint32_t          target_player_id;

    char              current_zone_name[48];
    char              zone_banner_name[48];
    float             zone_banner_timer;     /**< Remaining banner time, or 0 when hidden. */
} PlayingState;

/** Own the client lifecycle, screen state, network identity, and gameplay allocation. */
struct GameState {
    int is_running;
    GameMode mode;
    NetworkState net_state;
    float        net_wait_seconds; /**< Seconds spent awaiting the current network response. */
    InputState input;
    PlayerState player;
    WorldState world;
    Camera camera;
    MenuState main_menu;
    ServerListState server_list;
    CharacterSelectState char_select;

    GameTextures textures;
    int background_width;
    int background_height;

    InventoryState* inventory;
    CharacterScreenState* character_screen;

    char session_key[65];
    uint32_t account_id;
    char username[32];
    char realm_ip[16];
    int realm_port;
    int network_connected;
    char network_status[128];

    GameSettings settings;
    int          show_settings;

    PlayingState* playing;       /**< Heap allocation owned while playing, or NULL. */
};

#endif // GAME_TYPES_H
