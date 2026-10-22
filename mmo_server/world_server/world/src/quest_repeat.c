/**
 * @file
 * Reset-window arithmetic for repeatable quests.
 */

#include "quest_repeat.h"

#include <string.h>

/** Map a quests.json "repeat" spelling to its mode. */
static const struct {
    const char*     key;
    QuestRepeatMode mode;
} REPEAT_SPELLINGS[] = {
    { "once",   QUEST_REPEAT_ONCE   },
    { "free",   QUEST_REPEAT_FREE   },
    { "daily",  QUEST_REPEAT_DAILY  },
    { "weekly", QUEST_REPEAT_WEEKLY },
};

#define REPEAT_SPELLING_COUNT \
    ((int)(sizeof(REPEAT_SPELLINGS) / sizeof(REPEAT_SPELLINGS[0])))

/** Shift every reset off midnight UTC.
 *
 * Pinned at zero, and pinned deliberately: moving it later either skips a day
 * or grants a free one, so it is chosen once and lived with. Only the
 * comparison below reads it -- the stored window index is the same eight bytes
 * whatever it is -- so this is one of the few numbers here that can still be
 * changed after launch for free.
 */
#define QUEST_RESET_OFFSET_SECONDS 0

#define SECONDS_PER_DAY  86400
#define SECONDS_PER_WEEK (7 * SECONDS_PER_DAY)

QuestRepeatMode quest_repeat_mode_from_key(const char* key) {
    if (!key) return QUEST_REPEAT_MODE_COUNT;

    for (int i = 0; i < REPEAT_SPELLING_COUNT; i++)
        if (strcmp(REPEAT_SPELLINGS[i].key, key) == 0)
            return REPEAT_SPELLINGS[i].mode;

    return QUEST_REPEAT_MODE_COUNT;
}

/**
 * Report how long one reset period lasts.
 *
 * @return The period in seconds, or 0 for a mode that never resets.
 */
static long period_seconds(QuestRepeatMode mode) {
    switch (mode) {
        case QUEST_REPEAT_DAILY:  return SECONDS_PER_DAY;
        case QUEST_REPEAT_WEEKLY: return SECONDS_PER_WEEK;
        default:                  return 0;
    }
}

uint16_t quest_repeat_window_of(QuestRepeatMode mode, time_t now) {
    long period = period_seconds(mode);
    if (period == 0) return 0;

    long long elapsed = (long long)now - QUEST_RESET_OFFSET_SECONDS;
    if (elapsed < 0) return 0;

    /* A uint16 of days reaches into the twenty-second century, and of weeks far
     * past that; wrapping would make a stale window read as a fresh one, so the
     * ceiling saturates rather than overflowing. QUEST_WINDOW_EXHAUSTED is what
     * quest_repeat_allows() watches for. */
    long long window = elapsed / period;
    return window > QUEST_WINDOW_EXHAUSTED ? QUEST_WINDOW_EXHAUSTED : (uint16_t)window;
}

int quest_repeat_allows(QuestRepeatMode mode, uint16_t times_done,
                        uint16_t last_window, uint16_t max_count, time_t now) {
    if (mode == QUEST_REPEAT_ONCE || mode >= QUEST_REPEAT_MODE_COUNT) return 0;

    /* A cap outranks a reset: the last of a capped daily is the last of it. */
    if (max_count > 0 && times_done >= max_count) return 0;

    long period = period_seconds(mode);
    if (period == 0) return 1;   /* QUEST_REPEAT_FREE: no wait at all. */

    uint16_t window = quest_repeat_window_of(mode, now);

    /* Past the end of the counter every instant is the same window, so "is this
     * a later window than the last turn-in" is false forever. Answering yes
     * instead is the deliberate choice: a daily that resets imprecisely long
     * after this build is gone is a far smaller failure than every daily in the
     * game quietly ceasing to reset, with nothing in any log to explain it. */
    if (window >= QUEST_WINDOW_EXHAUSTED) return 1;

    return window > last_window;
}
