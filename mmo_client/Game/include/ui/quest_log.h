#ifndef QUEST_LOG_H
#define QUEST_LOG_H

/**
 * @file
 * Hold the client's quest state and decide what the player is currently tracking.
 *
 * The log knows more than a title and a counter: every objective carries what
 * advances it and what it points at, straight from the accept packet. That is
 * what lets the tracked step be drawn in three places at once -- the tracker
 * panel, a badge over the target NPC, and a marker on the map -- without any of
 * them holding a private table of who-is-where.
 */

#include "protocol.h"

#include <stdint.h>

/** Bound how many quests this panel will display at once.
 *
 * A display limit, not a game rule: the server puts no ceiling on how many
 * quests a character may hold. If that ever needs to be more than one screen,
 * paginate here rather than capping anything on the server.
 */
#define MAX_QUESTS    32
/** Match the protocol's maximum objectives per quest. */
#define MAX_QUEST_OBJ MAX_QUEST_OBJECTIVES

/** Track one quest objective, its progress, and where it happens. */
typedef struct {
    char     description[64];
    int32_t  current;
    int32_t  required;
    uint8_t  objective_type;  /**< QuestObjectiveType. */
    uint8_t  has_marker;      /**< Nonzero when marker_x/marker_y are meaningful. */
    uint32_t target_id;       /**< npc_type_id or item_id, per objective_type. */
    float    marker_x;
    float    marker_y;
} QuestObjEntry;

/** Track one accepted or completed client quest. */
typedef struct {
    uint32_t      id;
    char          title[48];
    uint8_t       obj_count;
    QuestObjEntry objectives[MAX_QUEST_OBJ];
    int           active;
    int           completed;
} QuestEntry;

/** Describe the step the player is currently being pointed at.
 *
 * Filled by quest_log_tracked_step(); `valid` is zero when nothing is tracked
 * or the tracked quest has no outstanding objective left.
 */
typedef struct {
    int      valid;
    uint32_t quest_id;
    const char* quest_title;
    const char* description;
    int32_t  current;
    int32_t  required;
    uint8_t  objective_type;
    uint32_t target_id;
    uint8_t  has_marker;
    float    marker_x;
    float    marker_y;
} QuestTrackedStep;

/** Track quest-log entries, visibility, and the tracked quest. */
typedef struct {
    QuestEntry entries[MAX_QUESTS];
    int        count;
    int        is_open;
    int        selected;    /**< Expanded and tracked entry index, or -1. */

    /** Quest the player just asked to give up, or zero.
     *
     * The panel cannot send packets and should not learn how to, so it records
     * the request and the game loop drains it with
     * quest_log_take_abandon_request(). The entry itself is not removed here --
     * that waits for the server's confirmation, so a refused abandon leaves the
     * log showing the quest is still there.
     */
    uint32_t   pending_abandon;
} QuestLogState;

void quest_log_init(QuestLogState* ql);

/** Add or refresh a quest from an accept packet.
 *
 * Existing objective progress is preserved when an entry is refreshed, which is
 * what makes a reconnect resend harmless.
 */
void quest_log_add(QuestLogState* ql, uint32_t id, const char* title,
                   uint8_t obj_count, const QuestObjectiveInfo* objectives);

void quest_log_update_progress(QuestLogState* ql, uint32_t quest_id,
                               uint8_t obj_index, int32_t current, int32_t required);

void quest_log_complete(QuestLogState* ql, uint32_t id);

void quest_log_remove(QuestLogState* ql, uint32_t id);

/** Report the outstanding objective of the tracked quest.
 *
 * The tracked quest is whichever entry is expanded in the log. When nothing is
 * expanded, the first active quest is tracked, so a player who never opens the
 * log is still pointed somewhere.
 */
QuestTrackedStep quest_log_tracked_step(const QuestLogState* ql);

/** Report whether an NPC type is what the tracked step wants the player to reach.
 *
 * Used by the world renderer to badge the right NPC overhead.
 */
int quest_log_npc_is_tracked(const QuestLogState* ql, uint8_t npc_type_id);

/** Take the quest the player asked to give up, clearing the request.
 *
 * @return The quest identifier, or zero when nothing was asked for.
 */
uint32_t quest_log_take_abandon_request(QuestLogState* ql);

void quest_log_render(const QuestLogState* ql, int vw, int vh);

void quest_log_handle_input_full(QuestLogState* ql,
                                 float mx, float my, int clicked,
                                 int key_j, int key_esc,
                                 int vw, int vh);

#endif // QUEST_LOG_H
