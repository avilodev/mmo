#ifndef SHOP_UI_H
#define SHOP_UI_H

// ============================================================================
// SHOP UI
// Buy/Sell window opened when the server sends PACKET_SHOP_OPEN.
// The caller (network.c) populates game->shop, then shop_ui_render /
// shop_ui_handle_input are called each frame by state_playing.c.
// ============================================================================

// Forward declaration — full type in game_types.h (included by caller)
typedef struct GameState GameState;

// Render the shop window (no-op if game->shop.is_open == 0)
void shop_ui_render(const GameState* game);

// Handle mouse input; sends buy/sell packets as needed.
// Returns 1 if the shop consumed the click (caller should skip other input).
int shop_ui_handle_input(GameState* game, float mx, float my, int clicked);

#endif // SHOP_UI_H
