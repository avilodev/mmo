#ifndef CURRENCY_PANEL_H
#define CURRENCY_PANEL_H

/**
 * @file
 * Declare the currency panel: the player's balance in every kingdom's coin.
 *
 * There is no universal coin. Each kingdom mints its own, so a player carries
 * one balance per kingdom and this panel is where they read all of them at
 * once. It lists every currency in the shared city table, so a new kingdom
 * appears here with no change to this file.
 */

#include "ui/window_pos.h"

#include <stdint.h>

/** Track currency-panel visibility. */
typedef struct {
    int is_open;
    window_pos win;   /**< Where the panel sits; centred until dragged. */
} CurrencyPanelState;

void currency_panel_init(CurrencyPanelState* cp);

/**
 * Draw the panel when open.
 *
 * @param balances  One balance per CurrencyId, or NULL before character data
 *                  has arrived, which draws the panel with every balance zero.
 */
void currency_panel_render(const CurrencyPanelState* cp,
                           const uint32_t* balances,
                           int local_currency,
                           int vw, int vh);

/**
 * Handle one frame of panel input.
 *
 * @param key_toggle  Nonzero on the frame the bound key is pressed.
 * @param key_esc     Nonzero on the frame Escape is pressed.
 * @return Nonzero when the panel consumed the click, so gameplay ignores it.
 */
int currency_panel_handle_input(CurrencyPanelState* cp,
                                float mx, float my, int clicked, int down,
                                int key_toggle, int key_esc,
                                int vw, int vh);

#endif // CURRENCY_PANEL_H
