#ifndef QUEST_LOG_H
#define QUEST_LOG_H

#include <stdint.h>

/**
 * @file
 * Declare client quest state updated by quest accept and progress packets.
 */

#define MAX_QUESTS    32
/** Match the protocol's maximum objectives per quest. */
#define MAX_QUEST_OBJ  4

/** Track one quest objective's description and progress. */
typedef struct {
    char    description[64];
    int32_t current;
    int32_t required;
} QuestObjEntry;

/** Track one accepted or completed client quest. */
typedef struct {
    uint32_t     id;
    char         title[48];
    uint8_t      obj_count;
    QuestObjEntry objectives[MAX_QUEST_OBJ];
    int          active;
    int          completed;
} QuestEntry;

/** Track quest-log entries, visibility, and expanded selection. */
typedef struct {
    QuestEntry entries[MAX_QUESTS];
    int        count;
    int        is_open;
    int        selected;    /**< Expanded entry index, or -1. */
} QuestLogState;

void quest_log_init(QuestLogState* ql);

/** Add or refresh parallel objective descriptions and required counts. */
void quest_log_add(QuestLogState* ql, uint32_t id,
                   const char* title,
                   uint8_t obj_count,
                   const char descriptions[][64],
                   const int32_t required[]);

void quest_log_update_progress(QuestLogState* ql, uint32_t quest_id,
                               uint8_t obj_index, int32_t current, int32_t required);

void quest_log_complete(QuestLogState* ql, uint32_t id);

void quest_log_remove(QuestLogState* ql, uint32_t id);

void quest_log_render(const QuestLogState* ql, int vw, int vh);

void quest_log_handle_input_full(QuestLogState* ql,
                                  float mx, float my, int clicked,
                                  int key_j, int key_esc,
                                  int vw, int vh);

#endif // QUEST_LOG_H
