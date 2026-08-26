#ifndef NPC_DIALOGUE_H
#define NPC_DIALOGUE_H

/**
 * @file
 * Present the dialogue page the server sent.
 *
 * The client holds no dialogue data of its own. It used to: a copy of every
 * conversation shipped in Game/data/dialogues, indexed by page number, and the
 * server sent only the numbers. That made the text a second source of truth
 * that could drift from the server's, and it meant an option the server had
 * hidden could still be rendered from the local copy. Both packets now carry
 * the page's text and the text of exactly the choices this player may take.
 */

#include "protocol.h"

#include <stdint.h>
#include <stdbool.h>

/** Track the dialogue page currently on screen. */
typedef struct {
    bool     is_active;
    uint32_t npc_id;
    uint32_t dialogue_id;
    uint8_t  current_page;
    char     npc_name[32];

    char    text[MAX_DIALOGUE_TEXT];
    uint8_t option_count;
    char    option_text[MAX_DIALOGUE_OPTIONS][MAX_OPTION_TEXT];
    /** Identify each choice to the server. Never send a row number instead:
     *  the server hides options this player fails, so rows are per player. */
    uint8_t option_ids[MAX_DIALOGUE_OPTIONS];

    int selected_option;  /**< Hovered row, or -1. */

    /** Screen-space window bounds, recomputed each frame from the viewport. */
    float window_x;
    float window_y;
    float window_width;
    float window_height;
} DialogueState;

/** Reset the dialogue window. Call once at gameplay start. */
void dialogue_ui_init(void);

/** Show a page opened by PACKET_NPC_INTERACT_RESPONSE. */
void dialogue_show(const NPCInteractResponsePacket* packet);

/** Replace the page from PACKET_DIALOGUE_UPDATE. */
void dialogue_update_page(const DialogueUpdatePacket* packet);

/** Close the dialogue window. */
void dialogue_close(void);

/** Report whether a dialogue window is open. */
bool dialogue_is_active(void);

/** Draw the dialogue window inside a logical viewport. */
void dialogue_render(int viewport_width, int viewport_height);

/** Resolve a click against the visible choices.
 *
 * @param out_option_id  Receives the identifier to send back to the server.
 * @return               Nonzero when a choice was hit.
 */
int dialogue_handle_click(float mouse_x, float mouse_y, uint8_t* out_option_id);

/** Track which choice the pointer is over, for highlighting. */
void dialogue_handle_hover(float mouse_x, float mouse_y);

uint32_t dialogue_get_current_npc(void);
uint32_t dialogue_get_current_dialogue_id(void);
uint8_t  dialogue_get_current_page(void);

#endif // NPC_DIALOGUE_H
