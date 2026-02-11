// ============================================================================
// dialogue_loader.c — JSON parser for dialogue definitions
//
// Manual JSON parsing following the same pattern as ability_def.c.
// No external JSON library used.
// ============================================================================

#include "dialogue_system.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// ---------------------------------------------------------------------------
// Forward declarations — JSON helpers
// ---------------------------------------------------------------------------

static const char* find_json_value(const char* json, const char* key);
static int parse_json_string(const char* val, char* out, int out_size);
static int parse_json_int(const char* val);
static const char* find_json_array(const char* json, const char* key);
static const char* next_array_element(const char* arr_pos);
static const char* skip_whitespace(const char* str);

// ---------------------------------------------------------------------------
// Main parser
// ---------------------------------------------------------------------------

int dialogue_parse_json(const char* json_content, DialogueDef** dialogue_table, int max_dialogues) {
    int loaded = 0;

    // Find "dialogues" array
    const char* dialogues_array = find_json_array(json_content, "dialogues");
    if (!dialogues_array) {
        fprintf(stderr, "[DIALOGUE] No 'dialogues' array found in JSON\n");
        return -1;
    }

    const char* dialogue_obj = dialogues_array;

    // Parse each dialogue object
    while ((dialogue_obj = next_array_element(dialogue_obj)) != NULL) {
        DialogueDef* dialogue = calloc(1, sizeof(DialogueDef));
        if (!dialogue) {
            fprintf(stderr, "[DIALOGUE] Memory allocation failed\n");
            return -1;
        }

        // Parse dialogue ID
        const char* id_val = find_json_value(dialogue_obj, "id");
        if (id_val) {
            dialogue->dialogue_id = parse_json_int(id_val);
        }

        // Parse dialogue name
        const char* name_val = find_json_value(dialogue_obj, "name");
        if (name_val) {
            parse_json_string(name_val, dialogue->name, sizeof(dialogue->name));
        }

        // Parse pages array
        const char* pages_array = find_json_array(dialogue_obj, "pages");
        if (pages_array) {
            const char* page_obj = pages_array;
            int page_idx = 0;

            while ((page_obj = next_array_element(page_obj)) != NULL && page_idx < MAX_PAGES_PER_DIALOGUE) {
                DialoguePageDef* page = &dialogue->pages[page_idx];

                // Parse page_num
                const char* page_num_val = find_json_value(page_obj, "page_num");
                if (page_num_val) {
                    page->page_num = parse_json_int(page_num_val);
                }

                // Parse text
                const char* text_val = find_json_value(page_obj, "text");
                if (text_val) {
                    parse_json_string(text_val, page->text, sizeof(page->text));
                }

                // Parse options array
                const char* options_array = find_json_array(page_obj, "options");
                if (options_array) {
                    const char* option_obj = options_array;
                    int option_idx = 0;

                    while ((option_obj = next_array_element(option_obj)) != NULL && option_idx < MAX_DIALOGUE_OPTIONS) {
                        DialogueOptionDef* option = &page->options[option_idx];

                        // Parse option_id
                        const char* option_id_val = find_json_value(option_obj, "option_id");
                        if (option_id_val) {
                            option->option_id = parse_json_int(option_id_val);
                        }

                        // Parse text
                        const char* option_text_val = find_json_value(option_obj, "text");
                        if (option_text_val) {
                            parse_json_string(option_text_val, option->text, sizeof(option->text));
                        }

                        // Parse next_page
                        const char* next_page_val = find_json_value(option_obj, "next_page");
                        if (next_page_val) {
                            option->next_page = parse_json_int(next_page_val);
                        }

                        // Default enabled = 1
                        option->enabled = 1;

                        option_idx++;
                    }

                    page->option_count = option_idx;
                }

                page_idx++;
            }

            dialogue->page_count = page_idx;
        }

        // Store in table
        if (dialogue->dialogue_id > 0 && dialogue->dialogue_id < max_dialogues) {
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

// ---------------------------------------------------------------------------
// JSON helper functions (following ability_def.c pattern)
// ---------------------------------------------------------------------------

static const char* skip_whitespace(const char* str) {
    while (*str && isspace(*str)) str++;
    return str;
}

// Find "key": <value> and return pointer to <value>
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

// Parse a JSON string value "..." into output buffer
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

// Parse a JSON integer
static int parse_json_int(const char* val) {
    val = skip_whitespace(val);
    return atoi(val);
}

// Find "key": [...] and return pointer to start of array (after '[')
static const char* find_json_array(const char* json, const char* key) {
    const char* val = find_json_value(json, key);
    if (!val) return NULL;

    val = skip_whitespace(val);
    if (*val != '[') return NULL;

    return val + 1;  // Return position after '['
}

// Get next array element (handles objects and values)
// Returns NULL when array ends
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
