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
    PACKET_CONNECT = 1,
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
    PACKET_PLAYER_MOVEMENT = 57,
    PACKET_PLAYER_MOVEMENT_ACK = 58,

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

    PACKET_NPC_POSITIONS = 97,

    PACKET_PLAYER_STATS         = 91,
    PACKET_REQUEST_PLAYER_STATS = 92,

    
    // Equipment packets (100-119)
    PACKET_EQUIP_ITEM = 100,
    PACKET_EQUIP_ITEM_RESPONSE = 101,
    PACKET_UNEQUIP_ITEM = 102,
    PACKET_UNEQUIP_ITEM_RESPONSE = 103,
    PACKET_UPDATE_EQUIPMENT = 104,
    PACKET_EQUIPMENT_DURABILITY = 105,
    
    // Inventory packets (120-139)
    PACKET_INVENTORY_UPDATE = 120,
    PACKET_ADD_ITEM = 121,
    PACKET_REMOVE_ITEM = 122,
    PACKET_MOVE_ITEM = 123,
    PACKET_MOVE_ITEM_RESPONSE = 124,
    PACKET_USE_ITEM = 125,
    PACKET_USE_ITEM_RESPONSE = 126,
    PACKET_DROP_ITEM = 127,
    PACKET_DROP_ITEM_RESPONSE = 128,
    
    // SERVER-TO-SERVER PACKETS (200-219)
    PACKET_REALM_AUTH = 200,
    PACKET_REALM_AUTH_ACK = 201,
    PACKET_WORLD_HEARTBEAT = 202,
    PACKET_WORLD_STATUS = 203,
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
    uint32_t player_id;
    char username[32];
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

typedef struct {

} RealmPing;

//
// World list packets
typedef struct {
    PacketHeader header;
} WorldListRequestPacket;
 
// World list
typedef struct {
    char name[64];
    uint32_t world_id;
    uint16_t population;
    int16_t capacity;
    uint8_t status; // 0=offline, 1=online, 2=full
    char ip[16];
    uint16_t port;
    char region[32];
} WorldInfo;

typedef struct {
    PacketHeader header;
    uint8_t count;
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

// Server -> Client: Cast started
typedef struct {
    PacketHeader header;
    uint32_t caster_id;
    uint32_t target_id;
    float cast_time;         // How long the cast takes
    uint8_t action_type;     // 0 = auto-attack, 1+ = spell
} CastStartPacket;

// Server -> Client: Damage dealt
typedef struct {
    PacketHeader header;
    uint32_t attacker_id;
    uint32_t target_id;
    uint32_t damage;
    uint32_t new_health;     // Target's new health
} DamagePacket;

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
    int32_t      max_health;
    int32_t      max_mana;
    int32_t      current_health;
    int32_t      current_mana;
    float        move_speed;
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
    uint8_t padding[3];  // Must match server alignment
} NPCPositionData;

typedef struct {
    PacketHeader header;
    uint8_t npc_count;
    uint8_t padding[3];  // Must match server alignment
    NPCPositionData npcs[MAX_NPCS_PER_PACKET];
} NPCPositionPacket;

// Network byte order conversion for 64-bit values
#define htonll(x) ((1==htonl(1)) ? (x) : ((uint64_t)htonl((x) & 0xFFFFFFFF) << 32) | htonl((x) >> 32))
#define ntohll(x) ((1==ntohl(1)) ? (x) : ((uint64_t)ntohl((x) & 0xFFFFFFFF) << 32) | ntohl((x) >> 32))

#pragma pack(pop)

#endif // PROTOCOL_H