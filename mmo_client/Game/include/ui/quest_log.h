#ifndef QUEST_LOG_H
#define QUEST_LOG_H

#include <stdint.h>

// ============================================================================
// QUEST LOG
// Lightweight client-side quest tracker.
// Quests are added via PACKET_QUEST_ACCEPT and progress updated via
// PACKET_QUEST_PROGRESS.  Rendered as a panel accessible with J.
// ============================================================================

#define MAX_QUESTS    32
#define MAX_QUEST_OBJ  4   // Must match MAX_QUEST_OBJECTIVES in protocol.h

typedef struct {
    char    description[64];
    int32_t current;
    int32_t required;
} QuestObjEntry;

typedef struct {
    uint32_t     id;
    char         title[48];
    uint8_t      obj_count;
    QuestObjEntry objectives[MAX_QUEST_OBJ];
    int          active;      // 1 = accepted / in progress
    int          completed;   // 1 = finished
} QuestEntry;

typedef struct {
    QuestEntry entries[MAX_QUESTS];
    int        count;
    int        is_open;     // 1 = panel visible
    int        selected;    // expanded entry index, -1 = none
} QuestLogState;

// Initialize (zero-fills, called by memset in game_init but also safe to call)
void quest_log_init(QuestLogState* ql);

// Add or refresh a quest from PACKET_QUEST_ACCEPT.
// descriptions[i] is the objective description, required[i] is the target count.
// current progress starts at 0 on add.
void quest_log_add(QuestLogState* ql, uint32_t id,
                   const char* title,
                   uint8_t obj_count,
                   const char descriptions[][64],
                   const int32_t required[]);

// Update in-progress objective count (from PACKET_QUEST_PROGRESS).
void quest_log_update_progress(QuestLogState* ql, uint32_t quest_id,
                               uint8_t obj_index, int32_t current, int32_t required);

// Mark quest as completed.
void quest_log_complete(QuestLogState* ql, uint32_t id);

// Remove a quest entry entirely.
void quest_log_remove(QuestLogState* ql, uint32_t id);

// Render the quest log panel.  Call in screen-space after camera pop.
// vw, vh = viewport dimensions.
void quest_log_render(const QuestLogState* ql, int vw, int vh);

// Handle input.  Needs viewport size to compute row hit areas.
// key_j = just pressed (toggle), key_esc = close if open, click outside = close.
void quest_log_handle_input_full(QuestLogState* ql,
                                  float mx, float my, int clicked,
                                  int key_j, int key_esc,
                                  int vw, int vh);

#endif // QUEST_LOG_H
