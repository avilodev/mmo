#ifndef TYPES_H
#define TYPES_H

#include "headers.h"

#define USERS_DB "database/databases/users_data/users.db"

#define MIN_HEADER_SIZE 7 
#define AUTH_REGISTER_SIZE 215

// Server Config 
#define LOGIN_SERVER_PORT 7776
#define REALM_SERVER_PORT 7777
#define WORLD_SERVER_PORT 7778
  
#define MAX_PLAYERS 1000 
#define MAX_PENDING_CONNECTIONS 10
#define TICK_RATE 60.0f 
#define TIMEOUT_SECONDS 30.0f
#define SESSION_EXPIRY_SECONDS 300 

#define MAX_PACKET_SIZE 8192

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

    PACKET_CAST_CANCEL = 73,
    PACKET_ATTACK_INTENT    = 74,
    PACKET_CAST_START_V2    = 75,
    PACKET_DAMAGE_V2        = 76,
    PACKET_ATTACK_RESULT    = 77, 

    PACKET_ABILITY_CAST_INTENT  = 80,   // Client -> Server: I want to cast ability X
    PACKET_ABILITY_CAST_START   = 81,   // Server -> Client: Cast begun (broadcast)
    PACKET_ABILITY_EFFECT       = 82,   // Server -> Client: Damage/heal/buff applied
    PACKET_ABILITY_CAST_CANCEL  = 83,   // Bidirectional: Cast interrupted/cancelled
    PACKET_STATUS_EFFECT_APPLY  = 84,   // Server -> Client: New status effect on target
    PACKET_STATUS_EFFECT_REMOVE = 85,   // Server -> Client: Status effect expired
    PACKET_SPAWN_ZONE           = 86,   // Server -> Client: Zone entity created
    PACKET_REMOVE_ZONE          = 87,   // Server -> Client: Zone entity removed
    PACKET_MANA_UPDATE          = 88,   // Server -> Client: Mana changed

    PACKET_LEVEL_UP          = 89,   // Server -> Client: Level up notification
    PACKET_PLAYER_STATS      = 91,   // Server -> Client: Full stat snapshot
    PACKET_REQUEST_PLAYER_STATS = 92,

    PACKET_NPC_POSITIONS = 97,
    
    
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

    // Dialogue packets (130-139)
    PACKET_NPC_INTERACT_REQUEST = 130,      // Client -> Server
    PACKET_NPC_INTERACT_RESPONSE = 131,     // Server -> Client
    PACKET_DIALOGUE_OPTION_SELECT = 132,    // Client -> Server
    PACKET_DIALOGUE_UPDATE = 133,           // Server -> Client
    PACKET_DIALOGUE_CLOSE = 134,            // Bidirectional

    // SERVER-TO-SERVER PACKETS (200-219)
    PACKET_REALM_AUTH = 200,
    PACKET_REALM_AUTH_ACK = 201,
    PACKET_WORLD_HEARTBEAT = 202,
    PACKET_WORLD_STATUS = 203,
} PacketType;

typedef enum {
    ATTACK_TYPE_SINGLE  = 0,    // Single nearest target
    ATTACK_TYPE_AOE     = 1,    // Radius around attacker
    ATTACK_TYPE_CONE    = 2,    // Cone in aim direction
    ATTACK_TYPE_LINE    = 3     // Piercing line in aim direction
} AttackType;

typedef enum {
    ATTACK_RESULT_OK            = 0,    // At least one target was hit
    ATTACK_RESULT_ON_COOLDOWN   = 1,    // Attacker is still on cooldown
    ATTACK_RESULT_ALREADY_CASTING = 2,  // Attacker is mid-cast
    ATTACK_RESULT_NO_TARGETS    = 3,    // No targets in range
    ATTACK_RESULT_INVALID       = 4     // Generic failure
} AttackResultCode;

#endif