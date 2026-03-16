#ifndef QUEST_LOG_H
#define QUEST_LOG_H

#include <stdint.h>

// ============================================================================
// QUEST LOG
// Lightweight client-side quest tracker.
// Quests are added by game systems (dialogue, server packets) and rendered
// as a panel accessible with J.
// ============================================================================

#define MAX_QUESTS 32

typedef struct {
    uint32_t id;
    char     title[48];
    char     objective[128];
    int      active;      // 1 = accepted / in progress
    int      completed;   // 1 = finished
} QuestEntry;

typedef struct {
    QuestEntry entries[MAX_QUESTS];
    int        count;
    int        is_open;     // 1 = panel visible
    int        selected;    // expanded entry index, -1 = none
} QuestLogState;

// Initialize (zero-fills, called by memset in game_init but also safe to call)
void quest_log_init(QuestLogState* ql);

// Add or refresh an existing quest entry (matched by id).
// If id already exists its title/objective are updated.
void quest_log_add(QuestLogState* ql, uint32_t id,
                   const char* title, const char* objective);

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
