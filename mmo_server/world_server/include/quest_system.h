#ifndef QUEST_SYSTEM_H
#define QUEST_SYSTEM_H

/** @file Decide who may take a quest, grant it, advance it, and hand it in.
 *
 * The quest feature is three files, split by what changes them:
 *
 *   quest_registry.h  what a quest *is*, authored in quests.json
 *   quest_storage.h   what one character has done with quests, and its file
 *   quest_system.h    the rules connecting the two, and the packets
 *
 * Including this header pulls in the other two, so a caller that just wants
 * "the quest system" still gets one include.
 *
 * An objective knows what advances it (QuestObjectiveType) and what it points
 * at, and that travels to the client so the quest log can mark the target NPC
 * overhead and on the map. Where the target *is* is answered from the live
 * world at send time rather than copied into the quest file, so moving a spawn
 * moves its marker.
 */

#include "quest_registry.h"
#include "quest_storage.h"
#include "server_types.h"   /* ActivePlayer */
#include "protocol.h"

#include <stdint.h>
#include <time.h>

/** Name the on-disk record for one in-progress quest.
 *
 * Deliberately identical to PlayerQuestSlot: the save path writes slots
 * straight out, and this alias is what says that the file format and the
 * in-memory shape are the same thing on purpose.
 */
typedef struct PlayerQuestSlot PlayerQuestEntry;

/** Report whether a character may take a quest right now.
 *
 * Checks race, level, prerequisites, and that the quest is neither already in
 * progress nor already finished. This is the single definition of "eligible";
 * quest_player_accept() and the dialogue condition both call it.
 *
 * @param p  A locked character.
 * @return   1 when the quest may be granted, or 0 otherwise.
 */
int quest_is_available_for(const QuestDef* q, const ActivePlayer* p);

/** Report whether a character may take a quest at a given instant.
 *
 * What quest_is_available_for() calls with the current time. Split out so a
 * daily's reset can be tested without waiting a day for it, and so the whole
 * eligibility rule stays a pure function of the character and the clock.
 */
int quest_is_available_at(const QuestDef* q, const ActivePlayer* p, time_t now);

/** Grant a quest to a character and send its initial state.
 *
 * @return Nonzero when accepted, or 0 when unknown, duplicated, gated out, or
 *         out of memory. There is no cap on how many quests a character may
 *         hold at once, so a full log is not among the reasons this can fail.
 */
int quest_player_accept(uint32_t character_id, int client_fd, uint32_t quest_id);

/** Hand in a completed quest and grant its rewards.
 *
 * @return Nonzero on a successful turn-in, or 0 when absent, inactive, or unfinished.
 */
int quest_player_turnin(uint32_t character_id, int client_fd, uint32_t quest_id);

/** Give up an active quest, freeing its slot and its progress.
 *
 * The only quest action a client starts. The named quest must actually be in
 * this character's own log -- the packet is never trusted to say whose quest it
 * is, and an abandon that does not find it changes nothing and sends nothing.
 *
 * Nothing is recorded as completed. An abandoned quest was never finished, and
 * an identifier in the completion history is permanent, so writing one here
 * would take the quest away for the rest of the character's life.
 *
 * @return 1 when the quest was held and is now gone, or 0 otherwise.
 */
int quest_player_abandon(uint32_t character_id, int client_fd, uint32_t quest_id);

void quest_on_npc_kill(uint32_t character_id, int client_fd, uint16_t npc_type_id);

void quest_on_item_collect(uint32_t character_id, int client_fd, uint32_t item_id);

void quest_on_npc_talk(uint32_t character_id, int client_fd, uint16_t npc_type_id);

/** Resend every active quest and its progress, as after entering the world. */
void quest_send_all(uint32_t character_id, int client_fd);

#endif // QUEST_SYSTEM_H
