#ifndef HEADERS_H
#define HEADERS_H

#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <sys/time.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/types.h> 

#define MAX_WORLDS 10
#define MAX_CAST_TARGETS    16 
#define MAX_NPCS_PER_PACKET 32

#pragma pack(push, 1)

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

// Auth packet header (with session key)
typedef struct {
    uint8_t type;
    uint32_t player_id;
    uint16_t payload_size;
    char session_key[32];
} AuthPacketHeader;

// Login Server Packets (use AuthPacketHeader because they need session validation)
typedef struct {
    PacketHeader header;
    char username[32];
    char password[64];
} AuthLoginPacket;

typedef struct {
    PacketHeader header;           // Use PacketHeader, not AuthPacketHeader
    uint8_t success;
    uint32_t player_id;            // Rename from assigned_player_id
    char message[128];             // Rename from error_message and match size
} AuthLoginResponsePacket;

// Registration packet
typedef struct {
    PacketHeader header;    // 7 bytes
    char username[32];      // 32 bytes
    char password[64];      // 64 bytes
    char email[64];         // 64 bytes
    char birthday[16];      // 16 bytes
    uint8_t reserved[32];   // 32 bytes
} AuthRegisterPacket;       // Total: 215 bytes

// Registration response packet
typedef struct {
    PacketHeader header;    // 7 bytes
    uint8_t success;        // 1 byte
    uint32_t player_id;     // 4 bytes
    char message[128];      // 128 bytes
} AuthRegisterResponsePacket;  // Total: 140 bytes

// Patch notes packets
typedef struct {
    PacketHeader header;
    uint16_t start;
    uint16_t end;
} PatchNotesRequest;

// Patch notes packets
typedef struct {
    PacketHeader header;
    char buffer[4000];
} PatchNotesResponse;

// STAGE 2: Start game (creates session)
typedef struct {
    PacketHeader header;
    uint32_t player_id;  // From stage 1 response
    char username[32];   // For verification
} StartGameRequestPacket;
 
typedef struct {
    AuthPacketHeader header;  // NOW includes session key
    uint8_t success;
    char message[128];
} StartGameResponsePacket;


///////////////////////////////////////////////////////////////////////////////////

// Connect to realm server with session from login
typedef struct {
    AuthPacketHeader header;
} RealmConnectPacket;

typedef struct {
    PacketHeader header;
    uint8_t success;
    char message[128];
} RealmConnectAckPacket;

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
} WorldListRequestPacket;

typedef struct {
    PacketHeader header;
    uint8_t count;
    WorldInfo worlds[MAX_WORLDS];
} WorldListResponsePacket;

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

typedef struct {
    PacketHeader header;
    uint32_t character_id;
    uint32_t world_id;
} WorldPlayerDataRequest;

// Character information structure (with equipment)
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

#pragma pack(pop)

// Active player in world (in-memory)
typedef struct {
    uint32_t character_id;
    uint32_t account_id;
    int client_fd;

    Class player_class;
    Race player_race;
    
    char username[32];
    float pos_x, pos_y;
    float vel_x, vel_y;
    int level;
    int health;
    int max_health;
    int32_t mana;          
    int32_t max_mana;  
    uint64_t experience;
    uint32_t gold;
    
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

    // Combat 
    double      last_attack_time;   
    float       attack_cooldown;    // Seconds remaining before next attack allowed

    int is_loaded;
    int is_dirty;
    time_t last_save;
    time_t last_activity;
    struct timeval last_move_tv;

    // Ability system
    float       ability_cooldowns[5];   // Remaining CD per slot (index = slot 0-4)
    uint16_t    ability_slots[5];       // Which ability IDs are equipped (by class)
    uint8_t     ability_count;          // How many abilities this player has

    // Derived class stats (recomputed on load/level-up, NOT persisted)
    int         strength;
    int         agility;
    int         intelligence;
    int         wisdom;
    int         defense;
    int         evasion;
    int         vitality;
    int         luck;
    float       move_speed;
    int         weapon_damage;      // Sum of equipped weapon damage values
    double      last_combat_time;   // For out-of-combat HP regen

    // Death/respawn
    uint8_t     is_dead;            // 1 = dead, waiting for respawn
    double      death_time;         // When the player died (CLOCK_MONOTONIC)

    // Active status effects on this player
    // (Kept simple — expand as needed)
    #define MAX_ACTIVE_EFFECTS 8
    struct {
        uint8_t     active;
        uint8_t     effect_type;        // StatusEffectType
        int         value;
        float       duration_remaining;
        float       tick_remaining;     // Time until next DOT/HOT tick
        float       tick_rate;
        uint32_t    source_id;          // Who applied this
    } active_effects[MAX_ACTIVE_EFFECTS];

    // Consumable cooldown
    double      last_consumable_time;  // CLOCK_MONOTONIC timestamp of last consumable use

    // Party
    uint32_t    party_id;              // 0 = not in a party

    pthread_mutex_t lock;
} ActivePlayer;

#pragma pack(push, 1)
/////////////////////////////////////////////////////////////////////////

// Enter world
typedef struct {
    PacketHeader header;
    uint32_t character_id;
    uint32_t world_id;
} EnterWorldPacket;

typedef struct {
    PacketHeader header;
    uint8_t success;
    char game_ticket[64]; // Short-lived token for world server
    char world_ip[16];
    uint16_t world_port;
    char message[128];
} EnterWorldResponsePacket;

// Connect to world server with game ticket
typedef struct {
    PacketHeader header;
    char game_ticket[64];
    uint32_t character_id;
} WorldConnectPacket;

typedef struct {
    PacketHeader header;
    uint8_t success;
    char welcome_message[128];
} WorldConnectAckPacket;

///////////////////////////////////////////////////////////////////////////////////
// SERVER-TO-SERVER COMMUNICATION
///////////////////////////////////////////////////////////////////////////////////

// Realm server authenticates to world server
typedef struct {
    PacketHeader header;
    char server_key[128];
    char realm_name[32];
} RealmAuthPacket;

typedef struct {
    PacketHeader header;
    uint8_t success;
    char message[64];
} RealmAuthAckPacket;

// Heartbeat from realm to world
typedef struct {
    PacketHeader header;
    uint64_t timestamp;
} WorldHeartbeatPacket;

// World server status response
typedef struct {
    PacketHeader header;
    char server_name[64];
    uint32_t player_count;
    uint32_t max_players;
    uint8_t status; // 0=offline, 1=online, 2=maintenance
    float cpu_usage;
    uint64_t uptime;
} WorldStatusPacket;

// World server registry (for realm server's internal use)
typedef struct {
    char name[64];
    char host[64];
    uint16_t port;
    int fd;
    uint8_t online;
    int connection_logged;
    uint32_t player_count;
    uint32_t max_players;
    time_t last_heartbeat;
} WorldServer;

// Client -> Server: Equip item from inventory
typedef struct {
    PacketHeader header;
    uint32_t item_id;
    uint8_t inventory_slot;  // Slot in inventory
    uint8_t equip_slot;      // Which equipment slot to equip to
    uint8_t padding[2];
} EquipItemPacket;

// Server -> Client: Equip result
typedef struct {
    PacketHeader header;
    uint8_t success;
    uint8_t padding[3];
    uint32_t equipped_item;  // Item that was equipped
    uint32_t returned_item;  // Item that was returned to inventory (if any)
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
    uint8_t inventory_slot;  // Where item was placed in inventory
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
    uint8_t effect_type;       // UseEffectType — what effect was applied
    uint8_t padding[2];
    int32_t health_changed;    // Amount of health restored (0 if none)
    int32_t mana_changed;      // Amount of mana restored (0 if none)
    int32_t new_health;
    int32_t new_mana;
    uint32_t item_id;          // Which item was consumed
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
// PLAYER DATA PACKET (Enhanced with equipment/inventory)
// ============================================================================

// Client -> Server: Request full player data
typedef struct {
    PacketHeader header;
} RequestPlayerDataPacket;

// Server -> Client: Full player data including equipment and inventory
typedef struct {
    PacketHeader header;
    
    // Basic stats
    uint32_t level;
    uint32_t health;
    uint32_t max_health;
    uint64_t experience;
    uint32_t gold;
    
    // Position
    float pos_x;
    float pos_y;
    float pos_z;
    
    // Equipment (item IDs)
    uint32_t helmet;
    uint32_t gloves;
    uint32_t chest_armor;
    uint32_t leggings;
    uint32_t boots;
    uint32_t main_hand;
    uint32_t second_hand;
    uint16_t blessing;
    uint16_t padding;
    
    // Inventory (150 slots)
    uint32_t inventory[150];
} PlayerDataPacket;

// ============================================================================
// MOVEMENT PACKET
// ============================================================================

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

typedef struct {
    PacketHeader header;
    uint32_t     caster_id;
    float        cast_time;
    uint8_t      attack_type;
    uint8_t      target_count;
    uint32_t     target_ids[MAX_CAST_TARGETS];
    float        origin_x;
    float        origin_y;
    float        aim_x;
    float        aim_y;
} CastStartV2Packet;

typedef struct {
    PacketHeader header;
    uint32_t     attacker_id;
    uint32_t     target_id;
    uint32_t     damage;
    uint32_t     target_new_health;
    uint8_t      is_kill;
} DamageV2Packet;

typedef struct {
    PacketHeader header;
    uint8_t      result_code;   // AttackResultCode enum
} AttackResultPacket;

typedef struct {
    PacketHeader header;
    float        aim_x;         // World X the player is aiming at
    float        aim_y;         // World Y the player is aiming at
} AttackIntentPacket;

// Server → Client: Cast cancelled/interrupted
typedef struct {
    PacketHeader header;
    uint32_t caster_id;
    uint8_t reason;          // 0=manual cancel, 1=moved, 2=interrupted
} CastCancelPacket;

// ============================================================================
// ABILITY SYSTEM PACKETS
// ============================================================================

#define MAX_ABILITY_SLOTS 5

// Client -> Server: Request to cast an ability
typedef struct {
    PacketHeader header;
    uint16_t     ability_id;        // Numeric ID from ability_def
    float        aim_x;             // World position aimed at
    float        aim_y;
    uint32_t     target_id;         // Target entity (0 if ground-targeted/self)
} AbilityCastIntentPacket;

// Server -> Client: Ability cast has started (broadcast to nearby)
typedef struct {
    PacketHeader header;
    uint32_t     caster_id;
    uint16_t     ability_id;
    float        cast_time;         // So client knows how long to show cast bar
    float        origin_x;
    float        origin_y;
    float        aim_x;
    float        aim_y;
} AbilityCastStartPacket;

// Server -> Client: Ability effect applied (damage, heal, etc.)
typedef struct {
    PacketHeader header;
    uint32_t     caster_id;
    uint32_t     target_id;
    uint16_t     ability_id;
    int32_t      damage;            // Positive = damage dealt
    int32_t      healing;           // Positive = healing done
    int32_t      target_new_health;
    uint8_t      is_kill;
} AbilityEffectPacket;

// Bidirectional: Cast cancelled
typedef struct {
    PacketHeader header;
    uint32_t     caster_id;
    uint16_t     ability_id;
    uint8_t      reason;            // 0=manual, 1=moved, 2=interrupted, 3=no_mana
} AbilityCastCancelPacket;

// Server -> Client: Status effect applied to a target
typedef struct {
    PacketHeader header;
    uint32_t     target_id;
    uint8_t      effect_type;       // StatusEffectType enum
    int32_t      value;
    float        duration;
    uint32_t     source_id;         // Who applied it
} StatusEffectApplyPacket;

// Server -> Client: Status effect removed
typedef struct {
    PacketHeader header;
    uint32_t     target_id;
    uint8_t      effect_type;
} StatusEffectRemovePacket;

// Server -> Client: Zone entity spawned
typedef struct {
    PacketHeader header;
    uint32_t     zone_id;           // Server-assigned ID for this zone
    uint32_t     caster_id;
    uint16_t     ability_id;        // So client knows which VFX to play
    float        pos_x;
    float        pos_y;
    float        duration;          // How long it lasts
    float        radius;            // AOE radius (for circle zones)
    uint8_t      has_collision;
} SpawnZonePacket;

// Server -> Client: Zone entity removed
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
// LEVELING DATA
// ============================================================================

// Server -> Client: Player leveled up
typedef struct {
    PacketHeader header;
    uint32_t     new_level;
    uint32_t     new_max_health;
    uint32_t     new_max_mana;
    uint32_t     new_health;        // Healed to full on level up
    uint32_t     new_mana;          // Restored to full on level up
    int32_t      strength;
    int32_t      agility;
    int32_t      intelligence;
    int32_t      wisdom;
    int32_t      defense;
    int32_t      evasion;
    int32_t      vitality;
    int32_t      luck;
    uint64_t     xp_for_next_level; // So client can show XP bar
} LevelUpPacket;

// Server -> Client: Full stat snapshot (sent on login)
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


// ============================================================================
// NPC DATA
// ============================================================================

// Add these structures to types.h
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
    uint8_t padding[3];  // Alignment padding
    NPCPositionData npcs[MAX_NPCS_PER_PACKET];
} NPCPositionPacket;

// ============================================================================
// DIALOGUE SYSTEM (Client-side text storage)
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
    char     npc_name[32];              // Keep: dynamic per NPC instance
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
// PROJECTILE PACKETS
// ============================================================================

#define MAX_PROJECTILES_PER_PACKET 32

// Server -> Client: A new projectile was spawned (client starts rendering)
typedef struct {
    PacketHeader header;
    uint32_t projectile_id;
    uint16_t ability_id;        // Client uses for VFX lookup
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

// Server -> Client: Batch update of visible projectile positions (30Hz)
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
    uint32_t killer_id;         // NPC or player who killed them (0 = environment)
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
    uint32_t ground_item_id;    // Unique ID for this ground item instance
    uint32_t item_id;           // Item definition ID (from items.json)
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
    uint8_t  success;           // 1=picked up, 0=failed
    uint32_t ground_item_id;
    uint32_t item_id;
    uint8_t  quantity;
    uint8_t  inventory_slot;    // Where it was placed
    char     message[64];
} LootPickupResponsePacket;

// Server -> Client: Ground item disappeared (picked up by someone else or despawned)
typedef struct {
    PacketHeader header;
    uint32_t ground_item_id;
} LootDespawnPacket;

// ============================================================================
// PLAYER POSITION BROADCAST
// ============================================================================

#define MAX_NEARBY_PLAYERS 32

// Single player entry in batch position update
typedef struct {
    uint32_t player_id;
    float    pos_x, pos_y;
    int32_t  health;
    int32_t  max_health;
    uint8_t  player_class;
    uint8_t  is_dead;
    uint8_t  padding[2];
} NearbyPlayerData;

// Server -> Client: Batch update of nearby player positions (20Hz)
typedef struct {
    PacketHeader header;
    uint8_t count;
    uint8_t padding[3];
    NearbyPlayerData players[MAX_NEARBY_PLAYERS];
} PlayerPositionBroadcastPacket;

// ============================================================================
// NPC TELEGRAPH PACKETS (FF14-style ground indicators)
// ============================================================================

// Server -> Client: NPC started casting — show ground indicator
typedef struct {
    PacketHeader header;
    uint32_t npc_id;            // Which NPC is casting
    uint16_t ability_id;        // Client uses for VFX lookup
    uint8_t  shape;             // 0=circle, 1=cone, 2=rectangle, 3=line
    float    pos_x, pos_y;      // Center of the telegraph zone
    float    dir_x, dir_y;      // Direction (for cone/rect/line; ignored for circle)
    float    radius;            // Circle/cone radius
    float    angle;             // Cone angle in degrees
    float    width;             // Rectangle/line width
    float    length;            // Rectangle/line length
    float    cast_time;         // Seconds until it resolves (client counts down)
} NPCTelegraphStartPacket;

// Server -> Client: NPC telegraph resolved — show impact VFX
typedef struct {
    PacketHeader header;
    uint32_t npc_id;
    uint16_t ability_id;
} NPCTelegraphResolvePacket;

// ============================================================================
// HELPER MACROS
// ============================================================================
// CHAT PACKETS
// ============================================================================

#define MAX_CHAT_MESSAGE 256

// Chat channel types
typedef enum {
    CHAT_CHANNEL_LOCAL  = 0,   // Nearby players only
    CHAT_CHANNEL_GLOBAL = 1,   // All players on server
    CHAT_CHANNEL_WHISPER = 2,  // Private message (future)
    CHAT_CHANNEL_PARTY  = 3,   // Party members only
} ChatChannel;

// Client -> Server: Player sends a chat message
typedef struct {
    PacketHeader header;
    uint8_t  channel;          // ChatChannel
    uint8_t  padding[3];
    char     message[MAX_CHAT_MESSAGE];
} ChatSendPacket;

// Server -> Client: Chat message broadcast
typedef struct {
    PacketHeader header;
    uint32_t sender_id;
    uint8_t  channel;          // ChatChannel
    uint8_t  padding[3];
    char     sender_name[32];
    char     message[MAX_CHAT_MESSAGE];
} ChatMessagePacket;

// ============================================================================

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

// Network byte order conversion for 64-bit values
#define htonll(x) ((1==htonl(1)) ? (x) : ((uint64_t)htonl((x) & 0xFFFFFFFF) << 32) | htonl((x) >> 32))
#define ntohll(x) ((1==ntohl(1)) ? (x) : ((uint64_t)ntohl((x) & 0xFFFFFFFF) << 32) | ntohl((x) >> 32))

#pragma pack(pop)

#endif