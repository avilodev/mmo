#ifndef QUEST_REGISTRY_H
#define QUEST_REGISTRY_H

/** @file Hold the authored quest definitions loaded from quests.json.
 *
 * This is the world's copy of what a quest *is* -- its steps and their
 * objectives, who may take it, what it pays, and whether it may be taken more
 * than once. It is read-only once loaded, shared by every character, and never
 * written to disk: what a character has *done* with a quest lives in
 * quest_state.h instead.
 *
 * Nothing here has a compiled ceiling. Identifiers index a registry that grows
 * to whatever the file declares, so a quest may be numbered however its author
 * likes. The only bound is per packet: MAX_QUEST_OBJECTIVES is how many
 * objectives one quest can put on the wire.
 */

#include "protocol.h"       /* QuestObjectiveType, MAX_QUEST_OBJECTIVES */
#include "quest_repeat.h"   /* QuestRepeatMode */

#include <stdint.h>

/** Define one objective's source, target, description, and required count. */
typedef struct {
    uint8_t  type;              /**< QuestObjectiveType. */
    uint32_t target_id;         /**< npc_type_id (kill/talk) or item_id (collect/buy/sell). */
    char     description[64];   /**< e.g. "Kill 5 Wolves". */
    int32_t  required_count;

    /** Pin the map marker instead of resolving it from the live world.
     *
     * Left unset for anything that has a spawned NPC to point at, which is the
     * normal case. Set it for an objective whose target is a place rather than
     * an entity, such as collecting from a region.
     */
    uint8_t  has_marker;
    float    marker_x;
    float    marker_y;

    /** Point the marker at the nearest living NPC of this type instead.
     *
     * For an objective whose target is an item -- buy it from this merchant,
     * sell it to that one -- the item has no position, but the NPC does. Zero
     * means "none"; kill and talk objectives need no such hint because their
     * own target is the NPC.
     */
    uint16_t marker_npc;
} QuestObjectiveDef;

/** Define one item stack granted as a quest reward. */
typedef struct {
    uint32_t item_id;
    uint8_t  quantity;
} QuestItemReward;

/** Define one step of a quest: what to do, and what finishing it pays.
 *
 * A quest is an ordered list of these. Finishing one pays its rewards at once
 * and moves the quest to the next; the last one is handed in to an NPC like a
 * whole quest used to be. A quest authored without steps is one unnamed step.
 */
typedef struct {
    char              title[48];    /**< Empty for an unnamed step. */
    uint8_t           obj_count;
    QuestObjectiveDef objectives[MAX_QUEST_OBJECTIVES];

    uint32_t          xp_reward;
    uint32_t          currency_reward;   /**< Paid in the quest's currency_id. */
    uint8_t           item_reward_count;
    QuestItemReward   item_rewards[MAX_QUEST_OBJECTIVES];
} QuestStepDef;

/** Bound the steps in one quest; the step number travels as one byte. */
#define MAX_QUEST_STEPS 255

/** Aggregate steps, entry requirements, and repeat rules for one quest. */
typedef struct {
    uint32_t          quest_id;
    char              title[48];

    /** The steps, in order. Heap-allocated; step_count is at least 1. */
    uint8_t           step_count;
    QuestStepDef*     steps;

    /** Coin kingdom every step pays in. */
    uint8_t           currency_id;

    /** Refuse the quest unless the character matches. Zero means "no requirement".
     *
     * This is where a chain is declared, once. Dialogue conditions ask the
     * quest whether a character qualifies rather than restating its
     * prerequisites, so an offer and a grant cannot disagree -- and a
     * fabricated accept packet is refused by the same rule that hid the offer.
     */
    uint32_t          require_race_id;
    int32_t           require_level;

    /** Prerequisites, as quest identifiers that must already be finished.
     *
     * Two lists because both shapes are real: a chain wants every step done
     * ("all"), while a quest reachable from several starts wants any one of
     * them ("any") -- the opening sendoff is offered by nine different race
     * leaders and needs exactly that. Both are heap-allocated; neither is
     * capped.
     */
    uint32_t*         require_all;
    int               require_all_count;
    uint32_t*         require_any;
    int               require_any_count;

    /** Say whether finishing this quest is the end of it.
     *
     * QUEST_REPEAT_ONCE, the default and what every story quest wants, is what
     * makes completion history the whole answer to "may they take it again".
     * Anything else hands that question to quest_repeat_allows(), which needs
     * the character's counter -- which is why a repeatable turn-in writes two
     * tables rather than one.
     */
    QuestRepeatMode   repeat_mode;
    /** Stop offering a repeatable after this many turn-ins. Zero means never. */
    uint16_t          max_count;
} QuestDef;

/** Load quest definitions from JSON, replacing any previous registry.
 *
 * A missing file is nonfatal: the world runs with no quests.
 *
 * @return 1 when the registry is usable, or 0 on allocation failure.
 */
int quest_registry_load(const char* json_path);

/** Release every loaded definition. Safe on an empty registry. */
void quest_registry_clear(void);

/** Look up a quest.
 *
 * @return A registry-owned definition, or NULL when absent.
 */
const QuestDef* quest_get(uint32_t quest_id);

/** Look up one step of a quest.
 *
 * @param step  The step, counted from 0.
 * @return      A registry-owned step, or NULL when the quest has no such step.
 */
const QuestStepDef* quest_step_get(const QuestDef* quest, int step);

/** Report whether a step is the one that gets handed in. */
int quest_step_is_last(const QuestDef* quest, int step);

#endif // QUEST_REGISTRY_H
