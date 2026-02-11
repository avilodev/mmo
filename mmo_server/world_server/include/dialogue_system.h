// ============================================================================
// dialogue_system.h — NPC dialogue tree system
//
// Manages dialogue definitions loaded from JSON and tracks active dialogue
// sessions between players and NPCs.
// ============================================================================

#ifndef DIALOGUE_SYSTEM_H
#define DIALOGUE_SYSTEM_H

#include <stdint.h>
#include <pthread.h>

#define MAX_DIALOGUES 1000
#define MAX_PAGES_PER_DIALOGUE 32
#define MAX_DIALOGUE_TEXT 512
#define MAX_DIALOGUE_OPTIONS 6
#define MAX_OPTION_TEXT 128
#define MAX_PLAYERS 1000

// ---------------------------------------------------------------------------
// Dialogue definition structures (loaded from JSON)
// ---------------------------------------------------------------------------

// Single dialogue option (player choice)
typedef struct {
    uint8_t  option_id;
    char     text[MAX_OPTION_TEXT];
    int8_t   next_page;          // -1 = close dialogue, >= 0 = page number
    uint8_t  enabled;            // For future conditional logic
} DialogueOptionDef;

// Single page of dialogue
typedef struct {
    uint8_t  page_num;
    char     text[MAX_DIALOGUE_TEXT];
    uint8_t  option_count;
    DialogueOptionDef options[MAX_DIALOGUE_OPTIONS];
} DialoguePageDef;

// Complete dialogue tree
typedef struct {
    uint32_t dialogue_id;
    char     name[64];
    uint8_t  page_count;
    DialoguePageDef pages[MAX_PAGES_PER_DIALOGUE];
} DialogueDef;

// ---------------------------------------------------------------------------
// Active dialogue sessions (runtime state)
// ---------------------------------------------------------------------------

// Tracks an active conversation between a player and NPC
typedef struct {
    uint32_t player_id;
    uint32_t npc_id;
    uint32_t dialogue_id;
    uint8_t  current_page;
    uint8_t  is_active;
    double   last_interaction_time;
} DialogueSession;

// ---------------------------------------------------------------------------
// Dialogue system API
// ---------------------------------------------------------------------------

// Initialize dialogue system from JSON file
// Returns 1 on success, 0 on failure
int dialogue_system_init(const char* json_path);

// Cleanup dialogue system (free all memory)
void dialogue_system_cleanup(void);

// Get dialogue definition by ID
// Returns NULL if not found
const DialogueDef* dialogue_get(uint32_t dialogue_id);

// Get total number of loaded dialogues
int dialogues_get_count(void);

// ---------------------------------------------------------------------------
// Session management
// ---------------------------------------------------------------------------

// Create a new dialogue session for a player
// Returns pointer to session on success, NULL on failure
DialogueSession* dialogue_session_create(uint32_t player_id, uint32_t npc_id, uint32_t dialogue_id);

// Get active dialogue session for a player
// Returns NULL if no active session
DialogueSession* dialogue_session_get(uint32_t player_id);

// Close dialogue session for a player
void dialogue_session_close(uint32_t player_id);

// Update the current page of a dialogue session
void dialogue_session_update_page(uint32_t player_id, uint8_t new_page);

// Check for timed-out sessions and auto-close them
// Call periodically (e.g., every second)
void dialogue_check_timeouts(void);

#endif // DIALOGUE_SYSTEM_H
