/** @file Define JSON dialogue trees, their visibility conditions, and active conversations.
 *
 * A dialogue is a numbered tree of pages; a page is text plus a list of options;
 * an option navigates, or fires one server action, or both. Nothing about that
 * shape is compiled in: pages, options, option text and the condition list on
 * each option are all heap-allocated at load, so a conversation is as long as
 * its author writes it. The one ceiling that remains is the wire's, and it is
 * named where it bites -- see dialogue_page_visible_options().
 *
 * Conditions are what let one NPC serve every race. An option carries a list of
 * them; the server evaluates the list against the asking player and sends only
 * the options that pass. The client never learns an option it may not pick,
 * which is also why option selection travels as an option_id rather than a row
 * index -- see dialogue_page_find_option().
 */

#ifndef DIALOGUE_SYSTEM_H
#define DIALOGUE_SYSTEM_H

#include <stdint.h>

/** Identify what a dialogue option requires of the player before it is offered.
 *
 * Adding a kind is one enum entry, one spelling in the loader's table, and one
 * case in the evaluator. No caller changes, and no data file that omits the new
 * kind is affected.
 */
typedef enum {
    DIALOGUE_COND_RACE = 0,          /**< value is a fused race/class id. */
    DIALOGUE_COND_NOT_RACE,          /**< value is a fused race/class id. */
    DIALOGUE_COND_MIN_LEVEL,         /**< value is a level. */
    DIALOGUE_COND_MAX_LEVEL,         /**< value is a level. */
    DIALOGUE_COND_QUEST_NOT_STARTED, /**< value is a quest id never accepted. */
    DIALOGUE_COND_QUEST_ACTIVE,      /**< value is an accepted quest, objectives outstanding. */
    DIALOGUE_COND_QUEST_COMPLETE,    /**< value is an accepted quest, objectives met, not turned in. */
    DIALOGUE_COND_QUEST_TURNED_IN,   /**< value is a quest already handed in. */
    /** value is a quest the character could accept right now.
     *
     * Asks the quest system, which owns the quest's race, level and
     * prerequisite rules. Prefer this to restating a prerequisite here: stated
     * twice, an offer and a grant can disagree, and the player is the one who
     * finds out. */
    DIALOGUE_COND_QUEST_AVAILABLE,
    DIALOGUE_COND_KIND_COUNT
} DialogueConditionKind;

/** Require one fact about the player before an option is offered. */
typedef struct {
    uint8_t  kind;   /**< DialogueConditionKind. */
    uint32_t value;  /**< Race id, level, or quest id, per kind. */
} DialogueCondition;

/** Identify server actions triggered by dialogue choices. */
#define DIALOGUE_ACTION_NONE         0
#define DIALOGUE_ACTION_OPEN_SHOP    1   /**< action_value = shop_id. */
#define DIALOGUE_ACTION_QUEST_ACCEPT 2   /**< action_value = quest_id. */
#define DIALOGUE_ACTION_QUEST_TURNIN 3   /**< action_value = quest_id; fail_page used if not complete. */

/** Close the dialogue instead of navigating to another page. */
#define DIALOGUE_PAGE_CLOSE (-1)

/** Fall back to next_page when an action's failure path is unset. */
#define DIALOGUE_PAGE_INHERIT (-2)

/** Define one player choice, what it requires, and where it leads. */
typedef struct {
    uint8_t  option_id;          /**< Unique within its page; what the client sends back. */
    char*    text;               /**< Owned; never NULL after a successful load. */
    int32_t  next_page;          /**< Page number, or DIALOGUE_PAGE_CLOSE. */
    int32_t  fail_page;          /**< Failure page, or DIALOGUE_PAGE_INHERIT. */
    uint8_t  action;             /**< DIALOGUE_ACTION_*. */
    uint32_t action_value;       /**< Shop or quest id, per action. */

    DialogueCondition* conditions;    /**< Owned; NULL when the option is unconditional. */
    int                condition_count;
} DialogueOptionDef;

/** Define one text page and every choice authored on it. */
typedef struct {
    int32_t  page_num;
    char*    text;               /**< Owned; never NULL after a successful load. */
    DialogueOptionDef* options;  /**< Owned. */
    int                option_count;
} DialoguePageDef;

/** Define one complete named dialogue tree. */
typedef struct {
    uint32_t dialogue_id;
    char*    name;               /**< Owned. */
    DialoguePageDef* pages;      /**< Owned, in page_num order as authored. */
    int              page_count;
} DialogueDef;

/** Track one active conversation between a player and an NPC. */
typedef struct {
    uint32_t character_id;
    uint32_t npc_id;
    uint32_t dialogue_id;
    int32_t  current_page;
    uint8_t  is_active;
    double   last_interaction_time;
} DialogueSession;

/** Load every JSON document in a directory into the registry.
 *
 * Replaces any previous contents. A dialogue whose file fails to parse is
 * skipped and reported; the rest still load.
 *
 * @return 1 after scanning the directory, or 0 when it cannot be opened.
 */
int dialogue_system_init(const char* dir_path);

/** Release every definition and close every session. */
void dialogue_system_cleanup(void);

/** Look up a dialogue by identifier.
 *
 * @return A registry-owned definition, or NULL when absent.
 */
const DialogueDef* dialogue_get(uint32_t dialogue_id);

/** Report how many dialogues are loaded. */
int dialogues_get_count(void);

/** Find a page by its authored page number.
 *
 * Page numbers are data, not array indices: a document may number its pages
 * with gaps, and this is the only correct way to reach one.
 *
 * @return A registry-owned page, or NULL when the dialogue has no such page.
 */
const DialoguePageDef* dialogue_find_page(const DialogueDef* dialogue, int32_t page_num);

/** Find an option on a page by the identifier the client sent back.
 *
 * @return A registry-owned option, or NULL when the page has no such option.
 */
const DialogueOptionDef* dialogue_page_find_option(const DialoguePageDef* page, uint8_t option_id);

/** Report whether every condition on an option holds for a character.
 *
 * An option with no conditions always passes. An option naming a quest or race
 * that no longer exists fails closed rather than being offered.
 *
 * @return 1 when the option may be offered, or 0 otherwise.
 */
int dialogue_option_available(const DialogueOptionDef* option, uint32_t character_id);

/** Collect the options on a page that a character may currently be offered.
 *
 * @param out_ids   Receives up to max_ids option identifiers, in authored order.
 * @param max_ids   Capacity of out_ids; the wire cap, MAX_DIALOGUE_OPTIONS.
 * @return          How many identifiers were written. When a page offers more
 *                  than max_ids at once the surplus is dropped and reported:
 *                  that is an authoring error, because the packet carries a
 *                  fixed number of choices and the player cannot pick what was
 *                  never sent.
 */
int dialogue_page_visible_options(const DialoguePageDef* page, uint32_t character_id,
                                  uint8_t* out_ids, int max_ids);

/** Open or replace a character's conversation.
 *
 * Returns a result, not a pointer into the table: the table rehashes as
 * conversations come and go, so any borrowed pointer stops being valid the
 * moment the lock is released. Read a conversation with
 * dialogue_session_snapshot() instead.
 *
 * @return 1 when the conversation is stored, or 0 when it cannot be.
 */
int dialogue_session_open(uint32_t character_id, uint32_t npc_id, uint32_t dialogue_id);

/** Copy a character's active conversation.
 *
 * Returns a copy rather than a pointer into the table: the table rehashes as
 * conversations come and go, so a borrowed pointer has no defined lifetime.
 *
 * @param out  Receives the session when one is active; may be NULL.
 * @return     1 when a conversation is active, or 0 otherwise.
 */
int dialogue_session_snapshot(uint32_t character_id, DialogueSession* out);

/** End a character's conversation. Safe when none is open. */
void dialogue_session_close(uint32_t character_id);

/** Move an active conversation to another page and refresh its idle timer. */
void dialogue_session_update_page(uint32_t character_id, int32_t new_page);

/** Close conversations left idle, from a periodic world update. */
void dialogue_check_timeouts(void);

/* --- Loader entry point, shared with dialogue_loader.c ------------------- */

/** Parse one dialogue document and hand ownership to the registry.
 *
 * @param json_text  Terminated JSON: either one dialogue object, or a document
 *                   with a "dialogues" array of them.
 * @param source     File name used in diagnostics.
 * @return           The number of dialogues installed, or 0 when none were.
 */
int dialogue_load_document(const char* json_text, const char* source);

/** Install one parsed definition, replacing any dialogue with the same id.
 *
 * The registry takes ownership on success and frees the definition on failure.
 *
 * @return 1 when installed, or 0 when the identifier is unusable.
 */
int dialogue_registry_install(DialogueDef* dialogue);

/** Release one definition and everything it owns. Tolerates NULL. */
void dialogue_def_free(DialogueDef* dialogue);

#endif // DIALOGUE_SYSTEM_H
