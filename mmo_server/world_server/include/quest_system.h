#ifndef QUEST_SYSTEM_H
#define QUEST_SYSTEM_H

#include <stdint.h>

/** Bound loaded quests, per-player records, objectives, and item rewards. */
#define MAX_QUESTS          256
#define MAX_PLAYER_QUESTS   32

#define MAX_QUEST_OBJECTIVES 4

/** Identify objective progress sources. */
#define QUEST_OBJ_KILL    0
#define QUEST_OBJ_COLLECT 1
#define QUEST_OBJ_TALK    2

/** Define one objective's event type, target, description, and required count. */
typedef struct {
    uint8_t  type;              // QUEST_OBJ_KILL / QUEST_OBJ_COLLECT
    uint32_t target_id;         // npc_type_id (kill) or item_id (collect)
    char     description[64];   // e.g. "Kill 5 Wolves"
    int32_t  required_count;
} QuestObjectiveDef;

/** Define one item stack granted as a quest reward. */
typedef struct {
    uint32_t item_id;
    uint8_t  quantity;
} QuestItemReward;

/** Aggregate objectives and completion rewards for one quest. */
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

/** Mirror ActivePlayer.PlayerQuestSlot for persistence and packet handling. */
typedef struct {
    uint32_t quest_id;
    uint8_t  is_active;
    uint8_t  is_complete;
    uint8_t  _pad[2];
    int32_t  progress[4];   // MAX_QUEST_OBJECTIVES
} PlayerQuestEntry;

// load quest definitions from JSON
int  quest_system_init(const char* json_path);
void quest_system_cleanup(void);
void quest_system_set_dir(const char* dir);

// return a registry-owned definition or NULL when absent
const QuestDef* quest_get(uint32_t quest_id);

// return nonzero after accepting and notifying the player
int quest_player_accept(uint32_t character_id, int client_fd, uint32_t quest_id);

// return nonzero after successful completed-quest turn-in
int quest_player_turnin(uint32_t character_id, int client_fd, uint32_t quest_id);

void quest_on_npc_kill(uint32_t character_id, int client_fd, uint16_t npc_type_id);

void quest_on_item_collect(uint32_t character_id, int client_fd, uint32_t item_id);

void quest_on_npc_talk(uint32_t character_id, int client_fd, uint16_t npc_type_id);

// persist and restore per-character quest state from files
int quest_player_save(uint32_t character_id, const PlayerQuestEntry* quests, int count);
int  quest_player_load(uint32_t character_id, PlayerQuestEntry* quests, int max_count);

void quest_send_all(uint32_t character_id, int client_fd);

#endif // QUEST_SYSTEM_H
