#ifndef NPC_DIALOGUE_H
#define NPC_DIALOGUE_H

#include <stdint.h>
#include <stdbool.h>

#define MAX_DIALOGUE_OPTIONS 6
#define MAX_PAGES_PER_DIALOGUE 32
#define MAX_DIALOGUES 256
#define MAX_DIALOGUE_TEXT_LENGTH 1024
#define MAX_OPTION_TEXT_LENGTH 256
#define MAX_NPC_NAME_LENGTH 64

/** Define a selectable response loaded from a dialogue JSON file. */
typedef struct {
    uint8_t option_id;
    char text[MAX_OPTION_TEXT_LENGTH];
    int8_t next_page;  /**< Next page index, or -1 to close the dialogue. */
} DialogueOptionDef;

/** Define one page of locally stored dialogue text and responses. */
typedef struct {
    uint8_t page_num;
    char text[MAX_DIALOGUE_TEXT_LENGTH];
    uint8_t option_count;
    DialogueOptionDef options[MAX_DIALOGUE_OPTIONS];
} DialoguePageDef;

/** Define a complete per-NPC dialogue loaded from JSON. */
typedef struct {
    uint32_t dialogue_id;
    char name[64];
    uint8_t page_count;
    DialoguePageDef pages[MAX_PAGES_PER_DIALOGUE];
} DialogueDef;

/** Track the dialogue page and window currently presented to the player. */
typedef struct {
    bool is_active;
    uint32_t npc_id;
    uint32_t dialogue_id;
    uint8_t current_page;
    char npc_name[MAX_NPC_NAME_LENGTH];

    /** Text resolved from the active dialogue and page identifiers. */
    char displayed_text[MAX_DIALOGUE_TEXT_LENGTH];
    uint8_t displayed_option_count;
    char displayed_options[MAX_DIALOGUE_OPTIONS][MAX_OPTION_TEXT_LENGTH];
    uint8_t option_ids[MAX_DIALOGUE_OPTIONS];

    int selected_option;  /**< Highlighted option index, or -1 when none is selected. */

    /** Screen-space dialogue window bounds. */
    float window_x;
    float window_y;
    float window_width;
    float window_height;
} DialogueState;

// Initialize dialogue system and load dialogues.json
int dialogue_system_init(const char* json_path);

// Clean up dialogue system
void dialogue_system_cleanup(void);

// Get dialogue definition by ID
const DialogueDef* dialogue_get(uint32_t dialogue_id);

// Show dialogue window with server data (looks up text locally)
void dialogue_show(uint32_t npc_id, const char* npc_name, uint32_t dialogue_id,
                  uint8_t page_num, uint8_t option_count, const uint8_t* option_ids);

// Update dialogue to new page (from server update packet)
void dialogue_update_page(uint32_t dialogue_id, uint8_t page_num,
                         uint8_t option_count, const uint8_t* option_ids);

// Close the dialogue window
void dialogue_close(void);

// Check if dialogue is currently active
bool dialogue_is_active(void);

// Update dialogue state (handle input, etc.)
void dialogue_update_state(float delta_time);

// Render the dialogue window
void dialogue_render(void);

// Handle mouse click - returns selected option ID or -1 if no option clicked
int dialogue_handle_click(float mouse_x, float mouse_y);

// Get the current dialogue state
uint32_t dialogue_get_current_npc(void);
uint32_t dialogue_get_current_dialogue_id(void);
uint8_t dialogue_get_current_page(void);

#endif // NPC_DIALOGUE_H
