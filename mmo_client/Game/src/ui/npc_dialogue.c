/**
 * @file
 * Draw the dialogue page the server sent and report the choice the player made.
 */

#include "ui/npc_dialogue.h"
#include "renderer.h"

#include <winsock2.h>   /* ntohl for the identifiers the packets carry */

#include <stdio.h>
#include <string.h>
#include <math.h>

/** Window proportions, as a fraction of the logical viewport. */
#define DLG_WIDTH_FRACTION   0.46f
#define DLG_MIN_WIDTH        520.0f
#define DLG_MAX_WIDTH        860.0f

#define DLG_HEADER_H          46.0f
#define DLG_MARGIN            22.0f
#define DLG_LINE_H            26.0f
#define DLG_OPTION_H          34.0f
#define DLG_OPTION_GAP         6.0f
#define DLG_FOOTER_H          30.0f

static DialogueState g_dialogue;

void dialogue_ui_init(void) {
    memset(&g_dialogue, 0, sizeof(g_dialogue));
    g_dialogue.selected_option = -1;
}

/**
 * Copy one page's text, choices, and identifiers into the window.
 *
 * @param option_count  Choice count as claimed by the packet; clamped to the wire cap.
 */
static void adopt_page(uint32_t dialogue_id, uint8_t page_num, uint8_t option_count,
                       const char* text,
                       const char option_text[][MAX_OPTION_TEXT],
                       const uint8_t* option_ids) {
    if (option_count > MAX_DIALOGUE_OPTIONS) option_count = MAX_DIALOGUE_OPTIONS;

    g_dialogue.dialogue_id     = dialogue_id;
    g_dialogue.current_page    = page_num;
    g_dialogue.option_count    = option_count;
    g_dialogue.selected_option = -1;

    snprintf(g_dialogue.text, sizeof(g_dialogue.text), "%s", text);

    for (int i = 0; i < option_count; i++) {
        snprintf(g_dialogue.option_text[i], MAX_OPTION_TEXT, "%s", option_text[i]);
        g_dialogue.option_ids[i] = option_ids[i];
    }
}

void dialogue_show(const NPCInteractResponsePacket* packet) {
    if (!packet) return;

    g_dialogue.is_active = true;
    g_dialogue.npc_id    = ntohl(packet->npc_id);
    snprintf(g_dialogue.npc_name, sizeof(g_dialogue.npc_name), "%s", packet->npc_name);

    adopt_page(ntohl(packet->dialogue_id), packet->page_num, packet->option_count,
               packet->text, packet->option_text, packet->option_ids);
}

void dialogue_update_page(const DialogueUpdatePacket* packet) {
    if (!packet || !g_dialogue.is_active) return;

    adopt_page(ntohl(packet->dialogue_id), packet->page_num, packet->option_count,
               packet->text, packet->option_text, packet->option_ids);
}

void dialogue_close(void) {
    g_dialogue.is_active      = false;
    g_dialogue.selected_option = -1;
}

bool dialogue_is_active(void) {
    return g_dialogue.is_active;
}

uint32_t dialogue_get_current_npc(void)         { return g_dialogue.npc_id; }
uint32_t dialogue_get_current_dialogue_id(void) { return g_dialogue.dialogue_id; }
uint8_t  dialogue_get_current_page(void)        { return g_dialogue.current_page; }

/**
 * Size and place the window for the current page and viewport.
 *
 * The height follows the wrapped text rather than a fixed number, so a two-line
 * greeting and a fifteen-line initiation both read as deliberate.
 */
static void layout_window(int viewport_width, int viewport_height) {
    float vw = (float)viewport_width;
    float vh = (float)viewport_height;

    float w = vw * DLG_WIDTH_FRACTION;
    if (w < DLG_MIN_WIDTH) w = DLG_MIN_WIDTH;
    if (w > DLG_MAX_WIDTH) w = DLG_MAX_WIDTH;
    if (w > vw - 40.0f)    w = vw - 40.0f;

    int text_lines = renderer_measure_text_wrapped(w - DLG_MARGIN * 2.0f, g_dialogue.text);
    if (text_lines < 1) text_lines = 1;

    float h = DLG_HEADER_H + DLG_MARGIN
            + text_lines * DLG_LINE_H + DLG_MARGIN
            + g_dialogue.option_count * (DLG_OPTION_H + DLG_OPTION_GAP)
            + DLG_FOOTER_H;

    if (h > vh - 40.0f) h = vh - 40.0f;

    g_dialogue.window_width  = w;
    g_dialogue.window_height = h;
    g_dialogue.window_x      = floorf((vw - w) * 0.5f);
    g_dialogue.window_y      = floorf((vh - h) * 0.5f);
}

/** Report where the first choice row starts, given the current layout. */
static float options_top(void) {
    return g_dialogue.window_y + g_dialogue.window_height
           - DLG_FOOTER_H
           - g_dialogue.option_count * (DLG_OPTION_H + DLG_OPTION_GAP);
}

/**
 * Find the choice row under a point.
 *
 * @return The row index, or -1 when the point is over no row.
 */
static int option_row_at(float mouse_x, float mouse_y) {
    if (!g_dialogue.is_active) return -1;

    float row_x = g_dialogue.window_x + DLG_MARGIN;
    float row_w = g_dialogue.window_width - DLG_MARGIN * 2.0f;
    if (mouse_x < row_x || mouse_x > row_x + row_w) return -1;

    float top = options_top();
    for (int i = 0; i < g_dialogue.option_count; i++) {
        float y = top + i * (DLG_OPTION_H + DLG_OPTION_GAP);
        if (mouse_y >= y && mouse_y <= y + DLG_OPTION_H) return i;
    }
    return -1;
}

void dialogue_handle_hover(float mouse_x, float mouse_y) {
    g_dialogue.selected_option = option_row_at(mouse_x, mouse_y);
}

int dialogue_handle_click(float mouse_x, float mouse_y, uint8_t* out_option_id) {
    int row = option_row_at(mouse_x, mouse_y);
    if (row < 0) return 0;

    if (out_option_id) *out_option_id = g_dialogue.option_ids[row];
    return 1;
}

void dialogue_render(int viewport_width, int viewport_height) {
    if (!g_dialogue.is_active) return;

    layout_window(viewport_width, viewport_height);

    float x = g_dialogue.window_x;
    float y = g_dialogue.window_y;
    float w = g_dialogue.window_width;
    float h = g_dialogue.window_height;

    renderer_draw_rect(0, 0, (float)viewport_width, (float)viewport_height,
                       0.0f, 0.0f, 0.0f, 0.5f);

    renderer_draw_rect(x, y, w, h, 0.09f, 0.09f, 0.13f, 0.97f);

    float border = 3.0f;
    renderer_draw_rect(x,           y,           w,      border, 0.80f, 0.62f, 0.20f, 1.0f);
    renderer_draw_rect(x,           y + h - border, w,   border, 0.80f, 0.62f, 0.20f, 1.0f);
    renderer_draw_rect(x,           y,           border, h,      0.80f, 0.62f, 0.20f, 1.0f);
    renderer_draw_rect(x + w - border, y,        border, h,      0.80f, 0.62f, 0.20f, 1.0f);

    renderer_draw_rect(x, y, w, DLG_HEADER_H, 0.18f, 0.18f, 0.23f, 1.0f);
    renderer_draw_text_centered(x, y, w, DLG_HEADER_H, g_dialogue.npc_name);

    renderer_draw_text_wrapped(x + DLG_MARGIN,
                               y + DLG_HEADER_H + DLG_MARGIN + DLG_LINE_H * 0.5f,
                               w - DLG_MARGIN * 2.0f, DLG_LINE_H,
                               g_dialogue.text);

    float row_x = x + DLG_MARGIN;
    float row_w = w - DLG_MARGIN * 2.0f;
    float top   = options_top();

    for (int i = 0; i < g_dialogue.option_count; i++) {
        float row_y = top + i * (DLG_OPTION_H + DLG_OPTION_GAP);
        int   hot   = (i == g_dialogue.selected_option);

        renderer_draw_rect(row_x, row_y, row_w, DLG_OPTION_H,
                           hot ? 0.28f : 0.20f, hot ? 0.42f : 0.20f,
                           hot ? 0.60f : 0.26f, 0.85f);
        renderer_draw_rect(row_x, row_y, row_w, 1.5f, 0.45f, 0.45f, 0.52f, 1.0f);

        char row[MAX_OPTION_TEXT + 8];
        snprintf(row, sizeof(row), "%d. %s", i + 1, g_dialogue.option_text[i]);
        renderer_draw_text(row_x + 12.0f, row_y + DLG_OPTION_H - 11.0f, row);
    }

    renderer_draw_text(x + w - 118.0f, y + h - 10.0f, "[ESC] Close");
}
