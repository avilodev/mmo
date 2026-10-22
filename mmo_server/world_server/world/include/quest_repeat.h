#ifndef QUEST_REPEAT_H
#define QUEST_REPEAT_H

/** @file Decide when a finished quest may be taken again.
 *
 * Policy, not storage. Everything here is arithmetic over a turn-in count and
 * a reset-window index, with no player, no file and no clock of its own -- the
 * caller passes the time in. That is deliberate: reset policy is the part of
 * repeatable quests most likely to be retuned after launch, and keeping it
 * free of state is what makes retuning free.
 *
 * The window is wall-clock, not a rolling personal timer. Everyone's dailies
 * flip at the same instant, which is what makes group content schedulable; a
 * rolling 24-hour timer drifts later every day until players cannot fit it in.
 */

#include <stdint.h>
#include <time.h>

/** Say whether, and how often, a quest may be taken again after it is finished.
 *
 * The default is ONCE, which is what every story quest wants and what a quest
 * file that says nothing gets. It is also the mode under which completion
 * history alone answers "may they take it again", so a character who only ever
 * does story quests never grows a counter table at all.
 */
typedef enum {
    QUEST_REPEAT_ONCE = 0,   /**< Finished is finished. */
    QUEST_REPEAT_FREE,       /**< Available again immediately; honours max_count. */
    QUEST_REPEAT_DAILY,      /**< Available again after the next daily reset. */
    QUEST_REPEAT_WEEKLY,     /**< Available again after the next weekly reset. */
    QUEST_REPEAT_MODE_COUNT
} QuestRepeatMode;

/** Resolve a quests.json "repeat" string.
 *
 * @return The mode, or QUEST_REPEAT_MODE_COUNT when the spelling is unknown.
 */
QuestRepeatMode quest_repeat_mode_from_key(const char* key);

/** Mark a window index that has run out of range.
 *
 * The counter is sixteen bits, which is centuries of days, but "centuries" is
 * not "never". Reaching this value means the index can no longer distinguish
 * one period from the next, and quest_repeat_allows() treats it as "reset now"
 * rather than "never again" -- see the note there.
 */
#define QUEST_WINDOW_EXHAUSTED UINT16_MAX

/** Report which reset period an instant falls in.
 *
 * A period index rather than a timestamp, because that is all a reset check
 * ever needs and it is what keeps a stored counter down to eight bytes with no
 * padding. Modes with no reset answer zero.
 */
uint16_t quest_repeat_window_of(QuestRepeatMode mode, time_t now);

/** Report whether a character who has already finished a quest may take it again.
 *
 * Asked only after completion history says they finished it; a quest they have
 * never done is available on its other requirements alone.
 *
 * @param times_done   Lifetime turn-ins.
 * @param last_window  Reset window the last turn-in fell in.
 * @param max_count    Lifetime cap, or zero for none.
 * @return             1 when it may be taken again now, or 0 otherwise.
 */
int quest_repeat_allows(QuestRepeatMode mode, uint16_t times_done,
                        uint16_t last_window, uint16_t max_count, time_t now);

#endif // QUEST_REPEAT_H
