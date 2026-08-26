/**
 * @file
 * Parse dialogue documents into heap-allocated definitions.
 *
 * Built on the shared JSON tree parser rather than substring scanning. That is
 * not a style preference: the scanner this replaced looked up a key with
 * strstr() from the start of an object, so an option that omitted "next_page"
 * silently inherited the next option's, and a page that omitted "text" borrowed
 * one from an option below it. A tree cannot do that.
 */

#include "dialogue_system.h"
#include "json_util.h"
#include "log.h"
#include "race_registry.h"

#include <stdlib.h>
#include <string.h>

/** Copy a string onto the heap, substituting "" for a missing value. */
static char* dup_text(const char* text) {
    if (!text) text = "";
    size_t len = strlen(text);
    char* out = malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, text, len + 1);
    return out;
}

/** Map a condition's JSON spelling to its kind. */
typedef struct {
    const char*           key;
    DialogueConditionKind kind;
    int                   value_is_race_key; /**< Nonzero when "value" names a race. */
} ConditionSpelling;

/** Name every condition kind exactly once.
 *
 * The table is the whole of the loader's knowledge of conditions: a new kind is
 * one row here and one case in the evaluator.
 */
static const ConditionSpelling CONDITION_SPELLINGS[] = {
    { "race",              DIALOGUE_COND_RACE,              1 },
    { "not_race",          DIALOGUE_COND_NOT_RACE,          1 },
    { "min_level",         DIALOGUE_COND_MIN_LEVEL,         0 },
    { "max_level",         DIALOGUE_COND_MAX_LEVEL,         0 },
    { "quest_not_started", DIALOGUE_COND_QUEST_NOT_STARTED, 0 },
    { "quest_active",      DIALOGUE_COND_QUEST_ACTIVE,      0 },
    { "quest_complete",    DIALOGUE_COND_QUEST_COMPLETE,    0 },
    { "quest_turned_in",   DIALOGUE_COND_QUEST_TURNED_IN,   0 },
    { "quest_available",   DIALOGUE_COND_QUEST_AVAILABLE,   0 },
};

#define CONDITION_SPELLING_COUNT \
    ((int)(sizeof(CONDITION_SPELLINGS) / sizeof(CONDITION_SPELLINGS[0])))

/** Map an action's JSON spelling to its identifier. */
typedef struct {
    const char* key;
    uint8_t     action;
} ActionSpelling;

static const ActionSpelling ACTION_SPELLINGS[] = {
    { "open_shop",    DIALOGUE_ACTION_OPEN_SHOP    },
    { "quest_accept", DIALOGUE_ACTION_QUEST_ACCEPT },
    { "quest_turnin", DIALOGUE_ACTION_QUEST_TURNIN },
};

#define ACTION_SPELLING_COUNT \
    ((int)(sizeof(ACTION_SPELLINGS) / sizeof(ACTION_SPELLINGS[0])))

/**
 * Resolve one condition's value, which may be a race key or a plain number.
 *
 * A race key is resolved here, at load, so evaluation never touches the race
 * registry and a misspelled race is reported once instead of failing silently
 * for every player who talks to the NPC.
 *
 * @return 1 when the value resolved, or 0 when it names nothing.
 */
static int resolve_condition_value(const ConditionSpelling* spelling,
                                   const JsonValue* value_node,
                                   const char* source, uint32_t dialogue_id,
                                   uint32_t* out_value) {
    if (spelling->value_is_race_key && json_type(value_node) == JSON_STRING) {
        const char* key = json_as_string(value_node, "");
        const RaceDef* race = race_get_by_key(key);
        if (!race) {
            LOG_ERROR("[DIALOGUE] %s: dialogue %u names race '%s', which is not in "
                      "races.json; the option carrying it will never be offered",
                      source, dialogue_id, key);
            return 0;
        }
        *out_value = race->id;
        return 1;
    }

    if (json_type(value_node) != JSON_NUMBER) return 0;
    double n = json_as_number(value_node, 0.0);
    if (n < 0.0) return 0;
    *out_value = (uint32_t)n;
    return 1;
}

/**
 * Parse one option's condition list.
 *
 * @param out_conditions  Receives an owned array, or NULL when there are none.
 * @return                The condition count, which is 0 for an absent list.
 */
static int parse_conditions(const JsonValue* option_obj, const char* source,
                            uint32_t dialogue_id, DialogueCondition** out_conditions) {
    *out_conditions = NULL;

    const JsonValue* list = json_get(option_obj, "conditions");
    if (json_type(list) != JSON_ARRAY) return 0;

    int authored = json_count(list);
    if (authored <= 0) return 0;

    DialogueCondition* conditions = calloc((size_t)authored, sizeof(*conditions));
    if (!conditions) return 0;

    int count = 0;
    for (int i = 0; i < authored; i++) {
        const JsonValue* entry = json_at(list, i);
        const char* type = json_get_string(entry, "type", NULL);
        if (!type) {
            LOG_ERROR("[DIALOGUE] %s: dialogue %u has a condition with no \"type\"",
                      source, dialogue_id);
            continue;
        }

        const ConditionSpelling* spelling = NULL;
        for (int s = 0; s < CONDITION_SPELLING_COUNT; s++) {
            if (strcmp(CONDITION_SPELLINGS[s].key, type) == 0) {
                spelling = &CONDITION_SPELLINGS[s];
                break;
            }
        }
        if (!spelling) {
            LOG_ERROR("[DIALOGUE] %s: dialogue %u uses unknown condition type '%s'",
                      source, dialogue_id, type);
            continue;
        }

        uint32_t value = 0;
        if (!resolve_condition_value(spelling, json_get(entry, "value"),
                                     source, dialogue_id, &value)) {
            LOG_ERROR("[DIALOGUE] %s: dialogue %u condition '%s' has no usable \"value\"",
                      source, dialogue_id, type);
            continue;
        }

        conditions[count].kind  = (uint8_t)spelling->kind;
        conditions[count].value = value;
        count++;
    }

    if (count == 0) {
        free(conditions);
        return 0;
    }

    *out_conditions = conditions;
    return count;
}

/** Release an options array and everything it owns. */
static void free_options(DialogueOptionDef* options, int count) {
    if (!options) return;
    for (int i = 0; i < count; i++) {
        free(options[i].text);
        free(options[i].conditions);
    }
    free(options);
}

/**
 * Parse one page's options.
 *
 * @param out_options  Receives an owned array, or NULL when there are none.
 * @return             The option count, or -1 on allocation failure.
 */
static int parse_options(const JsonValue* page_obj, const char* source,
                         uint32_t dialogue_id, int32_t page_num,
                         DialogueOptionDef** out_options) {
    *out_options = NULL;

    const JsonValue* list = json_get(page_obj, "options");
    if (json_type(list) != JSON_ARRAY) return 0;

    int authored = json_count(list);
    if (authored <= 0) return 0;

    DialogueOptionDef* options = calloc((size_t)authored, sizeof(*options));
    if (!options) return -1;

    int count = 0;
    for (int i = 0; i < authored; i++) {
        const JsonValue* entry = json_at(list, i);
        DialogueOptionDef* option = &options[count];

        int id = json_get_int(entry, "option_id", -1);
        if (id < 0 || id > 255) {
            LOG_ERROR("[DIALOGUE] %s: dialogue %u page %d has an option with "
                      "option_id %d; ids must be 0..255", source, dialogue_id, page_num, id);
            continue;
        }

        /* Selection travels back as an option_id, so a repeated id inside one
         * page would make two different choices indistinguishable. */
        int duplicate = 0;
        for (int j = 0; j < count; j++) {
            if (options[j].option_id == (uint8_t)id) { duplicate = 1; break; }
        }
        if (duplicate) {
            LOG_ERROR("[DIALOGUE] %s: dialogue %u page %d repeats option_id %d; "
                      "ids must be unique within a page", source, dialogue_id, page_num, id);
            continue;
        }

        option->option_id    = (uint8_t)id;
        option->text         = dup_text(json_get_string(entry, "text", ""));
        option->next_page    = json_get_int(entry, "next_page", DIALOGUE_PAGE_CLOSE);
        option->fail_page    = json_get_int(entry, "fail_page", DIALOGUE_PAGE_INHERIT);
        option->action       = DIALOGUE_ACTION_NONE;
        option->action_value = (uint32_t)json_get_int(entry, "action_value", 0);

        if (!option->text) {
            free_options(options, count);
            return -1;
        }

        const char* action = json_get_string(entry, "action", NULL);
        if (action) {
            int matched = 0;
            for (int a = 0; a < ACTION_SPELLING_COUNT; a++) {
                if (strcmp(ACTION_SPELLINGS[a].key, action) == 0) {
                    option->action = ACTION_SPELLINGS[a].action;
                    matched = 1;
                    break;
                }
            }
            if (!matched)
                LOG_ERROR("[DIALOGUE] %s: dialogue %u page %d uses unknown action '%s'",
                          source, dialogue_id, page_num, action);
        }

        option->condition_count = parse_conditions(entry, source, dialogue_id,
                                                   &option->conditions);
        count++;
    }

    if (count == 0) {
        free(options);
        return 0;
    }

    *out_options = options;
    return count;
}

/**
 * Build one dialogue definition from a parsed JSON object.
 *
 * @return An owned definition, or NULL when the object carries no usable id.
 */
static DialogueDef* parse_dialogue_object(const JsonValue* obj, const char* source) {
    int id = json_get_int(obj, "id", 0);
    if (id <= 0) {
        LOG_ERROR("[DIALOGUE] %s: dialogue object has no positive \"id\"", source);
        return NULL;
    }

    DialogueDef* dialogue = calloc(1, sizeof(*dialogue));
    if (!dialogue) return NULL;

    dialogue->dialogue_id = (uint32_t)id;
    dialogue->name        = dup_text(json_get_string(obj, "name", "Unnamed"));
    if (!dialogue->name) { free(dialogue); return NULL; }

    const JsonValue* pages = json_get(obj, "pages");
    int authored = (json_type(pages) == JSON_ARRAY) ? json_count(pages) : 0;
    if (authored <= 0) {
        LOG_ERROR("[DIALOGUE] %s: dialogue %d '%s' has no pages",
                  source, id, dialogue->name);
        return dialogue;
    }

    dialogue->pages = calloc((size_t)authored, sizeof(*dialogue->pages));
    if (!dialogue->pages) { dialogue_def_free(dialogue); return NULL; }

    for (int i = 0; i < authored; i++) {
        const JsonValue* page_obj = json_at(pages, i);
        DialoguePageDef* page = &dialogue->pages[dialogue->page_count];

        page->page_num = json_get_int(page_obj, "page_num", i);
        page->text     = dup_text(json_get_string(page_obj, "text", ""));
        if (!page->text) { dialogue_def_free(dialogue); return NULL; }

        /* Navigation targets a page number, so two pages sharing one number
         * make every jump to it ambiguous. */
        int duplicate = 0;
        for (int j = 0; j < dialogue->page_count; j++) {
            if (dialogue->pages[j].page_num == page->page_num) { duplicate = 1; break; }
        }
        if (duplicate) {
            LOG_ERROR("[DIALOGUE] %s: dialogue %d repeats page_num %d; dropping the repeat",
                      source, id, page->page_num);
            free(page->text);
            memset(page, 0, sizeof(*page));
            continue;
        }

        int option_count = parse_options(page_obj, source, dialogue->dialogue_id,
                                         page->page_num, &page->options);
        if (option_count < 0) { dialogue_def_free(dialogue); return NULL; }
        page->option_count = option_count;

        dialogue->page_count++;
    }

    return dialogue;
}

/**
 * Parse a dialogue document and install everything it defines.
 *
 * Accepts both shapes the data directory uses: a file holding a single dialogue
 * object, and a file holding a "dialogues" array of them.
 *
 * @return The number of dialogues installed.
 */
int dialogue_load_document(const char* json_text, const char* source) {
    JsonValue* root = json_parse(json_text);
    if (!root) {
        LOG_ERROR("[DIALOGUE] %s: not valid JSON", source);
        return 0;
    }

    int installed = 0;
    const JsonValue* array = json_get(root, "dialogues");

    if (json_type(array) == JSON_ARRAY) {
        int count = json_count(array);
        for (int i = 0; i < count; i++) {
            DialogueDef* dialogue = parse_dialogue_object(json_at(array, i), source);
            if (dialogue && dialogue_registry_install(dialogue)) installed++;
        }
    } else {
        DialogueDef* dialogue = parse_dialogue_object(root, source);
        if (dialogue && dialogue_registry_install(dialogue)) installed++;
    }

    json_free(root);
    return installed;
}

/**
 * Release a definition and every allocation it owns.
 */
void dialogue_def_free(DialogueDef* dialogue) {
    if (!dialogue) return;

    for (int i = 0; i < dialogue->page_count; i++) {
        free(dialogue->pages[i].text);
        free_options(dialogue->pages[i].options, dialogue->pages[i].option_count);
    }
    free(dialogue->pages);
    free(dialogue->name);
    free(dialogue);
}
