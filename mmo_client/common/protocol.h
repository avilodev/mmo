#ifndef PROTOCOL_H
#define PROTOCOL_H

/** @file Define the packed wire protocol shared by MMO clients and servers. */

#include <stdint.h>

/* CURRENCY_COUNT sizes the player's balance array on the wire. */
#include "world_regions.h"

/** Identify the wire contract this build speaks.
 *
 * The client and server trees each carry their own copy of this file and are
 * built, shipped, and run independently — neither tree includes from the other.
 * That independence is deliberate, and it is also why nothing structural can
 * catch the two copies drifting apart. This number is what catches it: the
 * client states its version when it connects, the server compares, and a
 * mismatch is refused with DISCONNECT_REASON_VERSION instead of both ends
 * quietly misparsing each other's bytes.
 *
 * Bump this in BOTH copies whenever a packet layout, an opcode value, or an
 * enum that travels on the wire changes. tests/check_protocol_version.sh (and
 * the client's equivalent) fails the build when this file changes without it.
 */
#define PROTOCOL_VERSION 9

/* Byte order: every multi-byte INTEGER on the wire is in network order
 * (htonl/htons); every FLOAT is native-endian and memcpy'd as-is, which is
 * correct only while both ends are little-endian. The server tree's copy of
 * this file lists the native-endian fields exhaustively. Anything added here
 * uses network order unless it has the same reason those have. */

#define MAX_WORLDS 10
#define MAX_CAST_TARGETS    16
#define MAX_NPCS_PER_PACKET 32
#define MAX_ABILITY_SLOTS    5

#pragma pack(push, 1)

/** Assign wire opcodes to protocol messages. */
typedef enum {
    PACKET_DISCONNECT = 2,
    PACKET_AUTH_LOGIN = 3,              // Username/password validation (no session created)
    PACKET_AUTH_REGISTER = 4,
    PACKET_AUTH_RESPONSE = 5,           // Success/fail (no session yet)
    PACKET_START_GAME_REQUEST = 6,      // Request to start game (creates session)
    PACKET_START_GAME_RESPONSE = 7,     // Returns session key + player ID
    PACKET_RATE_LIMITED = 8,            // Server -> Client: request dropped, over budget

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
    PACKET_RACE_LIST_REQUEST = 32,      // Client -> Server: what races exist?
    PACKET_RACE_LIST_RESPONSE = 33,     // Server -> Client: the loaded race registry

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

    PACKET_FORM_SWAP            = 93,   // Client -> Server: request a Human/Animal form swap
    PACKET_FORM_SWAP_ACK        = 94,   // Server -> Client: authoritative form and hotbar

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
    PACKET_INVENTORY_UPDATE = 129,   // Server -> Client: authoritative slot contents

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

    /* Quest management (204-205). The quest block at 191-193 is full, and
     * these two are the only quest traffic that starts at the client: quests
     * are taken and handed in through dialogue, never by packet. */
    PACKET_QUEST_ABANDON   = 204,  // Client -> Server: give up an active quest
    PACKET_QUEST_ABANDONED = 205,  // Server -> Client: the quest left the log

    PACKET_SESSION_LIST_REQUEST  = 210, // Client -> Server: request page of online players
    PACKET_SESSION_LIST_RESPONSE = 211, // Server -> Client: paginated list of online players

    PACKET_ZONE_CHANGE = 220,           // Server -> Client: player crossed a zone boundary

    /* Name resolution (212-213).
     *
     * Nearby players arrive as identifiers with no names. The client asks for
     * the ones it does not recognise, once each, and caches the answers for
     * the session -- see Game/src/network/name_cache.c. A name field in every
     * broadcast would resend a constant 32 bytes per player, 20 times a
     * second. */
    PACKET_NAME_QUERY_REQUEST  = 212,   // Client -> Server: who are these ids?
    PACKET_NAME_QUERY_RESPONSE = 213,   // Server -> Client: their names

    /* Friends and presence (230-238).
     *
     * The friend graph is a graph of ACCOUNTS, not of characters: character ids
     * are per-world SERIALs, so character 5 exists in all ten worlds and only
     * account_id means anything outside one. Nothing on this wire carries a
     * friend's character id for that reason -- you friend a person and are shown
     * whichever character of theirs is online.
     *
     * Every client->server opcode here names its subject by NAME, because a
     * player types a name and has no way to know an account id. Resolution
     * happens on the server. */
    PACKET_FRIEND_REQUEST         = 230, // Client -> Server: befriend this name
    PACKET_FRIEND_RESPOND         = 231, // Client -> Server: accept or decline a request
    PACKET_FRIEND_REMOVE          = 232, // Client -> Server: remove a friend, or cancel a request
    PACKET_FRIEND_BLOCK           = 233, // Client -> Server: block or unblock an account
    PACKET_FRIEND_LIST_REQUEST    = 234, // Client -> Server: open the panel
    PACKET_FRIEND_LIST_RESPONSE   = 235, // Server -> Client: friends and their presence
    PACKET_FRIEND_REQUESTS_LIST   = 236, // Server -> Client: requests waiting to be answered
    PACKET_FRIEND_REQUEST_NOTIFY  = 237, // Server -> Client: somebody just asked
    PACKET_FRIEND_PRESENCE_UPDATE = 238, // Server -> Client: a friend came online or left
    PACKET_FRIEND_OP_RESULT       = 239, // Server -> Client: what became of your last action
} PacketType;

/** Bound race identifiers on the wire.
 *
 * Race and class fuse into one identifier under the Blessed model: a character's
 * class_id and race_id always hold the same value. Names, passives, specs and stat
 * growth live in world_server/data/races.json, so adding a race is a data change.
 * This constant exists only to size arrays and validate untrusted wire values.
 */
#define MAX_RACES 32

/** Identify the combat role a spec fills, which in turn selects its resource. */
typedef enum {
    ROLE_TANK   = 0,
    ROLE_DPS    = 1,
    ROLE_HEALER = 2,
    ROLE_COUNT
} CombatRole;

/** Identify which of a character's two forms is active. */
typedef enum {
    FORM_HUMAN  = 0,
    FORM_ANIMAL = 1,
    FORM_COUNT
} PlayerForm;

/** Identify the resource pool a character spends, derived from its role. */
typedef enum {
    RESOURCE_NONE    = 0,   // Human Form: cooldown-only, no pool
    RESOURCE_MANA    = 1,   // ROLE_HEALER, driven by Focus
    RESOURCE_STAMINA = 2,   // ROLE_DPS, driven by Stamina Capacity
    RESOURCE_RAGE    = 3,   // ROLE_TANK, driven by Endurance; builds instead of draining
    RESOURCE_COUNT
} ResourceType;

/** Index a character attribute.
 *
 * Stats travel the wire as a dense array indexed by this enum rather than as named
 * fields, so adding a stat is one entry here plus one key in races.json. Keep the
 * order stable: it is the wire order.
 */
typedef enum {
    STAT_STRENGTH         = 0,  // Physical ability damage
    STAT_DEXTERITY        = 1,  // Attack and cast speed
    STAT_VITALITY         = 2,  // Max health
    STAT_INTELLIGENCE     = 3,  // Magical ability damage
    STAT_FOCUS            = 4,  // Max mana and regen — the healer resource stat
    STAT_ENDURANCE        = 5,  // Rage generation and retention — the tank resource stat
    STAT_FEROCITY         = 6,  // Critical damage multiplier
    STAT_STAMINA_CAPACITY = 7,  // Max stamina and regen — the DPS resource stat
    STAT_PRECISION        = 8,  // Critical strike chance
    STAT_FERALITY         = 9,  // Animal Form power scalar
    STAT_ARMOR            = 10, // Flat damage reduction
    STAT_COUNT
} StatId;

/** Prefix every ordinary packet with its opcode, player identifier, and payload length. */
typedef struct {
    uint8_t type;           // 1 byte
    uint32_t player_id;     // 4 bytes
    uint16_t payload_size;  // 2 bytes
} PacketHeader;             // Total: 7 bytes (no padding)

/** Describe one race to the character-creation screen.
 *
 * The client holds no table of races. It asks the server, which answers from the
 * registry it loaded from races.json — so adding a race really does require editing
 * races.json and abilities.json and nothing else, the client included.
 */
typedef struct {
    uint32_t race_id;           // Fused race/class identifier
    char     key[32];           // Stable machine name, e.g. "wolf"
    char     name[32];          // Display name
    char     latin[32];         // Latin name, shown beneath the display name
    char     passive_name[32];
    char     passive_desc[192];
    uint8_t  playable;          // 0 greys the entry out at character creation
    uint8_t  default_role;      // CombatRole of the starting spec
    uint8_t  spec_count;
    uint8_t  _reserved;         // must be 0
    uint8_t  spec_roles[4];     // CombatRole per spec, in declaration order
    uint8_t  spec_unlock[4];    // Level gate per spec
} RaceInfo;

/** Bound the race list carried by one packet. Mirrors MAX_RACES. */
#define MAX_RACE_LIST 32

/** Ask the realm server for the race registry. */
typedef struct {
    PacketHeader header;
} RaceListRequestPacket;

/** Return every loaded race, playable or not.
 *
 * Non-playable races are included deliberately: the creation screen shows them greyed
 * out, so a player can see what is coming without the client needing to know which
 * races exist ahead of time.
 */
typedef struct {
    PacketHeader header;
    uint8_t      count;
    uint8_t      _reserved[3];  // must be 0
    RaceInfo     races[MAX_RACE_LIST];
} RaceListResponsePacket;

/** Identify reasons supplied before a server closes a client connection. */
typedef enum {
    DISCONNECT_REASON_UNKNOWN     = 0,
    DISCONNECT_REASON_SHUTDOWN    = 1,   // server going down
    DISCONNECT_REASON_RATE_LIMIT  = 2,   // sustained packet budget abuse
    DISCONNECT_REASON_PROTOCOL    = 3,   // malformed or oversized packet
    DISCONNECT_REASON_AUTH        = 4,   // session invalid or expired
    DISCONNECT_REASON_KICKED      = 5,   // administrative
    DISCONNECT_REASON_VERSION     = 6    // client and server speak different PROTOCOL_VERSIONs
} DisconnectReason;

/** Describe a server-initiated disconnect before closing the socket. */
typedef struct {
    PacketHeader header;
    uint8_t      reason;        // DisconnectReason
    char         message[128];  // human-readable; may be empty
} DisconnectPacket;

/** Report a request rejected by the server's packet budget. */
typedef struct {
    PacketHeader header;
    uint8_t      rejected_type;    // opcode that was dropped
    uint8_t      limit_class;      // which budget it exhausted
    uint16_t     retry_after_ms;   // hint for the client; not enforcement
} RateLimitedPacket;

/** Assign zone categories shared with the server zone system. */
#define ZONE_TYPE_WILD    0
#define ZONE_TYPE_SAFE    1
#define ZONE_TYPE_DUNGEON 2
#define ZONE_TYPE_PVP     3

/** Announce a player's transition into a named zone. */
typedef struct {
    PacketHeader header;
    uint8_t      zone_id;
    uint8_t      zone_type;
    char         zone_name[48];
} ZoneChangePacket;

/** Prefix authenticated packets with a fixed-width session key. */
typedef struct {
    uint8_t type;
    uint32_t player_id;
    uint16_t payload_size;
    char session_key[32];
} AuthPacketHeader;

/** Request an inclusive range of patch-note records. */
typedef struct {
    PacketHeader header;
    uint16_t start;
    uint16_t end;
} PatchNotesRequest;

/** Return patch-note text in a fixed-capacity buffer. */
typedef struct {
    PacketHeader header;
    char buffer[4000];
} PatchNotesResponse;

/** Submit credentials for validation without creating a session. */
typedef struct {
    PacketHeader header;
    char username[32];
    char password[64];
} AuthLoginPacket;

/** Return login validation status and a single-use authentication token. */
typedef struct {
    PacketHeader header;
    uint8_t success;
    uint32_t player_id;
    char auth_token[32];     // Single-use token required by START_GAME_REQUEST
    char message[128];
} AuthLoginResponsePacket;

/** Submit fixed-width account registration fields. */
typedef struct {
    PacketHeader header;
    char username[32];
    char password[64];
    char email[64];
    char birthday[16];        // Format: "YYYY-MM-DD"
    uint8_t reserved[32];     // Reserved for future fields
} AuthRegisterPacket;

/** Return account registration status without creating a session. */
typedef struct {
    PacketHeader header;      // Basic header without session key
    uint8_t success;          // 1 = success, 0 = failure
    uint32_t player_id;       // New player ID (in network byte order)
    char message[128];        // Success or error message
} AuthRegisterResponsePacket;

/** Exchange a single-use login token for a game session. */
typedef struct {
    PacketHeader header;
    uint32_t player_id;      // Informational only; server trusts auth_token
    char username[32];       // Informational only
    char auth_token[32];     // Single-use proof of successful login
} StartGameRequestPacket;

/** Return session creation status and the authenticated header. */
typedef struct {
    AuthPacketHeader header;
    uint8_t success;
    char message[128];
} StartGameResponsePacket;

/** Authenticate the game client to the realm service.
 *
 * Carries the client's PROTOCOL_VERSION so a mismatched build is turned away at
 * the first packet, before it can misread any later one.
 */
typedef struct {
    AuthPacketHeader header;
    uint16_t protocol_version;   /**< Network byte order. */
} RealmConnectPacket;

/** Acknowledge the game client's realm connection. */
typedef struct {
    PacketHeader header;
    uint8_t success;
    char message[128];
} RealmConnectAckPacket;

/** Request the realm's current world list. */
typedef struct {
    PacketHeader header;
} WorldListRequestPacket;

/** Describe one world endpoint with explicit cross-platform padding.
 *
 * `host` and `region` are as wide as the realm's own WorldServer record. They
 * were 16 and 32 bytes against a 64-byte source, so a world configured by
 * hostname reached clients truncated and unreachable. */
typedef struct {
    char name[64];
    uint32_t world_id;
    uint16_t population;
    /** Unsigned, like `population`; the value behind it is a uint32_t. */
    uint16_t capacity;
    uint8_t status; // 0=offline, 1=online, 2=full
    uint8_t padding1[3];    // EXPLICIT PADDING ADDED
    /** Hostname or address literal; the client resolves it. */
    char host[64];
    uint16_t port;
    uint8_t padding2[2];    // EXPLICIT PADDING ADDED
    char region[64];
} WorldInfo;

/** Return up to MAX_WORLDS world descriptions. */
typedef struct {
    PacketHeader header;
    uint8_t count;
    uint8_t padding[3];
    WorldInfo worlds[MAX_WORLDS];
} WorldListResponsePacket;
/** Request characters belonging to an account on one world. */
typedef struct {
    PacketHeader header;
    uint32_t world_id;
} CharacterListRequestPacket;

/** Return the account's characters for one world. */
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

/** Request creation of a named character on one world. */
typedef struct {
    PacketHeader header;
    uint32_t world_id;
    char name[32];
    uint32_t class_id;
    uint32_t race_id;
} CharacterCreateRequestPacket;

/** Return the result and assigned identifier of character creation. */
typedef struct {
    PacketHeader header;
    uint32_t world_id;
    uint8_t success;
    uint32_t character_id;
    char character_name[32];
    char message[128];
} CharacterCreateResponsePacket;

/** Request deletion of a character from one world. */
typedef struct {
    PacketHeader header;
    uint32_t character_id;
    uint32_t world_id;
} CharacterDeleteRequestPacket;

/** Return the result of a character deletion request. */
typedef struct {
    PacketHeader header;
    uint32_t character_id;
    uint32_t world_id;
    uint8_t success;
    char message[128];
} CharacterDeleteResponsePacket;

/** Request entry to a world with a selected character. */
typedef struct {
    PacketHeader header;
    uint32_t character_id;
    uint32_t world_id;
} EnterWorldPacket;

/** Return a world endpoint and short-lived admission ticket. */
typedef struct {
    PacketHeader header;
    uint8_t success;
    char game_ticket[64];     // Short-lived token for world server
    /** Hostname or address literal the client dials; resolved client-side.
     *  Sized from the realm's WorldServer.host -- this is the field a world
     *  entry connects to, so a truncation here was a world nobody could
     *  reach. */
    char world_host[64];
    uint16_t world_port;
    char message[128];
} EnterWorldResponsePacket;

/** Present a realm-issued ticket to a world server.
 *
 * Carries the client's PROTOCOL_VERSION for the same reason RealmConnectPacket
 * does: a world entry is the last point at which a version mismatch can still
 * be reported as a version mismatch rather than as corrupt gameplay.
 */
typedef struct {
    PacketHeader header;
    char game_ticket[64];
    uint32_t character_id;
    uint16_t protocol_version;   /**< Network byte order. */
} WorldConnectPacket;

/** Acknowledge admission to a world server. */
typedef struct {
    PacketHeader header;
    uint8_t success;
    char welcome_message[128];
} WorldConnectAckPacket;

/** Authenticate a realm server to a world server. */
typedef struct {
    PacketHeader header;
    char server_key[128];
    char realm_name[32];
} RealmAuthPacket;

/** Acknowledge realm-to-world authentication. */
typedef struct {
    PacketHeader header;
    uint8_t success;
    char message[64];
} RealmAuthAckPacket;

/** Carry a realm-to-world heartbeat timestamp. */
typedef struct {
    PacketHeader header;
    uint64_t timestamp;
} WorldHeartbeatPacket;

/** Report a world server's capacity, load, status, and uptime. */
typedef struct {
    PacketHeader header;
    char server_name[64];
    uint32_t player_count;
    uint32_t max_players;
    uint8_t status;
    float cpu_usage;
    uint64_t uptime;
} WorldStatusPacket;

/** Request a character snapshot from a world server. */
typedef struct {
    PacketHeader header;
    uint32_t character_id;
    uint32_t world_id;
} WorldPlayerDataRequest;

/** Define the shared array order for equipped item slots. */
typedef enum {
    EQUIP_HELMET = 0,
    EQUIP_GLOVES,
    EQUIP_CHEST,
    EQUIP_LEGGINGS,
    EQUIP_BOOTS,
    EQUIP_MAIN_HAND,
    EQUIP_SECOND_HAND,
    EQUIP_BLESSING,
    EQUIP_SLOTS
} EquipSlotIndex;

#define INVENTORY_SLOT_COUNT 150

/** Place equipment slot identifiers above the inventory range. */
#define EQUIP_SLOT_BASE 200

/** Represent one fixed 16-byte inventory or equipment slot on the wire. */
typedef struct {
    uint64_t instance_id;   // 0 = empty slot
    uint32_t item_id;
    uint16_t quantity;
    uint8_t  is_bound;
    uint8_t  _reserved;     // keeps the struct at 16 bytes; must be 0
} InventorySlotData;

/** Carry a complete character, equipment, and inventory snapshot. */
typedef struct {
    PacketHeader header;

    uint32_t character_id;
    char name[32];
    uint32_t level;
    float pos_x;
    float pos_y;
    uint32_t health;
    uint32_t max_health;
    int32_t resource;
    int32_t max_resource;
    uint64_t experience;

    /** Balance per kingdom, indexed by CurrencyId. There is no universal coin. */
    uint32_t currency[CURRENCY_COUNT];

    uint32_t race_id;       // Fused race/class identifier; 1..loaded race count
    uint8_t  resource_type; // ResourceType, derived from the active spec's role
    uint8_t  form;          // PlayerForm currently active
    uint8_t  _reserved[2];  // keeps the struct naturally aligned; must be 0

    /** Empty slots use an instance identifier of zero. */
    InventorySlotData equipment[EQUIP_SLOTS];
    InventorySlotData inventory[INVENTORY_SLOT_COUNT];
} CharacterInfo;

/** Limit the number of authoritative slot changes carried by one packet. */
#define MAX_SLOT_UPDATES 8

/** Pair a database-compatible slot number with its authoritative contents. */
typedef struct {
    uint16_t          slot;
    InventorySlotData data;
} SlotUpdateEntry;

/** Return the authoritative contents of changed inventory or equipment slots. */
typedef struct {
    PacketHeader    header;
    uint8_t         count;          // how many entries are populated
    uint8_t         _pad[3];
    SlotUpdateEntry slots[MAX_SLOT_UPDATES];
} InventoryUpdatePacket;

_Static_assert(sizeof(SlotUpdateEntry) == 18,
               "SlotUpdateEntry must stay 18 bytes on every target");


/** Enforce packed layouts shared by the Windows client and Linux server. */
_Static_assert(sizeof(InventorySlotData) == 16,
               "InventorySlotData must stay 16 bytes on every target");
_Static_assert(sizeof(CharacterInfo) <= 8192,
               "CharacterInfo must fit inside MAX_PACKET_SIZE");
_Static_assert(sizeof(PacketHeader) == 7,
               "PacketHeader must stay 7 bytes");

/* The two handshake packets are the only ones whose layout must be agreed on
 * before a version can even be exchanged, so their sizes are pinned here. */
_Static_assert(sizeof(RealmConnectPacket) == 41,
               "RealmConnectPacket must stay 41 bytes on every target");
_Static_assert(sizeof(WorldConnectPacket) == 77,
               "WorldConnectPacket must stay 77 bytes on every target");



/** Submit a player's position and velocity.
 *
 * `sequence` numbers the proposal so a correction can name the one it answers.
 * See the client's player_apply_correction(): the client rewinds to the
 * corrected position and replays the moves it sent after that sequence, which
 * is what makes a correction it would have agreed with invisible instead of a
 * jerk. Network order.
 */
typedef struct {
    PacketHeader header;
    float pos_x;
    float pos_y;
    float vel_x;
    float vel_y;
    uint32_t sequence;
} PlayerMovePacket;

/** Correct a player's position, naming the proposal it rejects.
 *
 * Sent only on refusal; an accepted move is acknowledged by silence. Moves
 * after `sequence` are unresolved and are replayed from (pos_x, pos_y); the
 * move carrying `sequence` is the refused one and is dropped.
 */
typedef struct {
    PacketHeader header;
    float pos_x;
    float pos_y;
    uint32_t sequence;
} PlayerMoveAckPacket;

/** Identify supported attack-area shapes. */
typedef enum {
    ATTACK_TYPE_SINGLE = 0,
    ATTACK_TYPE_AOE    = 1,
    ATTACK_TYPE_CONE   = 2,
    ATTACK_TYPE_LINE   = 3,
    ATTACK_TYPE_COUNT  = 4
} AttackType;

/** Announce cancellation or interruption of an active cast. */
typedef struct {
    PacketHeader header;
    uint32_t caster_id;
    uint8_t reason;          // 0=manual cancel, 1=moved, 2=interrupted
} CastCancelPacket;

/** Identify the server result of an attack intent. */
typedef enum {
    ATTACK_RESULT_OK            = 0,    // At least one target was hit (damage packets follow)
    ATTACK_RESULT_ON_COOLDOWN   = 1,    // Attacker is still on cooldown
    ATTACK_RESULT_ALREADY_CASTING = 2,  // Attacker is mid-cast
    ATTACK_RESULT_NO_TARGETS    = 3,    // Resolved the area but nothing was there
    ATTACK_RESULT_INVALID       = 4     // Generic failure (attacker not found, etc.)
} AttackResultCode;

/** Request an attack aimed at a world position. */
typedef struct {
    PacketHeader header;        // header.player_id = attacker's character_id
    float        aim_x;         // World X the player is aiming at
    float        aim_y;         // World Y the player is aiming at
} AttackIntentPacket;

/** Announce the resolved targets and geometry of an attack cast. */
typedef struct {
    PacketHeader header;
    uint32_t     caster_id;                     // Who is attacking
    float        cast_time;                     // Seconds until damage resolves
    uint8_t      attack_type;                   // AttackType enum value
    uint8_t      target_count;                  // How many targets resolved (0..MAX_CAST_TARGETS)
    uint32_t     target_ids[MAX_CAST_TARGETS]; // Entity IDs that will be hit
    /** Preserve the cast-start origin for trajectory rendering. */
    float        origin_x;
    float        origin_y;
    float        aim_x;
    float        aim_y;
} CastStartV2Packet;

/** Report authoritative damage and health for one target. */
typedef struct {
    PacketHeader header;
    uint32_t     attacker_id;
    uint32_t     target_id;
    uint32_t     damage;            // Actual damage dealt (after variance)
    uint32_t     target_new_health; // Authoritative HP after hit
    uint8_t      is_kill;           // 1 if this hit brought target to 0
    uint8_t      is_crit;           // 1 if this was a critical hit
} DamageV2Packet;

/** Return the server's result for an attack intent. */
typedef struct {
    PacketHeader header;        // header.player_id = attacker's character_id
    uint8_t      result_code;   // AttackResultCode enum value
} AttackResultPacket;

/** Request an ability cast toward a position or entity. */
typedef struct {
    PacketHeader header;
    uint16_t     ability_id;
    float        aim_x;
    float        aim_y;
    uint32_t     target_id;     // 0 if ground-targeted/self
} AbilityCastIntentPacket;

/** Announce the authoritative start and geometry of an ability cast. */
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

/** Report an ability's authoritative damage, healing, and target health. */
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

/** Announce cancellation of an ability cast in either direction. */
typedef struct {
    PacketHeader header;
    uint32_t     caster_id;
    uint16_t     ability_id;
    uint8_t      reason;        // 0=manual, 1=moved, 2=interrupted, 3=no_mana
} AbilityCastCancelPacket;

/** Announce application of a timed status effect. */
typedef struct {
    PacketHeader header;
    uint32_t     target_id;
    uint8_t      effect_type;
    int32_t      value;
    float        duration;
    uint32_t     source_id;
} StatusEffectApplyPacket;

/** Announce removal of a status effect. */
typedef struct {
    PacketHeader header;
    uint32_t     target_id;
    uint8_t      effect_type;
} StatusEffectRemovePacket;

/** Announce an ability-created zone and its collision geometry. */
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

/** Announce removal of an ability-created zone. */
typedef struct {
    PacketHeader header;
    uint32_t     zone_id;
} RemoveZonePacket;

/** Synchronize a player's current and maximum mana. */
typedef struct {
    PacketHeader header;
    int32_t      mana;
    int32_t      max_mana;
} ManaUpdatePacket;

/** Describe one ability-bar slot sent on world entry, level-up, or form swap. */
typedef struct {
    uint16_t id;
    char     name[24];
    float    cooldown;
    float    cast_time;
    int16_t  resource_cost;  /**< Always 0 in Human Form, which has no pool. */
    char     image[32];  // Icon filename, e.g. "cleave.png" — looked up in Game/Sprites/Abilities/
} AbilitySlotInfo;

/** Synchronize the ability bar for one form.
 *
 * Both forms share the five hotbar slots; `form` says which kit these five are, so
 * the client can keep a bar per form and swap between them without a round trip.
 */
typedef struct {
    PacketHeader header;
    uint8_t  count;
    uint8_t  form;          /**< PlayerForm these slots belong to. */
    uint8_t  _reserved[2];  /**< Must be 0. */
    AbilitySlotInfo slots[MAX_ABILITY_SLOTS];
} AbilityDataPacket;

/** Request a swap to the other form. */
typedef struct {
    PacketHeader header;
    uint8_t      requested_form;  /**< PlayerForm the client wants to enter. */
} FormSwapPacket;

/** Confirm or refuse a form swap and restate the authoritative form state.
 *
 * Cooldowns are not carried here. They are stored as absolute expiry instants per
 * form and keep ticking while a form is inactive, so the client recomputes remaining
 * time from `ability_ready_in` without ever needing the swap to reset anything.
 */
typedef struct {
    PacketHeader header;
    uint8_t      accepted;        /**< 0 when refused, e.g. the swap cooldown is up. */
    uint8_t      form;            /**< Authoritative PlayerForm after the request. */
    uint8_t      resource_type;   /**< ResourceType for the new form. */
    uint8_t      _reserved;       /**< Must be 0. */
    int32_t      resource;
    int32_t      max_resource;
    float        swap_ready_in;   /**< Seconds until another swap is allowed. */
    /** Seconds remaining on each slot of the form now active; 0 when ready. */
    float        ability_ready_in[MAX_ABILITY_SLOTS];
} FormSwapAckPacket;

/** Synchronize level, attributes, resources, and the next experience threshold. */
typedef struct {
    PacketHeader header;
    uint32_t     new_level;
    uint32_t     new_max_health;
    uint32_t     new_max_resource;
    uint32_t     new_health;
    uint32_t     new_resource;
    int32_t      stats[STAT_COUNT];  /**< Indexed by StatId. */
    uint64_t     xp_for_next_level;
} LevelUpPacket;

/** Return a full authoritative player-stat snapshot. */
typedef struct {
    PacketHeader header;
    int32_t      stats[STAT_COUNT];  /**< Indexed by StatId. */
    int32_t      max_health;
    int32_t      max_resource;
    int32_t      current_health;
    int32_t      current_resource;
    float        move_speed;
    int32_t      weapon_damage;
    uint64_t     xp_for_next_level;
    uint8_t      resource_type;      /**< ResourceType. */
    uint8_t      form;               /**< PlayerForm. */
    uint8_t      _reserved[2];       /**< Must be 0. */
} PlayerStatsPacket;

/** Request a fresh authoritative player-stat snapshot. */
typedef struct {
    PacketHeader header;
} RequestPlayerStatsPacket;

/** Describe one NPC in a batched position update. */
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

/** Broadcast a bounded batch of NPC positions and combat states. */
typedef struct {
    PacketHeader header;
    uint8_t npc_count;
    uint8_t padding[3];  // Must match server alignment
    NPCPositionData npcs[MAX_NPCS_PER_PACKET];
} NPCPositionPacket;

/** Bound the choices one dialogue page may offer at once.
 *
 * This is the count a page can *show*, not the count it may declare: the server
 * hides options whose conditions the asking player fails, so a page written with
 * one branch per race stays inside this while still reading as a single line of
 * dialogue. A page that lets more than this through at once is an authoring
 * error, and the server says so rather than silently truncating.
 */
#define MAX_DIALOGUE_OPTIONS 6

/** Bound the text one page and one choice carry.
 *
 * Dialogue text travels on the wire rather than shipping with the client. That
 * is what keeps a single source of truth for what an NPC says -- the same rule
 * races, abilities, items and zones already follow -- and it is affordable
 * because these two packets are sent once per click and never in a broadcast.
 */
#define MAX_DIALOGUE_TEXT 1024
#define MAX_OPTION_TEXT    192

/** Request interaction with an NPC. */
typedef struct {
    PacketHeader header;
    uint32_t npc_id;
} NPCInteractRequestPacket;

/** Open an NPC dialogue, carrying the page's text and the choices offered. */
typedef struct {
    PacketHeader header;
    uint32_t npc_id;
    uint32_t dialogue_id;
    uint8_t  page_num;
    uint8_t  option_count;
    char     npc_name[32];
    /** Identify each offered choice. The client sends one of these back, never a
     *  row number: the rows differ between two players reading the same page. */
    uint8_t  option_ids[MAX_DIALOGUE_OPTIONS];
    uint8_t  padding[2];
    char     text[MAX_DIALOGUE_TEXT];
    char     option_text[MAX_DIALOGUE_OPTIONS][MAX_OPTION_TEXT];
} NPCInteractResponsePacket;

/** Submit a chosen option from the current dialogue page. */
typedef struct {
    PacketHeader header;
    uint32_t npc_id;
    uint32_t dialogue_id;
    uint8_t  current_page;
    uint8_t  option_id;   /**< One of the identifiers the page was sent with. */
    uint8_t  padding[2];
} DialogueOptionSelectPacket;

/** Advance an NPC dialogue to another page. */
typedef struct {
    PacketHeader header;
    uint32_t npc_id;
    uint32_t dialogue_id;
    uint8_t  page_num;
    uint8_t  option_count;
    uint8_t  option_ids[MAX_DIALOGUE_OPTIONS];
    uint8_t  padding[2];
    char     text[MAX_DIALOGUE_TEXT];
    char     option_text[MAX_DIALOGUE_OPTIONS][MAX_OPTION_TEXT];
} DialogueUpdatePacket;

/** Close an NPC dialogue in either direction. */
typedef struct {
    PacketHeader header;
    uint32_t npc_id;
} DialogueClosePacket;

/** Identify item equipment categories carried by item definitions. */
typedef enum {
    SLOT_NONE      = 0,
    SLOT_HELMET    = 1,
    SLOT_GLOVES    = 2,
    SLOT_CHEST     = 3,
    SLOT_LEGGINGS  = 4,
    SLOT_BOOTS     = 5,
    SLOT_MAIN_HAND = 6,
    SLOT_OFF_HAND  = 7,
    SLOT_TWO_HANDED = 8,
} EquipSlotType;

/** Request equipping an inventory item into a designated slot. */
typedef struct {
    PacketHeader header;
    uint32_t item_id;
    uint8_t inventory_slot;
    uint8_t equip_slot;
    uint8_t padding[2];
} EquipItemPacket;

/** Return the item identifiers affected by an equip request. */
typedef struct {
    PacketHeader header;
    uint8_t success;
    uint8_t padding[3];
    uint32_t equipped_item;
    uint32_t returned_item;
    char message[128];
} EquipItemResponsePacket;

/** Request moving equipped gear back to inventory. */
typedef struct {
    PacketHeader header;
    uint8_t equip_slot;
    uint8_t padding[3];
} UnequipItemPacket;

/** Return the inventory destination and item affected by unequipping. */
typedef struct {
    PacketHeader header;
    uint8_t success;
    uint8_t inventory_slot;
    uint8_t padding[2];
    uint32_t unequipped_item;
    char message[128];
} UnequipItemResponsePacket;

/** Request use of an item in an inventory slot. */
typedef struct {
    PacketHeader header;
    uint8_t inventory_slot;
    uint8_t padding[3];
} UseItemPacket;

/** Return the resource changes caused by using an item. */
typedef struct {
    PacketHeader header;
    uint8_t success;
    uint8_t effect_type;
    /** The slot the request named, echoed back.
     *
     * Without it the client had only item_id to go on, and removed the unit
     * from the first slot holding that item rather than from the stack the
     * player actually used. The authoritative INVENTORY_UPDATE that follows
     * repairs the correct slot and says nothing about the wrong one, so the
     * mistaken stack stayed short until the player relogged. Took one of the
     * two padding bytes, so the packet is the same size it always was. */
    uint8_t inventory_slot;
    uint8_t padding;
    int32_t health_changed;
    int32_t mana_changed;
    int32_t new_health;
    int32_t new_mana;
    uint32_t item_id;
    char message[128];
} UseItemResponsePacket;

/** Request removal of an item from an inventory slot. */
typedef struct {
    PacketHeader header;
    uint8_t inventory_slot;
    uint8_t padding[3];
} DropItemPacket;

/** Return the result and item identifier of a drop request. */
typedef struct {
    PacketHeader header;
    uint8_t success;
    uint8_t padding[3];
    uint32_t dropped_item;
} DropItemResponsePacket;

/** Request movement or swapping between two inventory slots. */
typedef struct {
    PacketHeader header;
    uint8_t from_slot;
    uint8_t to_slot;
    uint8_t padding[2];
} MoveItemPacket;

/** Return the slots affected by an inventory move request. */
typedef struct {
    PacketHeader header;
    uint8_t success;
    uint8_t from_slot;
    uint8_t to_slot;
    uint8_t padding;
} MoveItemResponsePacket;

/** Bound the projectiles carried by one batch update. */
#define MAX_PROJECTILES_PER_PACKET 32

/** Announce a projectile's owner, origin, direction, and speed. */
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

/** Describe one projectile position in a batch update. */
typedef struct {
    uint32_t projectile_id;
    float    pos_x, pos_y;
} ProjectilePositionData;

/** Broadcast a bounded batch of visible projectile positions. */
typedef struct {
    PacketHeader header;
    uint8_t count;
    uint8_t padding[3];
    ProjectilePositionData projectiles[MAX_PROJECTILES_PER_PACKET];
} ProjectileUpdatePacket;

/** Announce projectile destruction and its reason code. */
typedef struct {
    PacketHeader header;
    uint32_t projectile_id;
    uint8_t  reason;            // 0=expired, 1=hit_target, 2=cancelled
} ProjectileDestroyPacket;

/** Announce a player's death and the responsible entity. */
typedef struct {
    PacketHeader header;
    uint32_t dead_player_id;
    uint32_t killer_id;
    uint8_t  killer_type;       // 0=npc, 1=player, 2=environment
} PlayerDeathPacket;

/** Announce a player's respawn position and restored resources. */
typedef struct {
    PacketHeader header;
    uint32_t player_id;
    float    pos_x, pos_y;
    int32_t  health;
    int32_t  max_health;
    int32_t  mana;
    int32_t  max_mana;
} PlayerRespawnPacket;

/** Announce a ground-item drop and its world position. */
typedef struct {
    PacketHeader header;
    uint32_t ground_item_id;
    uint32_t item_id;
    uint8_t  quantity;
    float    pos_x, pos_y;
} LootDropPacket;

/** Request collection of a ground item. */
typedef struct {
    PacketHeader header;
    uint32_t ground_item_id;
} LootPickupRequestPacket;

/** Return the inventory destination and result of a loot pickup. */
typedef struct {
    PacketHeader header;
    uint8_t  success;
    uint32_t ground_item_id;
    uint32_t item_id;
    uint8_t  quantity;
    uint8_t  inventory_slot;
    char     message[64];
} LootPickupResponsePacket;

/** Announce removal of a ground item. */
typedef struct {
    PacketHeader header;
    uint32_t ground_item_id;
} LootDespawnPacket;

/** Bound the nearby players carried by one position broadcast. */
#define MAX_NEARBY_PLAYERS 32

/** Describe one nearby player's position, state, and reported latency. */
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

#define MAX_NAME_QUERY 32

/** Ask for the names behind a set of character identifiers.
 *
 * Answered from what is loaded in the world, so an identifier that has since
 * logged out is simply absent from the response rather than an error. The
 * client keeps its last known name in that case, which is what a player
 * expects to see for someone who just left.
 */
typedef struct {
    PacketHeader header;
    uint8_t      count;
    uint8_t      _reserved[3];
    uint32_t     character_ids[MAX_NAME_QUERY];   // network order
} NameQueryRequestPacket;

/** One resolved identifier and its name. */
typedef struct {
    uint32_t character_id;   // network order
    char     name[32];
} NameQueryEntry;

/** Return the names for as many of the requested identifiers as are online.
 *
 * `count` may be smaller than the request's: identifiers that resolve to
 * nobody are left out rather than returned blank, so the client can tell
 * "not online" from "named the empty string".
 */
typedef struct {
    PacketHeader   header;
    uint8_t        count;
    uint8_t        _reserved[3];
    NameQueryEntry entries[MAX_NAME_QUERY];
} NameQueryResponsePacket;

/** Broadcast a bounded batch of nearby player states. */
typedef struct {
    PacketHeader header;
    uint8_t count;
    uint8_t padding[3];
    NearbyPlayerData players[MAX_NEARBY_PLAYERS];
} PlayerPositionBroadcastPacket;

/** Set the number of online players returned per session-list page. */
#define SESSION_LIST_PAGE_SIZE 30

/** Describe one online player in the paginated session list. */
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

/** Request a zero-indexed page of online players. */
typedef struct {
    PacketHeader header;
    uint16_t page;      // 0-indexed
    uint8_t  padding[2];
} SessionListRequestPacket;

/** Return one page and the current pagination totals. */
typedef struct {
    PacketHeader     header;
    uint32_t         total_players;   // total online right now
    uint16_t         total_pages;
    uint16_t         current_page;    // 0-indexed
    uint8_t          count;           // entries in this packet (≤ SESSION_LIST_PAGE_SIZE)
    uint8_t          padding[3];
    SessionPlayerEntry entries[SESSION_LIST_PAGE_SIZE];
} SessionListResponsePacket;

/** Announce an NPC cast and its telegraph geometry. */
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

/** Announce resolution of an NPC cast telegraph. */
typedef struct {
    PacketHeader header;
    uint32_t npc_id;
    uint16_t ability_id;
} NPCTelegraphResolvePacket;

/** Bound chat message storage including its terminator. */
#define MAX_CHAT_MESSAGE 256

/** Identify supported chat delivery channels. */
typedef enum {
    CHAT_CHANNEL_LOCAL  = 0,
    CHAT_CHANNEL_GLOBAL = 1,
    CHAT_CHANNEL_WHISPER = 2,
    CHAT_CHANNEL_PARTY  = 3,
} ChatChannel;

/** Submit a message to a selected chat channel. */
typedef struct {
    PacketHeader header;
    uint8_t  channel;
    uint8_t  padding[3];
    char     message[MAX_CHAT_MESSAGE];
} ChatSendPacket;

/** Broadcast a chat message with its sender identity. */
typedef struct {
    PacketHeader header;
    uint32_t sender_id;
    uint8_t  channel;
    uint8_t  padding[3];
    char     sender_name[32];
    char     message[MAX_CHAT_MESSAGE];
} ChatMessagePacket;

/** Bound party membership including the leader. */
#define MAX_PARTY_SIZE 5

/** Invite a named player to a party. */
typedef struct {
    PacketHeader header;
    char target_name[32];
} PartyInvitePacket;

/** Notify a player of a pending party invitation. */
typedef struct {
    PacketHeader header;
    uint32_t from_id;
    char from_name[32];
} PartyInviteNotifyPacket;

/** Accept the caller's pending party invitation. */
typedef struct {
    PacketHeader header;
} PartyAcceptPacket;

/** Decline the caller's pending party invitation. */
typedef struct {
    PacketHeader header;
} PartyDeclinePacket;

/** Request departure from the caller's current party. */
typedef struct {
    PacketHeader header;
} PartyLeavePacket;

/** Request removal of a member by the party leader. */
typedef struct {
    PacketHeader header;
    uint32_t target_id;
} PartyKickPacket;

/** Synchronize complete party membership and resource state. */
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

/** Announce dissolution of the current party. */
typedef struct {
    PacketHeader header;
} PartyDisbandPacket;

/**
 * Report kill rewards and the player's resulting totals.
 *
 * Kills pay experience only. Enemies drop items, which the player sells to a
 * kingdom's NPCs for that kingdom's coin, so no currency moves on a kill.
 */
typedef struct {
    PacketHeader header;
    uint32_t     xp_gained;
    uint64_t     total_xp;       // Player's new total XP (for bar update)
} KillRewardPacket;

/** Bound the items advertised by one shop-open packet. */
#define MAX_SHOP_ITEMS 32

/** Pair a shop item identifier with its purchase price. */
typedef struct {
    uint32_t item_id;
    uint32_t buy_price;
} ShopItemInfo;

/** Open a named shop with its current item prices. */
typedef struct {
    PacketHeader header;
    uint32_t     shop_id;
    char         shop_name[32];
    uint8_t      item_count;
    uint8_t      padding[3];
    ShopItemInfo items[MAX_SHOP_ITEMS];
} ShopOpenPacket;

/** Request purchase of one item from a shop. */
typedef struct {
    PacketHeader header;
    uint32_t shop_id;
    uint32_t item_id;
} ShopBuyPacket;

/** Return the inventory destination and balance after a purchase. */
typedef struct {
    PacketHeader header;
    uint8_t  success;
    uint8_t  inventory_slot;
    uint8_t  currency_id;      // CurrencyId the shop charged in
    uint8_t  padding;
    uint32_t item_id;
    uint32_t new_balance;      // Player's balance in that currency after the buy
    char     message[64];
} ShopBuyResponsePacket;

/** Request sale of one inventory slot to a shop. */
typedef struct {
    PacketHeader header;
    uint32_t shop_id;
    uint8_t  inventory_slot;
    uint8_t  padding[3];
} ShopSellPacket;

/** Return the sold item, price, and resulting balance. */
typedef struct {
    PacketHeader header;
    uint8_t  success;
    uint8_t  inventory_slot;
    uint8_t  currency_id;      // CurrencyId the shop paid in
    uint8_t  padding;
    uint32_t item_id;
    uint32_t sell_price;
    uint32_t new_balance;      // Player's balance in that currency after the sale
    char     message[64];
} ShopSellResponsePacket;

/** Bound objectives and item rewards carried by quest packets. */
#define MAX_QUEST_OBJECTIVES 4

/** Identify what advances a quest objective.
 *
 * One definition for the wire, the world server's quest registry, and the
 * "type" string in quests.json. Adding a source is one enum entry plus its
 * spelling in the loader -- nothing here is duplicated anywhere else.
 */
typedef enum {
    QUEST_OBJECTIVE_KILL    = 0,   /**< target_id is an npc_type_id. */
    QUEST_OBJECTIVE_COLLECT = 1,   /**< target_id is an item_id. */
    QUEST_OBJECTIVE_TALK    = 2,   /**< target_id is an npc_type_id. */
    QUEST_OBJECTIVE_TYPE_COUNT
} QuestObjectiveType;

/** Describe one quest objective, what advances it, and where it happens.
 *
 * The target and marker travel with the objective so the client can point at it
 * without a second lookup table: the overhead badge needs target_id to know
 * which NPCs to flag, and the map marker needs a position for a target that is
 * nowhere near the player. The server fills the marker from the live world when
 * the quest data does not pin one, so moving an NPC moves its marker.
 */
typedef struct {
    char     description[64];
    int32_t  required;
    uint8_t  objective_type;   /**< QuestObjectiveType. */
    uint8_t  has_marker;       /**< Nonzero when marker_x/marker_y locate the objective. */
    uint8_t  _reserved[2];     /**< Must be 0. */
    uint32_t target_id;        /**< npc_type_id or item_id, per objective_type. */
    float    marker_x;         /**< World position, valid when has_marker. */
    float    marker_y;
} QuestObjectiveInfo;

/** Add a quest and its objectives to the client's log. */
typedef struct {
    PacketHeader       header;
    uint32_t           quest_id;
    char               title[48];
    uint8_t            obj_count;
    uint8_t            padding[3];
    QuestObjectiveInfo objectives[MAX_QUEST_OBJECTIVES];
} QuestAcceptPacket;

/** Synchronize progress for one quest objective. */
typedef struct {
    PacketHeader header;
    uint32_t quest_id;
    uint8_t  obj_index;
    uint8_t  padding[3];
    int32_t  current;
    int32_t  required;
} QuestProgressPacket;

/** Describe one item granted by quest completion. */
typedef struct {
    uint32_t item_id;
    uint8_t  quantity;
    uint8_t  inventory_slot;
    uint8_t  padding[2];
} QuestRewardItem;

_Static_assert(sizeof(QuestObjectiveInfo) == 84,
               "QuestObjectiveInfo must stay 84 bytes on every target");

/** Name one quest whose log entry is being given up.
 *
 * One shape for both directions: PACKET_QUEST_ABANDON is the request, and
 * PACKET_QUEST_ABANDONED is the confirmation sent once the quest is actually
 * gone. Two opcodes rather than an echo, because the direction is what says
 * which one is a claim and which one is a fact -- the client removes the quest
 * from its own log only on the confirmation, so a refused abandon leaves both
 * sides agreeing that the quest is still there.
 */
typedef struct {
    PacketHeader header;
    uint32_t     quest_id;
} QuestAbandonPacket;

/** Announce quest completion and all granted rewards. */
typedef struct {
    PacketHeader    header;
    uint32_t        quest_id;
    uint32_t        xp_reward;
    uint32_t        currency_reward;   // Coin granted, in the currency below
    uint8_t         currency_id;       // CurrencyId the reward is paid in
    uint8_t         item_count;
    uint8_t         padding[2];
    QuestRewardItem items[MAX_QUEST_OBJECTIVES];
} QuestCompletePacket;


/* --- Friends and presence -------------------------------------------------
 *
 * See mmo_server/Next_steps/friends_design.md. The wire rules that matter here:
 *
 *   * A friend is addressed by account_id in every SERVER -> CLIENT packet and
 *     by NAME in every CLIENT -> SERVER one. The client never learns an account
 *     id it can act on by guessing, and a player never has to know one.
 *
 *   * created_at on a pending request is a 64-bit time and therefore travels
 *     through mmo_htonll(), not htonl(). It is the only 64-bit field here.
 *
 *   * A friend's status is current only. There is no last-seen time and no
 *     last world: the game keeps no history of where somebody used to be, so
 *     a friend is on a world right now or they are offline, and that is the
 *     whole of what a list packet says about them.
 *
 *   * An offline friend still carries a name -- the character the account was
 *     last seen playing -- because it is what identifies the row and what the
 *     client's own commands take. It is a label, not a location.
 */

/** Bound the friends carried by one list packet.
 *
 * Equal to MAX_FRIENDS in social_database.h, so a full list is always one
 * packet and the client never has to page. 100 * 44 bytes plus the header sits
 * comfortably inside MAX_PACKET_SIZE; the static assert below is what keeps it
 * that way if the entry ever grows.
 */
#define MAX_FRIENDS_PER_PACKET 100

/** Bound the pending requests carried by one packet.
 *
 * Incoming requests are not capped the way outgoing ones are -- anybody may ask
 * -- so this is a wire cap and the server LIMITs its read to match. A player
 * with more than this many pending requests sees the oldest ones first.
 */
#define MAX_FRIEND_REQUESTS_PER_PACKET 20

/** Report what happened to a friend action.
 *
 * Mirrors FriendResult in social_database.h with one deliberate omission: there
 * is no "blocked" value. A request to somebody who has blocked you is reported
 * as FRIEND_WIRE_OK and nothing is written. Telling a sender they are blocked
 * is how you get a second account.
 */
typedef enum {
    FRIEND_WIRE_OK = 0,
    FRIEND_WIRE_ALREADY_FRIENDS = 1,
    FRIEND_WIRE_ALREADY_PENDING = 2,
    FRIEND_WIRE_MUTUAL          = 3,  // they had already asked; you are now friends
    FRIEND_WIRE_SELF            = 4,
    FRIEND_WIRE_NOT_FOUND       = 5,  // no such player, or no such request
    FRIEND_WIRE_FRIEND_CAP      = 6,
    FRIEND_WIRE_PENDING_CAP     = 7,
    FRIEND_WIRE_ERROR           = 8,
    FRIEND_WIRE_UNAVAILABLE     = 9   // the realm did not answer in time
} FriendWireResult;

/** Name the action a FRIEND_OP_RESULT is reporting on. */
typedef enum {
    FRIEND_ACTION_REQUEST = 0,
    FRIEND_ACTION_ACCEPT  = 1,
    FRIEND_ACTION_DECLINE = 2,
    FRIEND_ACTION_REMOVE  = 3,
    FRIEND_ACTION_BLOCK   = 4,
    FRIEND_ACTION_UNBLOCK = 5
} FriendActionId;

/** Ask to befriend a named player. */
typedef struct {
    PacketHeader header;
    char         target_name[32];
} FriendRequestPacket;

/** Answer a pending request.
 *
 * The sender is named rather than the request, because a player has one pending
 * request per person and a request id would be a number the client has to hold
 * and could get wrong.
 */
typedef struct {
    PacketHeader header;
    char         from_name[32];
    uint8_t      accept;          // nonzero accepts, zero declines
    uint8_t      padding[3];
} FriendRespondPacket;

/** Remove a friend, or withdraw a request already sent to them.
 *
 * One opcode for both because they are the same intent -- "undo whatever I have
 * with this person" -- and the server already knows which of the two exists.
 */
typedef struct {
    PacketHeader header;
    char         target_name[32];
} FriendRemovePacket;

/** Block or unblock an account. */
typedef struct {
    PacketHeader header;
    char         target_name[32];
    uint8_t      block;           // nonzero blocks, zero unblocks
    uint8_t      padding[3];
} FriendBlockPacket;

/** Ask for the friends list and the pending requests.
 *
 * Answered with a FRIEND_LIST_RESPONSE and a FRIEND_REQUESTS_LIST, in that
 * order. Two packets rather than one because they have unrelated size bounds
 * and the panel renders them in separate places.
 */
typedef struct {
    PacketHeader header;
} FriendListRequestPacket;

/** Describe one friend and whether they are playing right now.
 *
 * Current status only. There is no "last seen" and no "last world": the game
 * keeps no history of where somebody used to be, so a panel claiming to show
 * one would be showing something invented. A friend is online on a world, or
 * they are offline.
 *
 * `name` is the character the account was last seen playing, and is what
 * identifies the row -- including for an offline friend, who has no live
 * character to be named after. It is also the name the panel's commands take,
 * so what is displayed is what can be typed back.
 */
typedef struct {
    uint32_t account_id;
    uint32_t world_id;       // 0 when offline
    char     name[32];
    uint8_t  online;
    uint8_t  padding[3];
} FriendWireEntry;           // 44 bytes

/** Return the whole friends list. */
typedef struct {
    PacketHeader    header;
    uint8_t         count;
    uint8_t         padding[3];
    FriendWireEntry friends[MAX_FRIENDS_PER_PACKET];
} FriendListResponsePacket;

/** Describe one request waiting to be answered. */
typedef struct {
    uint32_t from_account;
    char     from_name[32];  // their last known character; "" when never seen
    int64_t  created_at;     // unix seconds, mmo_htonll
} FriendRequestWireEntry;    // 44 bytes

/** Return every request waiting for this player. */
typedef struct {
    PacketHeader           header;
    uint8_t                count;
    uint8_t                padding[3];
    FriendRequestWireEntry requests[MAX_FRIEND_REQUESTS_PER_PACKET];
} FriendRequestsListPacket;

/** Announce one request that has just arrived.
 *
 * A convenience over re-sending the whole list, and never the record: the panel
 * reads pending requests through the server when it opens rather than trusting
 * that this arrived. A dropped notify costs a popup, not a request.
 */
typedef struct {
    PacketHeader header;
    uint32_t     from_account;
    char         from_name[32];
} FriendRequestNotifyPacket;

/** Announce that a friend came online, changed character, or left. */
typedef struct {
    PacketHeader header;
    uint32_t     account_id;
    uint32_t     world_id;      // 0 when offline
    char         name[32];      // the character they are now on; "" when offline
    uint8_t      online;
    uint8_t      padding[3];
} FriendPresenceUpdatePacket;

/** Report the outcome of a friend action.
 *
 * `subject_name` echoes the name the player typed, so a client with two actions
 * in flight can attribute the answer without holding a request id.
 */
typedef struct {
    PacketHeader header;
    uint8_t      action;        // FriendActionId
    uint8_t      result;        // FriendWireResult
    uint8_t      padding[2];
    char         subject_name[32];
} FriendOpResultPacket;

_Static_assert(sizeof(FriendWireEntry) == 44,
               "FriendWireEntry must stay 44 bytes on every target");
_Static_assert(sizeof(FriendRequestWireEntry) == 44,
               "FriendRequestWireEntry must stay 44 bytes on every target");
_Static_assert(sizeof(FriendListResponsePacket) <= 8192,
               "FriendListResponsePacket must fit inside MAX_PACKET_SIZE");
_Static_assert(sizeof(FriendRequestsListPacket) <= 8192,
               "FriendRequestsListPacket must fit inside MAX_PACKET_SIZE");

/** Convert 64-bit integers to and from network order without platform-name collisions.
 *
 * Each macro evaluates its argument more than once; pass a value without side effects.
 */
#define mmo_htonll(x) ((1==htonl(1)) ? (x) : ((uint64_t)htonl((x) & 0xFFFFFFFF) << 32) | htonl((x) >> 32))
#define mmo_ntohll(x) ((1==ntohl(1)) ? (x) : ((uint64_t)ntohl((x) & 0xFFFFFFFF) << 32) | ntohl((x) >> 32))

#pragma pack(pop)

#endif // PROTOCOL_H
