#ifndef SHOP_UI_H
#define SHOP_UI_H

/**
 * @file
 * Declare the buy/sell UI driven by server-populated GameState shop data.
 */

typedef struct GameState GameState;

void shop_ui_render(const GameState* game);

/** Return nonzero when shop input consumes the click. */
int shop_ui_handle_input(GameState* game, float mx, float my, int clicked);

#endif // SHOP_UI_H
