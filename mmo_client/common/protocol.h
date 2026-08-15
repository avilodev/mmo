#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <winsock2.h>

#define MAX_WORLDS 10
#define MAX_CAST_TARGETS    16
#define MAX_NPCS_PER_PACKET 32

#pragma pack(push, 1)

// Packet Types
typedef enum {
    PACKET_DISCONNECT = 2,
    PACKET_AUTH_LOGIN = 3,              // Username/password validation (no session created)
    PACKET_AUTH_REGISTER = 4,
    PACKET_AUTH_RESPONSE = 5,           // Success/fail (no session yet)
    PACKET_START_GAME_REQUEST = 6,      // Request to start game (creates session)
    PACKET_START_GAME_RESPONSE = 7,     // Returns session key + player ID
    
    // PATCH NOTES
    PATCH_NOTES_REQUEST = 10,
    PATCH_NOTES_RESPONSE = 11,
    
    // REALM SERVER PACKETS (Character Selection)
    PACKET_REALM_CONNECT = 20,
    PACKET_REALM_CONNECT_ACK = 21,
    PACKET_CHARACTER_LIST_REQUEST = 22,
    PACKET_CHARACTER_LIST_RESPONSE = 23,
    PACKET_CHARACTER_CREATE_REQUEST = 24,
    PACKET_CHARACTER_CREATE_RESPONSE = 25,
    PACKET_CHARACTER_DELETE_REQUEST = 26,
    PACKET_CHARACTER_DELETE_RESPONSE = 27,
    PACKET_WORLD_LIST_REQUEST = 28,
    PACKET_WORLD_LIST_RESPONSE = 29,
    PACKET_ENTER_WORLD = 30,
    PACKET_ENTER_WORLD_RESPONSE = 31,
    
    // WORLD SERVER PACKETS (Gameplay)
    PACKET_WORLD_CONNECT = 50,
    PACKET_WORLD_CONNECT_ACK = 51,
    PACKET_PLAYER_MOVE = 52,
    PACKET_PLAYER_MOVE_ACK = 53,
    PACKET_REQUEST_PLAYER_DATA = 54,
    PACKET_PLAYER_DATA_RESPONSE = 55,
    PACKET_PING = 56,
    PACKET_LOGOUT = 59,             // Client -> Server: clean disconnect

    PACKET_CAST_CANCEL = 73,
    PACKET_ATTACK_INTENT    = 74,
    PACKET_CAST_START_V2    = 75,
    PACKET_DAMAGE_V2        = 76,
    PACKET_ATTACK_RESULT    = 77,

    PACKET_ABILITY_CAST_INTENT  = 80,
    PACKET_ABILITY_CAST_START   = 81,
    PACKET_ABILITY_EFFECT       = 82,
    PACKET_ABILITY_CAST_CANCEL  = 83,
    PACKET_STATUS_EFFECT_APPLY  = 84,
    PACKET_STATUS_EFFECT_REMOVE = 85,
    PACKET_SPAWN_ZONE           = 86,
    PACKET_REMOVE_ZONE          = 87,
    PACKET_MANA_UPDATE          = 88,

    PACKET_LEVEL_UP             = 89,
    PACKET_ABILITY_DATA         = 90,

    PACKET_PLAYER_STATS         = 91,
    PACKET_REQUEST_PLAYER_STATS = 92,

    PACKET_PLAYER_POSITIONS = 96,
    PACKET_NPC_POSITIONS = 97,

    // Equipment packets (100-119)
    PACKET_EQUIP_ITEM = 100,
    PACKET_EQUIP_ITEM_RESPONSE = 101,
    PACKET_UNEQUIP_ITEM = 102,
    PACKET_UNEQUIP_ITEM_RESPONSE = 103,

    // Inventory packets (120-139)
    PACKET_MOVE_ITEM = 123,
    PACKET_MOVE_ITEM_RESPONSE = 124,
    PACKET_USE_ITEM = 125,
    PACKET_USE_ITEM_RESPONSE = 126,
    PACKET_DROP_ITEM = 127,
    PACKET_DROP_ITEM_RESPONSE = 128,

    // Dialogue packets (130-139)
    PACKET_NPC_INTERACT_REQUEST = 130,
    PACKET_NPC_INTERACT_RESPONSE = 131,
    PACKET_DIALOGUE_OPTION_SELECT = 132,
    PACKET_DIALOGUE_UPDATE = 133,
    PACKET_DIALOGUE_CLOSE = 134,

    // Projectile packets (140-149)
    PACKET_PROJECTILE_SPAWN   = 140,
    PACKET_PROJECTILE_UPDATE  = 141,
    PACKET_PROJECTILE_DESTROY = 142,

    // Death/Respawn packets (150-154)
    PACKET_PLAYER_DEATH       = 150,
    PACKET_PLAYER_RESPAWN     = 151,

    // Loot packets (155-159)
    PACKET_LOOT_DROP            = 155,
    PACKET_LOOT_PICKUP_REQUEST  = 156,
    PACKET_LOOT_PICKUP_RESPONSE = 157,
    PACKET_LOOT_DESPAWN         = 158,

    // NPC Telegraph packets (160-164)
    PACKET_NPC_TELEGRAPH_START   = 160,
    PACKET_NPC_TELEGRAPH_RESOLVE = 161,

    // Chat packets (170-179)
    PACKET_CHAT_SEND     = 170,
    PACKET_CHAT_MESSAGE  = 171,

    // Party packets (180-189)
    PACKET_PARTY_INVITE        = 180,
    PACKET_PARTY_INVITE_NOTIFY = 181,
    PACKET_PARTY_ACCEPT        = 182,
    PACKET_PARTY_DECLINE       = 183,
    PACKET_PARTY_LEAVE         = 184,
    PACKET_PARTY_KICK          = 185,
    PACKET_PARTY_UPDATE        = 186,
    PACKET_PARTY_DISBAND       = 187,

    // Rewards
    PACKET_KILL_REWARD         = 190,      // Server -> Client: XP + gold gained on kill

    // SERVER-TO-SERVER PACKETS (200-209)
    PACKET_REALM_AUTH = 200,
    PACKET_REALM_AUTH_ACK = 201,
    PACKET_WORLD_HEARTBEAT = 202,
    PACKET_WORLD_STATUS = 203,

    // Quest packets (191-193)
    PACKET_QUEST_ACCEPT   = 191,  // Server -> Client: quest added to log
    PACKET_QUEST_PROGRESS = 192,  // Server -> Client: objective progress update
    PACKET_QUEST_COMPLETE = 193,  // Server -> Client: quest done + rewards

    // Shop packets (194-198)
    PACKET_SHOP_OPEN         = 194,  // Server -> Client: shop inventory
    PACKET_SHOP_BUY          = 195,  // Client -> Server: buy item
    PACKET_SHOP_BUY_RESPONSE = 196,  // Server -> Client: buy result
    PACKET_SHOP_SELL         = 197,  // Client -> Server: sell item
    PACKET_SHOP_SELL_RESPONSE = 198, // Server -> Client: sell result

    PACKET_SESSION_LIST_REQUEST  = 210, // Client -> Server: request page of online players
    PACKET_SESSION_LIST_RESPONSE = 211, // Server -> Client: paginated list of online players

    PACKET_ZONE_CHANGE = 220,           // Server -> Client: player crossed a zone boundary
} PacketType;

typedef enum {
    GLADIATOR = 1,
    NINJA = 2,
    LANDWEAVER = 3,
    SPIRIT = 4
} Class;

typedef enum {
    HUMAN = 1,
    PYSECK = 2,
    INFOR = 3
} Race;

// Standard packet header
typedef struct {
    uint8_t type;           // 1 byte
    uint32_t player_id;     // 4 bytes
    uint16_t payload_size;  // 2 bytes
} PacketHeader;             // Total: 7 bytes (no padding)

// Zone types (matches server zone_system.h)
#define ZONE_TYPE_WILD    0
#define ZONE_TYPE_SAFE    1
#define ZONE_TYPE_DUNGEON 2
#define ZONE_TYPE_PVP     3

typedef struct {
    PacketHeader header;
    uint8_t      zone_id;
    uint8_t      zone_type;
    char         zone_name[48];
} ZoneChangePacket;

// Auth packet header (with session key) - used when session key is needed
typedef struct {
    uint8_t type;
    uint32_t player_id;
    uint16_t payload_size;
    char session_key[32];
} AuthPacketHeader;

// Patch notes packets
typedef struct {
    PacketHeader header;
    uint16_t start;
    uint16_t end;
} PatchNotesRequest;

typedef struct {
    PacketHeader header;
    char buffer[4000];
} PatchNotesResponse;

// ============================================================================
// LOGIN PACKETS (validation only, no session)
// ============================================================================

typedef struct {
    PacketHeader header;
    char username[32];
    char password[64];
} AuthLoginPacket;

typedef struct {
    PacketHeader header;
    uint8_t success;
    uint32_t player_id;
    char auth_token[32];     // Single-use token required by START_GAME_REQUEST
    char message[128];
} AuthLoginResponsePacket;

// ============================================================================
// REGISTRATION PACKETS (creates session)
// ============================================================================

typedef struct {
    PacketHeader header;
    char username[32];
    char password[64];
    char email[64];
    char birthday[16];        // Format: "YYYY-MM-DD"
    uint8_t reserved[32];     // Reserved for future fields
} AuthRegisterPacket;

// Registration response - NO SESSION KEY
// User must log in after successful registration
typedef struct {
    PacketHeader header;      // Basic header without session key
    uint8_t success;          // 1 = success, 0 = failure
    uint32_t player_id;       // New player ID (in network byte order)
    char message[128];        // Success or error message
} AuthRegisterResponsePacket;

//

typedef struct {
    PacketHeader header;
    uint32_t player_id;      // Informational only; server trusts auth_token
    char username[32];       // Informational only
    char auth_token[32];     // Single-use proof of successful login
} StartGameRequestPacket;

typedef struct {
    AuthPacketHeader header;
    uint8_t success;
    char message[128];
} StartGameResponsePacket;

// Realm connection packets (used by game.exe, not launcher)
typedef struct {
    AuthPacketHeader header;
} RealmConnectPacket;

typedef struct {
    PacketHeader header;
    uint8_t success;
    char message[128];
} RealmConnectAckPacket;

// World list packets
typedef struct {
    PacketHeader header;
} WorldListRequestPacket;
 
// World list - PADDING FIXED
typedef struct {
    char name[64];
    uint32_t world_id;
    uint16_t population;
    int16_t capacity;
    uint8_t status; // 0=offline, 1=online, 2=full
    uint8_t padding1[3];    // EXPLICIT PADDING ADDED
    char ip[16];
    uint16_t port;
    uint8_t padding2[2];    // EXPLICIT PADDING ADDED
    char region[32];
} WorldInfo;

typedef struct {
    PacketHeader header;
    uint8_t count;
    uint8_t padding[3];
    WorldInfo worlds[MAX_WORLDS];
} WorldListResponsePacket;
//

typedef struct {
    PacketHeader header;
    uint32_t world_id;
} CharacterListRequestPacket;

//Lists all characters
typedef struct {
    PacketHeader header;
    uint32_t world_id;
    uint8_t count;
    uint8_t padding[3];
    struct {
        uint32_t character_id;
        char name[32];
        uint32_t level;
        uint32_t class_id;
        uint32_t race_id;
    } characters[10];
} CharacterListResponsePacket;

// Character create request
typedef struct {
    PacketHeader header;
    uint32_t world_id;
    char name[32];
    uint32_t class_id;
    uint32_t race_id;
} CharacterCreateRequestPacket;

// Character create response
typedef struct {
    PacketHeader header;
    uint32_t world_id;
    uint8_t success;
    uint32_t character_id;
    char character_name[32];
    char message[128];
} CharacterCreateResponsePacket;

// Character delete request
typedef struct {
    PacketHeader header; 
    uint32_t character_id;
    uint32_t world_id;
} CharacterDeleteRequestPacket;

// Character delete response
typedef struct {
    PacketHeader header;
    uint32_t character_id;
    uint32_t world_id;
    uint8_t success;
    char message[128];
} CharacterDeleteResponsePacket;

// ============================================================================
// ENTER WORLD PACKETS
// ============================================================================

// Client -> Realm: Request to enter a world with a character
typedef struct {
    PacketHeader header;
    uint32_t character_id;
    uint32_t world_id;
} EnterWorldPacket;

// Realm -> Client: Response with world server info and game ticket
typedef struct {
    PacketHeader header;
    uint8_t success;
    char game_ticket[64];     // Short-lived token for world server
    char world_ip[16];
    uint16_t world_port;
    char message[128];
} EnterWorldResponsePacket;

// ============================================================================
// WORLD SERVER CONNECTION PACKETS
// ============================================================================

// Client -> World: Connect to world server with game ticket
typedef struct {
    PacketHeader header;
    char game_ticket[64];
    uint32_t character_id;
} WorldConnectPacket;

// World -> Client: Connection acknowledgment
typedef struct {
    PacketHeader header;
    uint8_t success;
    char welcome_message[128];
} WorldConnectAckPacket;

typedef struct {
    PacketHeader header;
    uint32_t character_id;
    uint32_t world_id;
} WorldPlayerDataRequest;

typedef struct {
    PacketHeader header;

    uint32_t character_id;
    char name[32];
    uint32_t level;
    float pos_x;
    float pos_y;
    uint32_t health;
    uint32_t max_health;
    int32_t mana;
    int32_t max_mana;
    uint64_t experience;
    uint32_t gold;

    Class player_class;
    Race player_race;
    
    // Equipment
    uint32_t helmet;
    uint32_t gloves;
    uint32_t chest_armor;
    uint32_t leggings;
    uint32_t boots;
    uint32_t main_hand;
    uint32_t second_hand;
    uint16_t blessing;
    
    // Inventory (150 slots)
    uint32_t inventory[150];
} CharacterInfo;

/*
=========================
  MOVEMENT
=========================  
*/
// Client -> Server: Player movement update
typedef struct {
    PacketHeader header;
    float pos_x;
    float pos_y;

    float player_speed;

    float vel_x;
    float vel_y;
} PlayerMovePacket;

// Server -> Client: Movement acknowledgment (optional)
typedef struct {
    PacketHeader header;
    float pos_x;
    float pos_y;
} PlayerMoveAckPacket;

// ============================================================================
// COMBAT PACKETS
// ============================================================================

// Server -> Client: Cast cancelled/interrupted
typedef struct {
    PacketHeader header;
    uint32_t caster_id;
    uint8_t reason;          // 0=manual cancel, 1=moved, 2=interrupted
} CastCancelPacket;

typedef enum {
    ATTACK_RESULT_OK            = 0,    // At least one target was hit (damage packets follow)
    ATTACK_RESULT_ON_COOLDOWN   = 1,    // Attacker is still on cooldown
    ATTACK_RESULT_ALREADY_CASTING = 2,  // Attacker is mid-cast
    ATTACK_RESULT_NO_TARGETS    = 3,    // Resolved the area but nothing was there
    ATTACK_RESULT_INVALID       = 4     // Generic failure (attacker not found, etc.)
} AttackResultCode;


typedef struct {
    PacketHeader header;        // header.player_id = attacker's character_id
    float        aim_x;         // World X the player is aiming at
    float        aim_y;         // World Y the player is aiming at
} AttackIntentPacket;

typedef struct {
    PacketHeader header;
    uint32_t     caster_id;                     // Who is attacking
    float        cast_time;                     // Seconds until damage resolves
    uint8_t      attack_type;                   // AttackType enum value
    uint8_t      target_count;                  // How many targets resolved (0..MAX_CAST_TARGETS)
    uint32_t     target_ids[MAX_CAST_TARGETS]; // Entity IDs that will be hit
    // Rendering hint: attacker position at cast start (for trajectory drawing)
    float        origin_x;
    float        origin_y;
    float        aim_x;
    float        aim_y;
} CastStartV2Packet;

typedef struct {
    PacketHeader header;
    uint32_t     attacker_id;
    uint32_t     target_id;
    uint32_t     damage;            // Actual damage dealt (after variance)
    uint32_t     target_new_health; // Authoritative HP after hit
    uint8_t      is_kill;           // 1 if this hit brought target to 0
    uint8_t      is_crit;           // 1 if this was a critical hit
} DamageV2Packet;

typedef struct {
    PacketHeader header;        // header.player_id = attacker's character_id
    uint8_t      result_code;   // AttackResultCode enum value
} AttackResultPacket;

// ============================================================================
// Ability PACKETS
// ============================================================================

// Client -> Server: Cast an ability
typedef struct {
    PacketHeader header;
    uint16_t     ability_id;
    float        aim_x;
    float        aim_y;
    uint32_t     target_id;     // 0 if ground-targeted/self
} AbilityCastIntentPacket;

// Server -> Client: Cast started
typedef struct {
    PacketHeader header;
    uint32_t     caster_id;
    uint16_t     ability_id;
    float        cast_time;
    float        origin_x;
    float        origin_y;
    float        aim_x;
    float        aim_y;
} AbilityCastStartPacket;

// Server -> Client: Ability effect applied
typedef struct {
    PacketHeader header;
    uint32_t     caster_id;
    uint32_t     target_id;
    uint16_t     ability_id;
    int32_t      damage;
    int32_t      healing;
    int32_t      target_new_health;
    uint8_t      is_kill;
    uint8_t      is_crit;           // 1 if this was a critical hit or heal
} AbilityEffectPacket;

// Bidirectional: Ability cast cancelled
typedef struct {
    PacketHeader header;
    uint32_t     caster_id;
    uint16_t     ability_id;
    uint8_t      reason;        // 0=manual, 1=moved, 2=interrupted, 3=no_mana
} AbilityCastCancelPacket;

// Server -> Client: Status effect applied
typedef struct {
    PacketHeader header;
    uint32_t     target_id;
    uint8_t      effect_type;
    int32_t      value;
    float        duration;
    uint32_t     source_id;
} StatusEffectApplyPacket;

// Server -> Client: Status effect removed
typedef struct {
    PacketHeader header;
    uint32_t     target_id;
    uint8_t      effect_type;
} StatusEffectRemovePacket;

// Server -> Client: Zone spawned
typedef struct {
    PacketHeader header;
    uint32_t     zone_id;
    uint32_t     caster_id;
    uint16_t     ability_id;
    float        pos_x;
    float        pos_y;
    float        duration;
    float        radius;
    uint8_t      has_collision;
} SpawnZonePacket;

// Server -> Client: Zone removed
typedef struct {
    PacketHeader header;
    uint32_t     zone_id;
} RemoveZonePacket;

// Server -> Client: Mana update
typedef struct {
    PacketHeader header;
    int32_t      mana;
    int32_t      max_mana;
} ManaUpdatePacket;

// Server -> Client: Ability bar loadout (sent on world entry and on level-up)
typedef struct {
    uint16_t id;
    char     name[24];
    float    cooldown;
    float    cast_time;
    int16_t  mana_cost;
    char     image[32];  // Icon filename, e.g. "cleave.png" — looked up in Game/Sprites/Abilities/
} AbilitySlotInfo;

typedef struct {
    PacketHeader header;
    uint8_t  count;
    AbilitySlotInfo slots[5];
} AbilityDataPacket;

// ============================================================================
// LEVEL & STATS PACKETS
// ============================================================================

// Server -> Client: Player leveled up
typedef struct {
    PacketHeader header;
    uint32_t     new_level;
    uint32_t     new_max_health;
    uint32_t     new_max_mana;
    uint32_t     new_health;
    uint32_t     new_mana;
    int32_t      strength;
    int32_t      agility;
    int32_t      intelligence;
    int32_t      wisdom;
    int32_t      defense;
    int32_t      evasion;
    int32_t      vitality;
    int32_t      luck;
    uint64_t     xp_for_next_level;
} LevelUpPacket;

// Server -> Client: Full stat snapshot (sent on login + on request)
typedef struct {
    PacketHeader header;
    int32_t      strength;
    int32_t      agility;
    int32_t      intelligence;
    int32_t      wisdom;
    int32_t      defense;
    int32_t      evasion;
    int32_t      vitality;
    int32_t      luck;
    int32_t      max_health;
    int32_t      max_mana;
    int32_t      current_health;
    int32_t      current_mana;
    float        move_speed;
    int32_t      weapon_damage;
    uint64_t     xp_for_next_level;
} PlayerStatsPacket;

// Client -> Server: Request stats refresh (e.g. on reconnect)
typedef struct {
    PacketHeader header;
} RequestPlayerStatsPacket;

// ============================================================================
// ENEMY UPDATE PACKETS
// ============================================================================

typedef struct {
    uint32_t npc_id;
    float pos_x;
    float pos_y;
    uint32_t health;
    uint32_t max_health;
    uint8_t is_alive;
    uint8_t category;        // NPCCategory: 0=passive, 1=hostile, 2=quest
    uint8_t is_interactable; // 1 if player can interact (talk)
    uint8_t npc_type_id;     // NPC type for client display
} NPCPositionData;

typedef struct {
    PacketHeader header;
    uint8_t npc_count;
    uint8_t padding[3];  // Must match server alignment
    NPCPositionData npcs[MAX_NPCS_PER_PACKET];
} NPCPositionPacket;

// ============================================================================
// NPC DIALOGUE PACKETS (Client-side text storage)
// ============================================================================

#define MAX_DIALOGUE_OPTIONS 6

// Client -> Server: Request to interact with NPC
typedef struct {
    PacketHeader header;
    uint32_t npc_id;
} NPCInteractRequestPacket;

// Server -> Client: Initial dialogue response
// Client looks up text from local dialogues.json using dialogue_id + page_num
typedef struct {
    PacketHeader header;
    uint32_t npc_id;
    uint32_t dialogue_id;
    uint8_t  page_num;
    uint8_t  option_count;
    char     npc_name[32];
    uint8_t  option_ids[MAX_DIALOGUE_OPTIONS];
    uint8_t  padding[2];
} NPCInteractResponsePacket;

// Client -> Server: Player selects dialogue option
typedef struct {
    PacketHeader header;
    uint32_t npc_id;
    uint32_t dialogue_id;
    uint8_t  current_page;
    uint8_t  option_selected;
    uint8_t  padding[2];
} DialogueOptionSelectPacket;

// Server -> Client: Update to new dialogue page
// Client looks up text from local dialogues.json using dialogue_id + page_num
typedef struct {
    PacketHeader header;
    uint32_t npc_id;
    uint32_t dialogue_id;
    uint8_t  page_num;
    uint8_t  option_count;
    uint8_t  option_ids[MAX_DIALOGUE_OPTIONS];
    uint8_t  padding[2];
} DialogueUpdatePacket;

// Bidirectional: Close dialogue window
typedef struct {
    PacketHeader header;
    uint32_t npc_id;
} DialogueClosePacket;

// ============================================================================
// EQUIPMENT PACKETS
// ============================================================================

typedef enum {
    SLOT_HELMET    = 1,
    SLOT_CHEST     = 2,
    SLOT_GLOVES    = 3,
    SLOT_LEGGINGS  = 4,
    SLOT_BOOTS     = 5,
    SLOT_MAIN_HAND = 6,
    SLOT_OFF_HAND  = 7,
} EquipSlotType;

// Client -> Server: Equip item from inventory
typedef struct {
    PacketHeader header;
    uint32_t item_id;
    uint8_t inventory_slot;
    uint8_t equip_slot;
    uint8_t padding[2];
} EquipItemPacket;

// Server -> Client: Equip result
typedef struct {
    PacketHeader header;
    uint8_t success;
    uint8_t padding[3];
    uint32_t equipped_item;
    uint32_t returned_item;
    char message[128];
} EquipItemResponsePacket;

// Client -> Server: Unequip item to inventory
typedef struct {
    PacketHeader header;
    uint8_t equip_slot;
    uint8_t padding[3];
} UnequipItemPacket;

// Server -> Client: Unequip result
typedef struct {
    PacketHeader header;
    uint8_t success;
    uint8_t inventory_slot;
    uint8_t padding[2];
    uint32_t unequipped_item;
    char message[128];
} UnequipItemResponsePacket;

// ============================================================================
// INVENTORY PACKETS
// ============================================================================

// Client -> Server: Use item from inventory
typedef struct {
    PacketHeader header;
    uint8_t inventory_slot;
    uint8_t padding[3];
} UseItemPacket;

// Server -> Client: Use item result
typedef struct {
    PacketHeader header;
    uint8_t success;
    uint8_t effect_type;
    uint8_t padding[2];
    int32_t health_changed;
    int32_t mana_changed;
    int32_t new_health;
    int32_t new_mana;
    uint32_t item_id;
    char message[128];
} UseItemResponsePacket;

// Client -> Server: Drop item from inventory
typedef struct {
    PacketHeader header;
    uint8_t inventory_slot;
    uint8_t padding[3];
} DropItemPacket;

// Server -> Client: Drop item result
typedef struct {
    PacketHeader header;
    uint8_t success;
    uint8_t padding[3];
    uint32_t dropped_item;
} DropItemResponsePacket;

// Client -> Server: Move item between inventory slots
typedef struct {
    PacketHeader header;
    uint8_t from_slot;
    uint8_t to_slot;
    uint8_t padding[2];
} MoveItemPacket;

// Server -> Client: Move item result
typedef struct {
    PacketHeader header;
    uint8_t success;
    uint8_t from_slot;
    uint8_t to_slot;
    uint8_t padding;
} MoveItemResponsePacket;

// ============================================================================
// PROJECTILE PACKETS
// ============================================================================

#define MAX_PROJECTILES_PER_PACKET 32

// Server -> Client: A new projectile was spawned
typedef struct {
    PacketHeader header;
    uint32_t projectile_id;
    uint16_t ability_id;
    uint32_t owner_id;
    uint8_t  owner_type;        // 0=player, 1=npc
    float    pos_x, pos_y;
    float    dir_x, dir_y;
    float    speed;
} ProjectileSpawnPacket;

// Single projectile position entry for batch updates
typedef struct {
    uint32_t projectile_id;
    float    pos_x, pos_y;
} ProjectilePositionData;

// Server -> Client: Batch update of visible projectile positions
typedef struct {
    PacketHeader header;
    uint8_t count;
    uint8_t padding[3];
    ProjectilePositionData projectiles[MAX_PROJECTILES_PER_PACKET];
} ProjectileUpdatePacket;

// Server -> Client: A projectile was destroyed
typedef struct {
    PacketHeader header;
    uint32_t projectile_id;
    uint8_t  reason;            // 0=expired, 1=hit_target, 2=cancelled
} ProjectileDestroyPacket;

// ============================================================================
// DEATH / RESPAWN PACKETS
// ============================================================================

// Server -> Client: A player died
typedef struct {
    PacketHeader header;
    uint32_t dead_player_id;
    uint32_t killer_id;
    uint8_t  killer_type;       // 0=npc, 1=player, 2=environment
} PlayerDeathPacket;

// Server -> Client: A player respawned
typedef struct {
    PacketHeader header;
    uint32_t player_id;
    float    pos_x, pos_y;
    int32_t  health;
    int32_t  max_health;
    int32_t  mana;
    int32_t  max_mana;
} PlayerRespawnPacket;

// ============================================================================
// LOOT PACKETS
// ============================================================================

// Server -> Client: Item dropped on ground
typedef struct {
    PacketHeader header;
    uint32_t ground_item_id;
    uint32_t item_id;
    uint8_t  quantity;
    float    pos_x, pos_y;
} LootDropPacket;

// Client -> Server: Pick up a ground item
typedef struct {
    PacketHeader header;
    uint32_t ground_item_id;
} LootPickupRequestPacket;

// Server -> Client: Pickup result
typedef struct {
    PacketHeader header;
    uint8_t  success;
    uint32_t ground_item_id;
    uint32_t item_id;
    uint8_t  quantity;
    uint8_t  inventory_slot;
    char     message[64];
} LootPickupResponsePacket;

// Server -> Client: Ground item disappeared
typedef struct {
    PacketHeader header;
    uint32_t ground_item_id;
} LootDespawnPacket;

// ============================================================================
// PLAYER POSITION BROADCAST
// ============================================================================

#define MAX_NEARBY_PLAYERS 32

typedef struct {
    uint32_t player_id;
    float    pos_x, pos_y;
    int32_t  health;
    int32_t  max_health;
    uint8_t  player_class;
    uint8_t  player_race;
    uint8_t  level;
    uint8_t  is_dead;
    uint16_t ping_ms;       // client-reported RTT in milliseconds
    uint8_t  padding[2];
} NearbyPlayerData;

// Server -> Client: Batch update of nearby player positions
typedef struct {
    PacketHeader header;
    uint8_t count;
    uint8_t padding[3];
    NearbyPlayerData players[MAX_NEARBY_PLAYERS];
} PlayerPositionBroadcastPacket;

// ============================================================================
// SESSION LIST (O MENU) — full server player list, paginated at 30 per page
// ============================================================================

#define SESSION_LIST_PAGE_SIZE 30

// One entry per player in the server list
typedef struct {
    uint32_t player_id;
    char     name[32];
    uint8_t  level;
    uint8_t  player_class;
    uint8_t  player_race;
    uint8_t  padding;
    uint16_t ping_ms;
    uint8_t  padding2[2];
} SessionPlayerEntry;  // 44 bytes

// Client -> Server: request page N of the online player list
typedef struct {
    PacketHeader header;
    uint16_t page;      // 0-indexed
    uint8_t  padding[2];
} SessionListRequestPacket;

// Server -> Client: one page of the online player list
typedef struct {
    PacketHeader     header;
    uint32_t         total_players;   // total online right now
    uint16_t         total_pages;
    uint16_t         current_page;    // 0-indexed
    uint8_t          count;           // entries in this packet (≤ SESSION_LIST_PAGE_SIZE)
    uint8_t          padding[3];
    SessionPlayerEntry entries[SESSION_LIST_PAGE_SIZE];
} SessionListResponsePacket;

// ============================================================================
// NPC TELEGRAPH PACKETS
// ============================================================================

// Server -> Client: NPC started casting - show ground indicator
typedef struct {
    PacketHeader header;
    uint32_t npc_id;
    uint16_t ability_id;
    uint8_t  shape;             // 0=circle, 1=cone, 2=rectangle, 3=line
    float    pos_x, pos_y;
    float    dir_x, dir_y;
    float    radius;
    float    angle;
    float    width;
    float    length;
    float    cast_time;
} NPCTelegraphStartPacket;

// Server -> Client: NPC telegraph resolved - show impact VFX
typedef struct {
    PacketHeader header;
    uint32_t npc_id;
    uint16_t ability_id;
} NPCTelegraphResolvePacket;

// ============================================================================
// CHAT PACKETS
// ============================================================================

#define MAX_CHAT_MESSAGE 256

typedef enum {
    CHAT_CHANNEL_LOCAL  = 0,
    CHAT_CHANNEL_GLOBAL = 1,
    CHAT_CHANNEL_WHISPER = 2,
    CHAT_CHANNEL_PARTY  = 3,
} ChatChannel;

// Client -> Server: Player sends a chat message
typedef struct {
    PacketHeader header;
    uint8_t  channel;
    uint8_t  padding[3];
    char     message[MAX_CHAT_MESSAGE];
} ChatSendPacket;

// Server -> Client: Chat message broadcast
typedef struct {
    PacketHeader header;
    uint32_t sender_id;
    uint8_t  channel;
    uint8_t  padding[3];
    char     sender_name[32];
    char     message[MAX_CHAT_MESSAGE];
} ChatMessagePacket;

// ============================================================================
// PARTY PACKETS
// ============================================================================

#define MAX_PARTY_SIZE 5

// Client -> Server: invite player by name
typedef struct {
    PacketHeader header;
    char target_name[32];
} PartyInvitePacket;

// Server -> Client: you have a pending invite
typedef struct {
    PacketHeader header;
    uint32_t from_id;
    char from_name[32];
} PartyInviteNotifyPacket;

// Client -> Server: accept invite
typedef struct {
    PacketHeader header;
} PartyAcceptPacket;

// Client -> Server: decline invite
typedef struct {
    PacketHeader header;
} PartyDeclinePacket;

// Client -> Server: leave party
typedef struct {
    PacketHeader header;
} PartyLeavePacket;

// Client -> Server: leader kicks member
typedef struct {
    PacketHeader header;
    uint32_t target_id;
} PartyKickPacket;

// Server -> Client: full party state update
typedef struct {
    PacketHeader header;
    uint32_t party_id;
    uint32_t leader_id;
    uint8_t  member_count;
    uint8_t  padding[3];
    struct {
        uint32_t character_id;
        char     name[32];
        uint8_t  level;
        uint8_t  player_class;
        uint8_t  padding[2];
        int32_t  health;
        int32_t  max_health;
        int32_t  mana;
        int32_t  max_mana;
    } members[MAX_PARTY_SIZE];
} PartyUpdatePacket;

// Server -> Client: party disbanded
typedef struct {
    PacketHeader header;
} PartyDisbandPacket;

// ============================================================================
// REWARD PACKETS
// ============================================================================

// Server -> Client: XP and gold gained from a kill
typedef struct {
    PacketHeader header;
    uint32_t     xp_gained;
    uint32_t     gold_gained;
    uint64_t     total_xp;       // Player's new total XP (for bar update)
    uint32_t     total_gold;     // Player's new total gold
} KillRewardPacket;

// ============================================================================
// SHOP PACKETS
// ============================================================================

#define MAX_SHOP_ITEMS 32

typedef struct {
    uint32_t item_id;
    uint32_t buy_price;
} ShopItemInfo;

// Server -> Client: Open shop window with item list
typedef struct {
    PacketHeader header;
    uint32_t     shop_id;
    char         shop_name[32];
    uint8_t      item_count;
    uint8_t      padding[3];
    ShopItemInfo items[MAX_SHOP_ITEMS];
} ShopOpenPacket;

// Client -> Server: Buy one item from the shop
typedef struct {
    PacketHeader header;
    uint32_t shop_id;
    uint32_t item_id;
} ShopBuyPacket;

// Server -> Client: Result of a buy request
typedef struct {
    PacketHeader header;
    uint8_t  success;
    uint8_t  inventory_slot;
    uint8_t  padding[2];
    uint32_t item_id;
    uint32_t new_gold;
    char     message[64];
} ShopBuyResponsePacket;

// Client -> Server: Sell one inventory slot to the shop
typedef struct {
    PacketHeader header;
    uint32_t shop_id;
    uint8_t  inventory_slot;
    uint8_t  padding[3];
} ShopSellPacket;

// Server -> Client: Result of a sell request
typedef struct {
    PacketHeader header;
    uint8_t  success;
    uint8_t  inventory_slot;
    uint8_t  padding[2];
    uint32_t item_id;
    uint32_t sell_price;
    uint32_t new_gold;
    char     message[64];
} ShopSellResponsePacket;

// ============================================================================
// QUEST PACKETS
// ============================================================================

#define MAX_QUEST_OBJECTIVES 4

typedef struct {
    char    description[64];
    int32_t required;
} QuestObjectiveInfo;

// Server -> Client: Quest accepted / added to log
typedef struct {
    PacketHeader       header;
    uint32_t           quest_id;
    char               title[48];
    uint8_t            obj_count;
    uint8_t            padding[3];
    QuestObjectiveInfo objectives[MAX_QUEST_OBJECTIVES];
} QuestAcceptPacket;

// Server -> Client: Real-time objective progress update
typedef struct {
    PacketHeader header;
    uint32_t quest_id;
    uint8_t  obj_index;
    uint8_t  padding[3];
    int32_t  current;
    int32_t  required;
} QuestProgressPacket;

typedef struct {
    uint32_t item_id;
    uint8_t  quantity;
    uint8_t  inventory_slot;
    uint8_t  padding[2];
} QuestRewardItem;

// Server -> Client: Quest completed + rewards granted
typedef struct {
    PacketHeader    header;
    uint32_t        quest_id;
    uint32_t        xp_reward;
    uint32_t        gold_reward;
    uint8_t         item_count;
    uint8_t         padding[3];
    QuestRewardItem items[MAX_QUEST_OBJECTIVES];
} QuestCompletePacket;

// Network byte order conversion for 64-bit values
#define htonll(x) ((1==htonl(1)) ? (x) : ((uint64_t)htonl((x) & 0xFFFFFFFF) << 32) | htonl((x) >> 32))
#define ntohll(x) ((1==ntohl(1)) ? (x) : ((uint64_t)ntohl((x) & 0xFFFFFFFF) << 32) | ntohl((x) >> 32))

#pragma pack(pop)

#endif // PROTOCOL_H
