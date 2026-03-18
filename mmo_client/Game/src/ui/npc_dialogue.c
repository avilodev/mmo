#include "ui/npc_dialogue.h"
#include "renderer.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <dirent.h>

// ============================================================================
// Static Storage
// ============================================================================

static DialogueDef* dialogue_table[MAX_DIALOGUES] = {0};
static int dialogues_loaded = 0;
static DialogueState g_dialogue = {0};

// ============================================================================
// JSON Parsing Helpers
// ============================================================================

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

static int parse_json_string(const char* val, char* out, int out_size) {
    val = skip_whitespace(val);
    if (*val != '"') return 0;
    val++;

    int i = 0;
    while (*val && *val != '"' && i < out_size - 1) {
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

    return val + 1;
}

static const char* next_array_element(const char* arr_pos) {
    arr_pos = skip_whitespace(arr_pos);

    if (*arr_pos == ']' || *arr_pos == '\0') {
        return NULL;
    }

    if (*arr_pos == ',') {
        arr_pos++;
        arr_pos = skip_whitespace(arr_pos);
    }

    if (*arr_pos == '{') {
        const char* obj_start = arr_pos;
        int brace_count = 1;
        arr_pos++;

        while (*arr_pos && brace_count > 0) {
            if (*arr_pos == '{') brace_count++;
            else if (*arr_pos == '}') brace_count--;
            arr_pos++;
        }

        return obj_start;
    }

    return NULL;
}

static char* read_file(const char* filepath) {
    FILE* f = fopen(filepath, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char* buffer = malloc(size + 1);
    if (!buffer) {
        fclose(f);
        return NULL;
    }

    fread(buffer, 1, size, f);
    buffer[size] = '\0';
    fclose(f);

    return buffer;
}

// ============================================================================
// Dialogue System Init/Cleanup
// ============================================================================

// Parse one per-NPC JSON file {"id":N,"name":"...","pages":[...]}
static int load_single_dialogue(const char* json_content) {
    DialogueDef* dialogue = calloc(1, sizeof(DialogueDef));
    if (!dialogue) return 0;

    const char* id_val = find_json_value(json_content, "id");
    if (id_val) dialogue->dialogue_id = parse_json_int(id_val);

    const char* name_val = find_json_value(json_content, "name");
    if (name_val) parse_json_string(name_val, dialogue->name, sizeof(dialogue->name));

    const char* pages_array = find_json_array(json_content, "pages");
    if (pages_array) {
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

                    option_idx++;
                }
                page->option_count = option_idx;
            }
            page_idx++;
        }
        dialogue->page_count = page_idx;
    }

    if (dialogue->dialogue_id > 0 && dialogue->dialogue_id < MAX_DIALOGUES) {
        if (dialogue_table[dialogue->dialogue_id]) free(dialogue_table[dialogue->dialogue_id]);
        dialogue_table[dialogue->dialogue_id] = dialogue;
        printf("[DIALOGUE] Loaded '%s' (id=%u, %u pages)\n",
               dialogue->name, dialogue->dialogue_id, dialogue->page_count);
        return 1;
    }

    fprintf(stderr, "[DIALOGUE] Invalid dialogue ID: %u\n", dialogue->dialogue_id);
    free(dialogue);
    return 0;
}

int dialogue_system_init(const char* dir_path) {
    printf("[DIALOGUE] Loading dialogues from directory: %s\n", dir_path);

    memset(dialogue_table, 0, sizeof(dialogue_table));
    memset(&g_dialogue, 0, sizeof(g_dialogue));
    dialogues_loaded = 0;

    g_dialogue.window_width  = 600.0f;
    g_dialogue.window_height = 400.0f;

    DIR* dir = opendir(dir_path);
    if (!dir) {
        fprintf(stderr, "[DIALOGUE] Failed to open dialogue directory: %s\n", dir_path);
        return 0;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        const char* fname = entry->d_name;
        size_t len = strlen(fname);
        if (len < 6 || strcmp(fname + len - 5, ".json") != 0)
            continue;

        char filepath[512];
        snprintf(filepath, sizeof(filepath), "%s/%s", dir_path, fname);

        char* json_content = read_file(filepath);
        if (!json_content) {
            fprintf(stderr, "[DIALOGUE] Could not read: %s\n", filepath);
            continue;
        }

        if (load_single_dialogue(json_content)) dialogues_loaded++;
        free(json_content);
    }

    closedir(dir);
    printf("[DIALOGUE] Successfully loaded %d dialogues\n", dialogues_loaded);
    return 1;
}

void dialogue_system_cleanup(void) {
    for (int i = 0; i < MAX_DIALOGUES; i++) {
        if (dialogue_table[i]) {
            free(dialogue_table[i]);
            dialogue_table[i] = NULL;
        }
    }

    memset(&g_dialogue, 0, sizeof(g_dialogue));
    dialogues_loaded = 0;
    printf("[DIALOGUE] Dialogue system cleaned up\n");
}

const DialogueDef* dialogue_get(uint32_t dialogue_id) {
    if (dialogue_id == 0 || dialogue_id >= MAX_DIALOGUES) {
        return NULL;
    }
    return dialogue_table[dialogue_id];
}

// ============================================================================
// Dialogue UI Functions
// ============================================================================

static void dialogue_lookup_and_display(uint32_t dialogue_id, uint8_t page_num,
                                        uint8_t option_count, const uint8_t* option_ids) {
    const DialogueDef* dialogue = dialogue_get(dialogue_id);
    if (!dialogue) {
        fprintf(stderr, "[DIALOGUE] Dialogue %u not found\n", dialogue_id);
        return;
    }

    if (page_num >= dialogue->page_count) {
        fprintf(stderr, "[DIALOGUE] Page %u out of range for dialogue %u\n", page_num, dialogue_id);
        return;
    }

    const DialoguePageDef* page = &dialogue->pages[page_num];

    strncpy(g_dialogue.displayed_text, page->text, MAX_DIALOGUE_TEXT_LENGTH - 1);
    g_dialogue.displayed_text[MAX_DIALOGUE_TEXT_LENGTH - 1] = '\0';

    g_dialogue.displayed_option_count = option_count;

    for (int i = 0; i < option_count && i < MAX_DIALOGUE_OPTIONS; i++) {
        g_dialogue.option_ids[i] = option_ids[i];

        int found = 0;
        for (int j = 0; j < page->option_count; j++) {
            if (page->options[j].option_id == option_ids[i]) {
                strncpy(g_dialogue.displayed_options[i], page->options[j].text, MAX_OPTION_TEXT_LENGTH - 1);
                g_dialogue.displayed_options[i][MAX_OPTION_TEXT_LENGTH - 1] = '\0';
                found = 1;
                break;
            }
        }

        if (!found) {
            snprintf(g_dialogue.displayed_options[i], MAX_OPTION_TEXT_LENGTH, "Option %u", option_ids[i]);
        }
    }
}

void dialogue_show(uint32_t npc_id, const char* npc_name, uint32_t dialogue_id,
                  uint8_t page_num, uint8_t option_count, const uint8_t* option_ids) {
    g_dialogue.is_active = true;
    g_dialogue.npc_id = npc_id;
    g_dialogue.dialogue_id = dialogue_id;
    g_dialogue.current_page = page_num;
    g_dialogue.selected_option = -1;

    strncpy(g_dialogue.npc_name, npc_name, MAX_NPC_NAME_LENGTH - 1);
    g_dialogue.npc_name[MAX_NPC_NAME_LENGTH - 1] = '\0';

    dialogue_lookup_and_display(dialogue_id, page_num, option_count, option_ids);

    printf("[DIALOGUE] Showing dialogue %u page %u for NPC %u: %s\n",
           dialogue_id, page_num, npc_id, npc_name);
}

void dialogue_update_page(uint32_t dialogue_id, uint8_t page_num,
                         uint8_t option_count, const uint8_t* option_ids) {
    if (!g_dialogue.is_active) return;

    g_dialogue.dialogue_id = dialogue_id;
    g_dialogue.current_page = page_num;
    g_dialogue.selected_option = -1;

    dialogue_lookup_and_display(dialogue_id, page_num, option_count, option_ids);

    printf("[DIALOGUE] Updated to page %u\n", page_num);
}

void dialogue_close(void) {
    g_dialogue.is_active = false;
    g_dialogue.selected_option = -1;
    printf("[DIALOGUE] Closed dialogue window\n");
}

bool dialogue_is_active(void) {
    return g_dialogue.is_active;
}

void dialogue_update_state(float delta_time) {
    (void)delta_time;
}

void dialogue_render(void) {
    if (!g_dialogue.is_active) {
        return;
    }

    float screen_width = 1920.0f;
    float screen_height = 1080.0f;

    g_dialogue.window_x = (screen_width - g_dialogue.window_width) / 2.0f;
    g_dialogue.window_y = (screen_height - g_dialogue.window_height) / 2.0f;

    float x = g_dialogue.window_x;
    float y = g_dialogue.window_y;
    float w = g_dialogue.window_width;
    float h = g_dialogue.window_height;

    renderer_draw_rect(0, 0, screen_width, screen_height, 0.0f, 0.0f, 0.0f, 0.5f);

    renderer_draw_rect(x, y, w, h, 0.15f, 0.15f, 0.15f, 0.95f);

    float border_thickness = 3.0f;
    renderer_draw_rect(x, y, w, border_thickness, 0.8f, 0.6f, 0.2f, 1.0f);
    renderer_draw_rect(x, y + h - border_thickness, w, border_thickness, 0.8f, 0.6f, 0.2f, 1.0f);
    renderer_draw_rect(x, y, border_thickness, h, 0.8f, 0.6f, 0.2f, 1.0f);
    renderer_draw_rect(x + w - border_thickness, y, border_thickness, h, 0.8f, 0.6f, 0.2f, 1.0f);

    float header_height = 50.0f;
    renderer_draw_rect(x, y, w, header_height, 0.2f, 0.2f, 0.25f, 1.0f);
    renderer_draw_text_centered(x, y, w, header_height, g_dialogue.npc_name);

    float text_margin = 20.0f;
    float text_y = y + header_height + text_margin;
    renderer_draw_text(x + text_margin, text_y, g_dialogue.displayed_text);

    float option_start_y = y + h - 160.0f;
    float option_height = 35.0f;
    float option_spacing = 5.0f;

    for (int i = 0; i < g_dialogue.displayed_option_count; i++) {
        float opt_y = option_start_y + i * (option_height + option_spacing);
        float opt_x = x + text_margin;
        float opt_w = w - (text_margin * 2);

        if (i == g_dialogue.selected_option) {
            renderer_draw_rect(opt_x, opt_y, opt_w, option_height, 0.3f, 0.5f, 0.7f, 0.8f);
        } else {
            renderer_draw_rect(opt_x, opt_y, opt_w, option_height, 0.25f, 0.25f, 0.3f, 0.7f);
        }

        renderer_draw_rect(opt_x, opt_y, opt_w, 2.0f, 0.5f, 0.5f, 0.5f, 1.0f);
        renderer_draw_rect(opt_x, opt_y + option_height - 2.0f, opt_w, 2.0f, 0.5f, 0.5f, 0.5f, 1.0f);

        char option_text[MAX_OPTION_TEXT_LENGTH + 10];
        snprintf(option_text, sizeof(option_text), "%d. %s", i + 1, g_dialogue.displayed_options[i]);
        renderer_draw_text(opt_x + 10.0f, opt_y + 10.0f, option_text);
    }

    char close_hint[] = "[ESC] Close";
    renderer_draw_text(x + w - 120.0f, y + h - 25.0f, close_hint);
}

int dialogue_handle_click(float mouse_x, float mouse_y) {
    if (!g_dialogue.is_active) {
        return -1;
    }

    float x = g_dialogue.window_x;
    float y = g_dialogue.window_y;
    float w = g_dialogue.window_width;
    float h = g_dialogue.window_height;

    if (mouse_x < x || mouse_x > x + w || mouse_y < y || mouse_y > y + h) {
        return -1;
    }

    float text_margin = 20.0f;
    float option_start_y = y + h - 160.0f;
    float option_height = 35.0f;
    float option_spacing = 5.0f;

    for (int i = 0; i < g_dialogue.displayed_option_count; i++) {
        float opt_y = option_start_y + i * (option_height + option_spacing);
        float opt_x = x + text_margin;
        float opt_w = w - (text_margin * 2);

        if (mouse_x >= opt_x && mouse_x <= opt_x + opt_w &&
            mouse_y >= opt_y && mouse_y <= opt_y + option_height) {
            printf("[DIALOGUE] Option %d (ID %u) selected\n", i, g_dialogue.option_ids[i]);
            return i;
        }
    }

    return -1;
}

uint32_t dialogue_get_current_npc(void) {
    return g_dialogue.npc_id;
}

uint32_t dialogue_get_current_dialogue_id(void) {
    return g_dialogue.dialogue_id;
}

uint8_t dialogue_get_current_page(void) {
    return g_dialogue.current_page;
}
