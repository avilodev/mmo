#ifndef SERVER_TYPES_H
#define SERVER_TYPES_H

#include "protocol.h"

#include <pthread.h>
#include <time.h>

// Movement allowance, in world pixels, accrued since the last move packet.
// Credit accrues with elapsed real time and is capped, so a client that queues
// up packets and releases them at once cannot bank more travel than the cap —
// the burst is spent against one shared allowance rather than granting each
// packet its own. See move_validator.h.
typedef struct {
    float           credit;
    struct timespec last_tv;
} MoveBudget;

// Server-only in-memory player state. This structure is never sent on the wire.
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

    uint32_t helmet;
    uint32_t gloves;
    uint32_t chest_armor;
    uint32_t leggings;
    uint32_t boots;
    uint32_t main_hand;
    uint32_t second_hand;
    uint16_t blessing;
    uint32_t inventory[150];

    double last_attack_time;
    float attack_cooldown;

    int is_loaded;
    // Slot is claimed by a login that is still loading from the database.
    // Gameplay scans test is_loaded and so correctly ignore a reserved slot,
    // but slot allocation must test is_reserved too or two concurrent logins
    // will claim the same slot while the first one is still loading.
    int is_reserved;
    int is_ready;
    int is_dirty;
    time_t last_save;
    time_t last_activity;
    MoveBudget move_budget;

    uint8_t current_zone_id;

    float ability_cooldowns[5];
    uint16_t ability_slots[5];
    uint8_t ability_count;

    int strength;
    int agility;
    int intelligence;
    int wisdom;
    int defense;
    int evasion;
    int vitality;
    int luck;
    int reg;
    float move_speed;
    int weapon_damage;
    double last_combat_time;

    uint8_t is_dead;
    double death_time;

#define MAX_ACTIVE_EFFECTS 8
    struct {
        uint8_t active;
        uint8_t effect_type;
        uint8_t buff_stat;
        uint8_t _pad;
        int value;
        float duration_remaining;
        float tick_remaining;
        float tick_rate;
        uint32_t source_id;
    } active_effects[MAX_ACTIVE_EFFECTS];

    double last_consumable_time;
    uint32_t party_id;
    uint16_t ping_ms;

#define MAX_PLAYER_QUESTS 32
    struct PlayerQuestSlot {
        uint32_t quest_id;
        uint8_t is_active;
        uint8_t is_complete;
        uint8_t _pad[2];
        int32_t progress[4];
    } quests[MAX_PLAYER_QUESTS];
    int quest_count;

    pthread_mutex_t lock;
} ActivePlayer;

// Realm-server runtime state for a configured world process.
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

#endif
