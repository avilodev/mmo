/**
 * @file
 * Parse per-NPC and legacy aggregate dialogue JSON documents.
 */

#include "dialogue_system.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static const char* find_json_value(const char* json, const char* key);
static int parse_json_string(const char* val, char* out, int out_size);
static int parse_json_int(const char* val);
static const char* find_json_array(const char* json, const char* key);
static const char* next_array_element(const char* arr_pos);
static const char* skip_whitespace(const char* str);

/**
 * Populate one dialogue definition from a JSON object.
 */
static void parse_dialogue_object(const char* obj, DialogueDef* dialogue) {
    const char* id_val = find_json_value(obj, "id");
    if (id_val) dialogue->dialogue_id = parse_json_int(id_val);

    const char* name_val = find_json_value(obj, "name");
    if (name_val) parse_json_string(name_val, dialogue->name, sizeof(dialogue->name));

    const char* pages_array = find_json_array(obj, "pages");
    if (!pages_array) return;

    const char* page_obj = pages_array;
    int page_idx = 0;

    while ((page_obj = next_array_element(page_obj)) != NULL && page_idx < MAX_PAGES_PER_DIALOGUE) {
        DialoguePageDef* page = &dialogue->pages[page_idx];

        const char* page_num_val = find_json_value(page_obj, "page_num");
        if (page_num_val) page->page_num = parse_json_int(page_num_val);

        const char* text_val = find_json_value(page_obj, "text");
        if (text_val) parse_json_string(text_val, page->text, sizeof(page->text));

        const char* options_array = find_json_array(page_obj, "options");
        if (options_array) {
            const char* option_obj = options_array;
            int option_idx = 0;

            while ((option_obj = next_array_element(option_obj)) != NULL && option_idx < MAX_DIALOGUE_OPTIONS) {
                DialogueOptionDef* option = &page->options[option_idx];

                const char* option_id_val = find_json_value(option_obj, "option_id");
                if (option_id_val) option->option_id = parse_json_int(option_id_val);

                const char* option_text_val = find_json_value(option_obj, "text");
                if (option_text_val) parse_json_string(option_text_val, option->text, sizeof(option->text));

                const char* next_page_val = find_json_value(option_obj, "next_page");
                if (next_page_val) option->next_page = parse_json_int(next_page_val);

                option->fail_page = -2;
                const char* fail_page_val = find_json_value(option_obj, "fail_page");
                if (fail_page_val) option->fail_page = parse_json_int(fail_page_val);

                option->action = DIALOGUE_ACTION_NONE;
                const char* action_val = find_json_value(option_obj, "action");
                if (action_val) {
                    char action_str[32] = {0};
                    parse_json_string(action_val, action_str, sizeof(action_str));
                    if      (strcmp(action_str, "open_shop")    == 0) option->action = DIALOGUE_ACTION_OPEN_SHOP;
                    else if (strcmp(action_str, "quest_accept") == 0) option->action = DIALOGUE_ACTION_QUEST_ACCEPT;
                    else if (strcmp(action_str, "quest_turnin") == 0) option->action = DIALOGUE_ACTION_QUEST_TURNIN;
                }

                option->action_value = 0;
                const char* action_value_val = find_json_value(option_obj, "action_value");
                if (action_value_val) option->action_value = (uint32_t)parse_json_int(action_value_val);

                option->enabled = 1;
                option_idx++;
            }

            page->option_count = option_idx;
        }

        page_idx++;
    }

    dialogue->page_count = page_idx;
}

/**
 * Parse and install one per-NPC dialogue document.
 *
 * Replaces and frees an existing definition with the same identifier.
 *
 * @param json_content   Terminated JSON object containing an id and pages.
 * @param dialogue_table Destination table that assumes ownership on success.
 * @param max_dialogues  Number of available table slots; must be positive.
 * @return               1 when installed, or 0 on allocation failure or an invalid identifier.
 */
int dialogue_parse_single(const char* json_content, DialogueDef** dialogue_table, int max_dialogues) {
    DialogueDef* dialogue = calloc(1, sizeof(DialogueDef));
    if (!dialogue) {
        fprintf(stderr, "[DIALOGUE] Memory allocation failed\n");
        return 0;
    }

    parse_dialogue_object(json_content, dialogue);

    if (max_dialogues > 0 && dialogue->dialogue_id > 0 &&
        dialogue->dialogue_id < (uint32_t)max_dialogues) {
        if (dialogue_table[dialogue->dialogue_id]) {
            free(dialogue_table[dialogue->dialogue_id]);
        }
        dialogue_table[dialogue->dialogue_id] = dialogue;
        printf("[DIALOGUE] Loaded '%s' (id=%u, %u pages)\n",
               dialogue->name, dialogue->dialogue_id, dialogue->page_count);
        return 1;
    }

    fprintf(stderr, "[DIALOGUE] Invalid dialogue ID: %u\n", dialogue->dialogue_id);
    free(dialogue);
    return 0;
}

/**
 * Parse and install dialogues from a legacy aggregate document.
 *
 * The destination table assumes ownership of every installed definition.
 *
 * @param json_content   Terminated JSON document containing a dialogues array.
 * @param dialogue_table Destination definition table.
 * @param max_dialogues  Number of available table slots; must be positive.
 * @return               The number installed, or -1 when the array is absent or allocation fails.
 */
int dialogue_parse_json(const char* json_content, DialogueDef** dialogue_table, int max_dialogues) {
    int loaded = 0;

    const char* dialogues_array = find_json_array(json_content, "dialogues");
    if (!dialogues_array) {
        fprintf(stderr, "[DIALOGUE] No 'dialogues' array found in JSON\n");
        return -1;
    }

    const char* dialogue_obj = dialogues_array;
    while ((dialogue_obj = next_array_element(dialogue_obj)) != NULL) {
        DialogueDef* dialogue = calloc(1, sizeof(DialogueDef));
        if (!dialogue) {
            fprintf(stderr, "[DIALOGUE] Memory allocation failed\n");
            return -1;
        }

        parse_dialogue_object(dialogue_obj, dialogue);

        if (max_dialogues > 0 && dialogue->dialogue_id > 0 &&
            dialogue->dialogue_id < (uint32_t)max_dialogues) {
            dialogue_table[dialogue->dialogue_id] = dialogue;
            loaded++;
            printf("[DIALOGUE] Loaded dialogue %u '%s' with %u pages\n",
                   dialogue->dialogue_id, dialogue->name, dialogue->page_count);
        } else {
            fprintf(stderr, "[DIALOGUE] Invalid dialogue ID: %u\n", dialogue->dialogue_id);
            free(dialogue);
        }
    }

    return loaded;
}

static const char* skip_whitespace(const char* str) {
    while (*str && isspace(*str)) str++;
    return str;
}

static const char* find_json_value(const char* json, const char* key) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);

    const char* pos = strstr(json, search);
    if (!pos) return NULL;

    pos = strchr(pos, ':');
    if (!pos) return NULL;
    pos++;

    return skip_whitespace(pos);
}

/**
 * Decode a quoted JSON string and its supported escape sequences.
 *
 * @param out       Destination buffer.
 * @param out_size  Destination capacity including the terminator.
 * @return          1 for a quoted value, or 0 otherwise.
 */
static int parse_json_string(const char* val, char* out, int out_size) {
    val = skip_whitespace(val);

    if (*val != '"') return 0;
    val++;

    int i = 0;
    while (*val && *val != '"' && i < out_size - 1) {
        // Handle escape sequences
        if (*val == '\\' && *(val + 1)) {
            val++;
            if (*val == 'n') out[i++] = '\n';
            else if (*val == 't') out[i++] = '\t';
            else if (*val == 'r') out[i++] = '\r';
            else if (*val == '"') out[i++] = '"';
            else if (*val == '\\') out[i++] = '\\';
            else out[i++] = *val;
            val++;
        } else {
            out[i++] = *val++;
        }
    }

    out[i] = '\0';
    return 1;
}

static int parse_json_int(const char* val) {
    val = skip_whitespace(val);
    return atoi(val);
}

static const char* find_json_array(const char* json, const char* key) {
    const char* val = find_json_value(json, key);
    if (!val) return NULL;

    val = skip_whitespace(val);
    if (*val != '[') return NULL;

    return val + 1;  // Return position after '['
}

/**
 * Locate the next object element in a JSON array.
 *
 * @return The next opening brace, or NULL at the array end or for a non-object element.
 */
static const char* next_array_element(const char* arr_pos) {
    arr_pos = skip_whitespace(arr_pos);

    // If we're currently at an object, skip past it first
    if (*arr_pos == '{') {
        int brace_count = 1;
        arr_pos++;

        while (*arr_pos && brace_count > 0) {
            if (*arr_pos == '{') brace_count++;
            else if (*arr_pos == '}') brace_count--;
            arr_pos++;
        }
    }

    // Skip whitespace and comma
    arr_pos = skip_whitespace(arr_pos);
    if (*arr_pos == ',') {
        arr_pos++;
        arr_pos = skip_whitespace(arr_pos);
    }

    // End of array
    if (*arr_pos == ']' || *arr_pos == '\0') {
        return NULL;
    }

    // Should be at start of next element
    if (*arr_pos == '{') {
        return arr_pos;
    }

    return NULL;
}
