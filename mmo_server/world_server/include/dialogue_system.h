/** @file Define JSON dialogue trees and active player/NPC conversations. */

#ifndef DIALOGUE_SYSTEM_H
#define DIALOGUE_SYSTEM_H

#include <stdint.h>
#include <pthread.h>

/** Bound loaded dialogue trees, pages, text, options, and active players. */
#define MAX_DIALOGUES 1000
#define MAX_PAGES_PER_DIALOGUE 32
#define MAX_DIALOGUE_TEXT 512
#define MAX_DIALOGUE_OPTIONS 6
#define MAX_OPTION_TEXT 128
#define MAX_PLAYERS 1000

/** Identify server actions triggered by dialogue choices. */
#define DIALOGUE_ACTION_NONE         0
#define DIALOGUE_ACTION_OPEN_SHOP    1   // action_value = shop_id
#define DIALOGUE_ACTION_QUEST_ACCEPT 2   // action_value = quest_id
#define DIALOGUE_ACTION_QUEST_TURNIN 3   // action_value = quest_id; fail_page used if not complete

/** Define one player choice and its navigation or server action. */
typedef struct {
    uint8_t  option_id;
    char     text[MAX_OPTION_TEXT];
    int8_t   next_page;          /**< Page number, or -1 to close dialogue. */
    int8_t   fail_page;          /**< Failure page, or -2 to use next_page. */
    uint8_t  action;             // DIALOGUE_ACTION_*
    uint8_t  enabled;
    uint32_t action_value;       // shop_id or quest_id depending on action
} DialogueOptionDef;

/** Define one text page and its available choices. */
typedef struct {
    uint8_t  page_num;
    char     text[MAX_DIALOGUE_TEXT];
    uint8_t  option_count;
    DialogueOptionDef options[MAX_DIALOGUE_OPTIONS];
} DialoguePageDef;

/** Define one complete named dialogue tree. */
typedef struct {
    uint32_t dialogue_id;
    char     name[64];
    uint8_t  page_count;
    DialoguePageDef pages[MAX_PAGES_PER_DIALOGUE];
} DialogueDef;

/** Track one active conversation between a player and NPC. */
typedef struct {
    uint32_t player_id;
    uint32_t npc_id;
    uint32_t dialogue_id;
    uint8_t  current_page;
    uint8_t  is_active;
    double   last_interaction_time;
} DialogueSession;

// return nonzero when the JSON registry loads successfully
int dialogue_system_init(const char* json_path);

void dialogue_system_cleanup(void);

// return a registry-owned definition or NULL when absent
const DialogueDef* dialogue_get(uint32_t dialogue_id);

int dialogues_get_count(void);

// return a system-owned session or NULL when creation fails
DialogueSession* dialogue_session_create(uint32_t player_id, uint32_t npc_id, uint32_t dialogue_id);

// return the system-owned active session or NULL when absent
DialogueSession* dialogue_session_get(uint32_t player_id);

void dialogue_session_close(uint32_t player_id);

void dialogue_session_update_page(uint32_t player_id, uint8_t new_page);

// expire inactive sessions from a periodic world update
void dialogue_check_timeouts(void);

#endif // DIALOGUE_SYSTEM_H
