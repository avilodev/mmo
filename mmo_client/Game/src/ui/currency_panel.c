/**
 * @file
 * Render the player's balance in every kingdom's currency.
 *
 * The panel sizes itself from the shared city table rather than from a fixed
 * list, so adding a kingdom adds a row here with no change to this file.
 */

#include "ui/currency_panel.h"
#include "world_regions.h"
#include "renderer.h"

#include <stdio.h>
#include <string.h>

#define CP_PW       420.0f   // Panel width
#define CP_TITLE_H   44.0f   // Title bar height
#define CP_ROW_H     40.0f   // Height per currency row
#define CP_PAD       14.0f   // Inner padding
#define CP_FOOT_H    30.0f   // Footer note height

/** Report the panel's height for the current currency count. */
static float panel_height(void) {
    return CP_TITLE_H + CP_PAD + (float)CURRENCY_COUNT * CP_ROW_H + CP_FOOT_H;
}

/** Report the panel's top-left corner, centred in the viewport. */
static void panel_origin(int vw, int vh, float* out_x, float* out_y) {
    *out_x = ((float)vw - CP_PW)          * 0.5f;
    *out_y = ((float)vh - panel_height()) * 0.5f;
}

/** Draw a one-pixel border around a rectangle. */
static void draw_border(float x, float y, float w, float h,
                        float r, float g, float b, float a) {
    renderer_draw_rect(x,            y,            w,    1.5f, r, g, b, a);
    renderer_draw_rect(x,            y + h - 1.5f, w,    1.5f, r, g, b, a);
    renderer_draw_rect(x,            y,            1.5f, h,    r, g, b, a);
    renderer_draw_rect(x + w - 1.5f, y,            1.5f, h,    r, g, b, a);
}

/**
 * Initialize a closed currency panel.
 */
void currency_panel_init(CurrencyPanelState* cp) {
    if (!cp) return;
    memset(cp, 0, sizeof(*cp));
}

/**
 * Show a hidden panel, or hide a shown one.
 */
void currency_panel_toggle(CurrencyPanelState* cp) {
    if (!cp) return;
    cp->is_open = !cp->is_open;
}

/**
 * Draw the panel when open.
 *
 * Every currency gets a row whether or not the player holds any of it, so the
 * kingdoms a player has yet to earn from are visible rather than hidden.
 */
void currency_panel_render(const CurrencyPanelState* cp,
                           const uint32_t* balances,
                           int local_currency,
                           int vw, int vh) {
    if (!cp || !cp->is_open) return;

    float ph = panel_height();
    float px, py;
    panel_origin(vw, vh, &px, &py);

    // Panel background and frame
    renderer_draw_rect(px, py, CP_PW, ph, 0.08f, 0.08f, 0.14f, 0.97f);
    draw_border(px, py, CP_PW, ph, 0.35f, 0.50f, 0.70f, 0.8f);

    // Title bar
    renderer_draw_rect(px, py, CP_PW, CP_TITLE_H, 0.12f, 0.18f, 0.28f, 1.0f);
    renderer_draw_rect(px, py + CP_TITLE_H, CP_PW, 1.5f, 0.35f, 0.50f, 0.70f, 0.8f);
    renderer_draw_text(px + CP_PW * 0.5f - 44.0f, py + 28.0f, "CURRENCY");

    // Close affordance, matching the quest log's key hint
    renderer_draw_text(px + CP_PW - 52.0f, py + 28.0f, "[G]");

    float row_y = py + CP_TITLE_H + CP_PAD;

    for (int c = 0; c < CURRENCY_COUNT; c++) {
        uint32_t amount = balances ? balances[c] : 0u;
        int is_local = (c == local_currency);

        // The kingdom the player is standing in reads brighter than the rest.
        if (is_local)
            renderer_draw_rect(px + 6.0f, row_y, CP_PW - 12.0f, CP_ROW_H - 4.0f,
                               0.14f, 0.16f, 0.22f, 0.75f);

        // Coin swatch
        renderer_draw_rect(px + CP_PAD, row_y + 12.0f, 14.0f, 14.0f,
                           1.0f, 0.80f, 0.05f, is_local ? 1.0f : 0.55f);

        renderer_draw_text(px + CP_PAD + 26.0f, row_y + CP_ROW_H - 14.0f,
                           world_currency_name(c));

        // Balance, right-aligned by eye against the panel edge
        char amount_text[32];
        snprintf(amount_text, sizeof(amount_text), "%u", amount);
        float digits = (float)strlen(amount_text);
        renderer_draw_text(px + CP_PW - CP_PAD - digits * 9.0f,
                           row_y + CP_ROW_H - 14.0f, amount_text);

        row_y += CP_ROW_H;
    }

    // Footer names where the player is, which is the coin local trade pays in.
    char footer[96];
    snprintf(footer, sizeof(footer), "Local coin: %s",
             world_currency_city_name(local_currency));
    renderer_draw_text(px + CP_PAD, py + ph - 10.0f, footer);
}

/**
 * Handle one frame of panel input.
 *
 * A click outside the panel closes it, matching the quest log.
 *
 * @return Nonzero when the panel consumed the click.
 */
int currency_panel_handle_input(CurrencyPanelState* cp,
                                float mx, float my, int clicked,
                                int key_toggle, int key_esc,
                                int vw, int vh) {
    if (!cp) return 0;

    if (key_toggle) { cp->is_open = !cp->is_open; return 1; }
    if (!cp->is_open) return 0;
    if (key_esc)    { cp->is_open = 0; return 1; }
    if (!clicked)   return 0;

    float ph = panel_height();
    float px, py;
    panel_origin(vw, vh, &px, &py);

    if (mx < px || mx > px + CP_PW || my < py || my > py + ph) {
        cp->is_open = 0;
        return 1;
    }

    // The panel is read-only, so a click inside it is swallowed and ignored.
    return 1;
}
