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
#include "ui/window_pos.h"

#include <stddef.h>
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
	uint8_t       step;             // Step the objectives belong to, from 1; 0 until told.
	uint8_t       step_count;       // Steps in the quest; 1 or 0 means it has no named steps.
	char          step_title[48];
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
	uint8_t     step;
	uint8_t     step_count;
	const char* step_title;
    const char* description;
    int32_t  current;
    int32_t  required;
    uint8_t  objective_type;
    uint32_t target_id;
    uint8_t  has_marker;
    float    marker_x;
    float    marker_y;
} QuestTrackedStep;

// Bound how many available quests the log lists; a display limit, not a rule.
#define MAX_AVAILABLE_QUESTS 32

// One item the server says a quest pays.
typedef struct {
	uint32_t item_id;
	uint8_t  quantity;
} available_reward_item;

// A quest the server says the character may take, and who gives it.
typedef struct {
	uint32_t              id;
	char                  title[48];
	uint8_t               giver_npc_type;
	uint8_t               has_giver_pos;   // Nonzero when giver_x/giver_y mean something.
	float                 giver_x;
	float                 giver_y;
	uint8_t               step_count;      // Steps in the quest; the objectives are step 1's.
	uint8_t               obj_count;
	QuestObjEntry         objectives[MAX_QUEST_OBJ];
	uint32_t              xp_reward;
	uint32_t              currency_reward;
	uint8_t               currency_id;
	uint8_t               item_count;
	available_reward_item items[MAX_QUEST_OBJ];
} available_quest_entry;

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

	// Quests the server says may be taken, replaced whole each time it speaks.
	available_quest_entry available[MAX_AVAILABLE_QUESTS];
	int                   available_count;
	int                   available_selected;   // Expanded entry index, or -1.

	// Where the list and the details window sit; the details follow the list until moved.
	window_pos            list_win;
	window_pos            detail_win;
} QuestLogState;

void quest_log_init(QuestLogState* ql);

/** Add or refresh a quest from an accept packet.
 *
 * Existing objective progress is preserved when an entry is refreshed, which is
 * what makes a reconnect resend harmless.
 */
void quest_log_add(QuestLogState* ql, uint32_t id, const char* title,
                   uint8_t obj_count, const QuestObjectiveInfo* objectives);

// Record which step a quest is on, after quest_log_add; a new step starts from no progress.
void quest_log_set_step(QuestLogState* ql, uint32_t id, uint8_t step, uint8_t step_count,
                        const char* step_title);

// Write "Step 2 of 7: Honest Goods"; returns 0 and writes nothing for a one-step quest.
int quest_step_label(const QuestEntry* e, char* out, size_t size);

// Write "2/7"; returns 0 and writes nothing for a one-step quest.
int quest_step_tag(const QuestEntry* e, char* out, size_t size);

void quest_log_update_progress(QuestLogState* ql, uint32_t quest_id,
                               uint8_t obj_index, int32_t current, int32_t required);

void quest_log_complete(QuestLogState* ql, uint32_t id);

/** Make room for one more entry by dropping the oldest finished quest.
 *
 * @return 1 when an entry can be added, or 0 when every entry is in progress.
 */
int quest_log_make_room(QuestLogState* ql);

/** List a quest the server says was finished in an earlier session.
 *
 * An entry the log already holds is left alone, so a repeatable that is under
 * way again stays in progress. The text must already be terminated.
 */
void quest_log_finished_add(QuestLogState* ql, uint32_t id, const char* title);

void quest_log_remove(QuestLogState* ql, uint32_t id);

/** Report the outstanding objective of the tracked quest.
 *
 * The tracked quest is whichever entry is expanded in the log. When nothing is
 * expanded, the first active quest is tracked, so a player who never opens the
 * log is still pointed somewhere.
 */
QuestTrackedStep quest_log_tracked_step(const QuestLogState* ql);

/** Take the quest the player asked to give up, clearing the request.
 *
 * @return The quest identifier, or zero when nothing was asked for.
 */
uint32_t quest_log_take_abandon_request(QuestLogState* ql);

// Forget every available quest; the server's list follows.
void quest_log_available_reset(QuestLogState* ql);

// Add one quest from the wire, past the display limit it is dropped.
void quest_log_available_add(QuestLogState* ql, const AvailableQuestPacket* pkt);

// Collect the available quests one NPC type gives; returns how many were written.
int quest_log_available_from(const QuestLogState* ql, uint8_t giver_npc_type,
                             const available_quest_entry** out, int max_out);

// Write "Reward: 40 XP, 25 Ennara coin, Item x1" for an available quest.
void quest_log_available_reward_text(const available_quest_entry* e, char* out, size_t size);

// Draw the quest list and the details of the picked quest
void quest_log_render(const QuestLogState* ql, int vw, int vh, float player_x, float player_y);

// Take a frame of input; nonzero down is the left button held, for dragging.
void quest_log_handle_input_full(QuestLogState* ql,
                                 float mx, float my, int clicked, int down,
                                 int key_j, int key_esc,
                                 int vw, int vh);

#endif // QUEST_LOG_H
