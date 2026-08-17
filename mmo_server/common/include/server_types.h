#ifndef SERVER_TYPES_H
#define SERVER_TYPES_H

/** @file Define server-only player and world runtime state. */

#include "protocol.h"
#include "item_instance.h"

#include <pthread.h>
#include <time.h>

/** Track capped movement credit accrued between packets. */
typedef struct {
    float           credit;  /**< Available movement distance in world pixels. */
    struct timespec last_tv; /**< Timestamp used to accrue additional credit. */
} MoveBudget;

/** Hold one active player's server-only mutable state. */
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

    /** Preserve item identity when moving stacks between bag and equipment. */
    ItemInstance inventory[INVENTORY_SLOTS];
    ItemInstance equipment[EQUIP_SLOTS];

    double last_attack_time;
    float attack_cooldown;

    int is_loaded;
    int is_reserved; /**< Marks a slot claimed by an in-progress database load. */
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

/** Bound concurrent status effects stored for one active player. */
#define MAX_ACTIVE_EFFECTS 8
    /** Track one active status effect and its remaining timers. */
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

/** Bound quest records retained in one active player slot. */
#define MAX_PLAYER_QUESTS 32
    /** Track one quest and its objective progress counters. */
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

/** Track one configured world process from the realm server. */
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
