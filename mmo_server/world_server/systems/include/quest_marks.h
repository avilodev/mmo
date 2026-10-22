#ifndef QUEST_MARKS_H
#define QUEST_MARKS_H

/** @file Work out which NPCs have a quest for one character, and tell the client.
 *
 * What an NPC can offer is written once, in its dialogue: an option that
 * accepts a quest, behind the conditions that decide who sees it, and an option
 * that hands one in. This reads those same options rather than keeping a second
 * list of who gives what, so a mark can never promise a quest the conversation
 * will not offer -- they are one rule asked twice.
 *
 * Kept apart from quest_system.c because it needs the dialogue system, and the
 * quest rules are linked on their own by tests that have no use for it.
 */

#include "protocol.h"

#include <stdint.h>

/** An NPC type and the dialogue it speaks. */
typedef struct {
    uint8_t  npc_type_id;
    uint32_t dialogue_id;
} QuestMarkSource;

/** Work out the marks one character sees over a set of NPC types.
 *
 * One entry per type that has something to say: a finished quest to hand in
 * wins over a new one to take. Types with nothing for this character are left
 * out.
 *
 * @param out  Receives up to max_out entries.
 * @return     How many were written.
 */
int quest_marks_compute(uint32_t character_id, const QuestMarkSource* sources,
                        int source_count, NPCQuestMarkEntry* out, int max_out);

// One quest a character may take, and the NPC type that offers it.
typedef struct {
	uint32_t quest_id;
	uint8_t  giver_npc_type;
} available_quest;

/** List every quest one character may take right now, one entry per giver.
 *
 * The same rule as the marks -- an accept option whose conditions hold and
 * whose quest the character qualifies for -- asked for every option instead of
 * the strongest one per NPC type. Nothing caps the list.
 *
 * @param out  Receives a heap array the caller frees, or NULL when empty.
 * @return     How many entries, or -1 on allocation failure.
 */
int quest_available_compute(uint32_t character_id, const QuestMarkSource* sources,
                            int source_count, available_quest** out);

/** Send a character the marks over NPC heads and the quests they may take.
 *
 * The server is the only voice here: the client shows what it is told and
 * accepting a quest is checked again when the request arrives.
 */
void quest_marks_send(uint32_t character_id, int client_fd);

#endif // QUEST_MARKS_H
