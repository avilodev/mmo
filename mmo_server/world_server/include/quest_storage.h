#ifndef QUEST_STORAGE_H
#define QUEST_STORAGE_H

/** @file Hold what one character has done with quests, and put it on disk.
 *
 * Three questions, deliberately answered by different tables rather than one
 * wider one, because different populations pay for each. What is in progress is
 * the log, and it is emptied as quests are handed in. Whether something was
 * ever finished is the history, and it grows for a character's whole life. How
 * often and how recently a *repeatable* was finished is the counter table, and
 * it holds an entry only for the handful of quests that can repeat.
 *
 * Nothing here has a compiled ceiling; every table is heap-allocated and sized
 * from what the character has actually done. They travel together as a
 * PlayerQuestState (server_types.h) so that adding a table later is a change in
 * this file and nowhere else.
 */

#include "server_types.h"   /* PlayerQuestState and the tables it holds */
#include "quest_repeat.h"   /* QuestRepeatMode */

#include <stdint.h>

/* --- Lifetime ------------------------------------------------------------ */

/** Release every table a character's quest state owns.
 *
 * Called when a player slot is recycled, and after a snapshot has been written.
 * Safe on already-empty storage.
 */
void quest_state_release(PlayerQuestState* state);

/** Deep-copy a character's quest state, for saving off the player's lock.
 *
 * The save runs on another thread after the slot lock is released, by which
 * time the live tables can have been grown or freed, so the save path is given
 * its own copy rather than a pointer into the player.
 *
 * @return 1 when the copy is complete, or 0 on allocation failure, in which
 *         case @p out is released and left empty.
 */
int quest_state_copy(PlayerQuestState* out, const PlayerQuestState* src);

/* --- The active log ------------------------------------------------------ */

/** Find a character's record for one quest.
 *
 * @return The slot, or NULL when the quest is not in progress.
 */
struct PlayerQuestSlot* quest_log_find(PlayerQuestLog* log, uint32_t quest_id);

/** Make room for one more active quest.
 *
 * There is no ceiling. The only way this fails is out of memory.
 *
 * @return 1 when the log can take another entry, or 0 on allocation failure.
 */
int quest_log_reserve_one(PlayerQuestLog* log);

/** Drop one quest from the active log, freeing its slot and its progress.
 *
 * Used by both a hand-in and an abandon, which differ only in what else they
 * do: a hand-in also records completion, and an abandon deliberately does not.
 *
 * @return 1 when the quest was in the log and is now gone, or 0 when absent.
 */
int quest_log_remove(PlayerQuestLog* log, uint32_t quest_id);

/* --- The completion history ---------------------------------------------- */

/** Record a finished quest in a history set, growing it as needed.
 *
 * @return 1 when the set holds the identifier afterwards, or 0 on allocation failure.
 */
int quest_history_add(QuestHistory* history, uint32_t quest_id);

/** Report whether a character has finished a quest. O(1). */
int quest_history_contains(const QuestHistory* history, uint32_t quest_id);

/** Report how many quests a character has finished. */
int quest_history_count(const QuestHistory* history);

/* --- Repeat counters ----------------------------------------------------- */

/** Find a character's counter for one repeatable quest.
 *
 * @return The entry, or NULL when they have never turned this quest in.
 */
const QuestCounter* quest_counters_find(const QuestCounters* counters, uint32_t quest_id);

/** Record one turn-in of a repeatable quest, creating its entry if needed.
 *
 * The count saturates at its maximum rather than wrapping, because a wrapped
 * count would silently reopen a capped quest.
 *
 * @param window  Reset window this turn-in falls in; see quest_repeat.h.
 * @return        1 when the table holds the turn-in afterwards, or 0 on
 *                allocation failure.
 */
int quest_counters_record(QuestCounters* counters, uint32_t quest_id, uint16_t window);

/** Report how many distinct repeatable quests a character has turned in. */
int quest_counters_count(const QuestCounters* counters);

/** Release a counter table on its own. Safe on an empty one. */
void quest_counters_release(QuestCounters* counters);

/* --- Recording a turn-in ------------------------------------------------- */

/** Record one completed turn-in in every table it belongs in.
 *
 * The single place the two-table rule lives, and the reason it is one call
 * rather than two at the call site. Completion always enters the history set,
 * because that is what every prerequisite check reads; a repeatable *also*
 * increments its counter, because that is what the reset gate reads. Recording
 * only the counter would leave anything chained behind a repeatable
 * permanently unopenable, and recording only the history would lose when it
 * last happened.
 *
 * A once-only quest writes no counter, so a character who does nothing but
 * story quests never allocates a counter table.
 *
 * @param now  The instant of the turn-in; only a repeatable reads it.
 * @return     1 when every table holds it afterwards, or 0 on allocation failure.
 */
int quest_state_record_turnin(PlayerQuestState* state, uint32_t quest_id,
                              QuestRepeatMode mode, time_t now);

/* --- On disk ------------------------------------------------------------- */

/** Set and create the directory holding per-character quest-state files. */
void quest_storage_set_dir(const char* dir);

/** Persist a character's quest state.
 *
 * @return 1 when the file is written and renamed, or 0 on failure.
 */
int quest_player_save(uint32_t character_id, const PlayerQuestState* state);

/** Restore a character's quest state.
 *
 * Reads the current format and the ones before it. The oldest held a single
 * fixed array in which a turned-in quest stayed behind as an inactive record;
 * those records become history entries, which is what they always meant.
 *
 * @return 1 when something was loaded, or 0 when the file is absent or unusable.
 */
int quest_player_load(uint32_t character_id, PlayerQuestState* state);

#endif // QUEST_STORAGE_H
