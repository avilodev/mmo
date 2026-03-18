#ifndef QUEST_SYSTEM_H
#define QUEST_SYSTEM_H

#include <stdint.h>
#include "../../common/protocol.h"  // MAX_QUEST_OBJECTIVES

#define MAX_QUESTS          256
#define MAX_PLAYER_QUESTS   32

// Types of quest objectives
#define QUEST_OBJ_KILL    0
#define QUEST_OBJ_COLLECT 1

typedef struct {
    uint8_t  type;              // QUEST_OBJ_KILL / QUEST_OBJ_COLLECT
    uint32_t target_id;         // npc_type_id (kill) or item_id (collect)
    char     description[64];   // e.g. "Kill 5 Wolves"
    int32_t  required_count;
} QuestObjectiveDef;

typedef struct {
    uint32_t item_id;
    uint8_t  quantity;
} QuestItemReward;

typedef struct {
    uint32_t          quest_id;
    char              title[48];
    uint8_t           obj_count;
    QuestObjectiveDef objectives[MAX_QUEST_OBJECTIVES];
    uint32_t          xp_reward;
    uint32_t          gold_reward;
    uint8_t           item_reward_count;
    QuestItemReward   item_rewards[MAX_QUEST_OBJECTIVES];
} QuestDef;

// Per-player runtime quest state (stored in ActivePlayer).
// Layout MUST match struct PlayerQuestSlot in headers.h.
typedef struct {
    uint32_t quest_id;
    uint8_t  is_active;
    uint8_t  is_complete;
    uint8_t  _pad[2];
    int32_t  progress[4];   // MAX_QUEST_OBJECTIVES
} PlayerQuestEntry;

// Init: load quests.json
int  quest_system_init(const char* json_path);
void quest_system_cleanup(void);

// Look up a quest definition
const QuestDef* quest_get(uint32_t quest_id);

// Player operations (client_fd needed to push packets)
// Returns 1 on success, 0 on failure (already accepted, quest not found, etc.)
int quest_player_accept(uint32_t character_id, int client_fd, uint32_t quest_id);

// Returns 1 = turned in successfully, 0 = not complete yet
int quest_player_turnin(uint32_t character_id, int client_fd, uint32_t quest_id);

// Called on NPC kill — updates kill objectives for the attacker
void quest_on_npc_kill(uint32_t character_id, int client_fd, uint16_t npc_type_id);

// Save/load quest state for a character (file-based)
void quest_player_save(uint32_t character_id, const PlayerQuestEntry* quests, int count);
int  quest_player_load(uint32_t character_id, PlayerQuestEntry* quests, int max_count);

// Send the full quest log to a client (called on login)
void quest_send_all(uint32_t character_id, int client_fd);

#endif // QUEST_SYSTEM_H
