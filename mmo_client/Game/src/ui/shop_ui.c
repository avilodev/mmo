// ============================================================================
// shop_ui.c — Buy/Sell shop window
// ============================================================================

#include "ui/shop_ui.h"
#include "core/game_types.h"
#include "renderer.h"
#include "player/inventory.h"
#include "network/network.h"

#include <stdio.h>
#include <string.h>

// ============================================================================
// Layout constants
// ============================================================================

#define SW_W        400.0f
#define SW_H        480.0f
#define SW_PAD       12.0f
#define SW_ROW_H     38.0f
#define SW_TAB_H     34.0f
#define SW_HEADER_H  44.0f
#define SW_BTN_W     60.0f
#define SW_BTN_H     24.0f

// ============================================================================
// Helper: draw the shop window chrome (background + title + tabs)
// Returns the Y position where the item list starts.
// ============================================================================

static float shop_chrome(const GameState* game, int vw, int vh,
                          float* out_px, float* out_py) {
    float px = ((float)vw - SW_W) * 0.5f;
    float py = ((float)vh - SW_H) * 0.5f;
    *out_px = px;
    *out_py = py;

    // Background
    renderer_draw_rect(px, py, SW_W, SW_H, 0.09f, 0.07f, 0.12f, 0.97f);

    // Border
    renderer_draw_rect(px,          py,          SW_W, 2.0f, 0.55f, 0.40f, 0.70f, 1.0f);
    renderer_draw_rect(px,          py+SW_H-2,   SW_W, 2.0f, 0.55f, 0.40f, 0.70f, 1.0f);
    renderer_draw_rect(px,          py,          2.0f, SW_H, 0.55f, 0.40f, 0.70f, 1.0f);
    renderer_draw_rect(px+SW_W-2,   py,          2.0f, SW_H, 0.55f, 0.40f, 0.70f, 1.0f);

    // Title bar
    renderer_draw_rect(px, py, SW_W, SW_HEADER_H, 0.15f, 0.10f, 0.20f, 1.0f);

    char title[64];
    if (game->shop.shop_name[0])
        snprintf(title, sizeof(title), "%s", game->shop.shop_name);
    else
        snprintf(title, sizeof(title), "Shop");
    renderer_draw_text(px + SW_W * 0.5f - 30.0f, py + SW_HEADER_H - 12.0f, title);

    // Close button [X]
    renderer_draw_rect(px + SW_W - 30.0f, py + 10.0f, 22.0f, 22.0f,
                       0.60f, 0.10f, 0.10f, 0.9f);
    renderer_draw_text(px + SW_W - 24.0f, py + 26.0f, "X");

    // Tabs
    float tab_y = py + SW_HEADER_H;
    float tab_w = SW_W * 0.5f;

    // Buy tab
    float buy_bg = (game->shop.sell_tab == 0) ? 0.20f : 0.10f;
    renderer_draw_rect(px,         tab_y, tab_w, SW_TAB_H, buy_bg, buy_bg, buy_bg+0.08f, 1.0f);
    renderer_draw_text(px + tab_w * 0.5f - 14.0f, tab_y + SW_TAB_H - 10.0f, "BUY");

    // Sell tab
    float sell_bg = (game->shop.sell_tab == 1) ? 0.20f : 0.10f;
    renderer_draw_rect(px+tab_w,   tab_y, tab_w, SW_TAB_H, sell_bg, sell_bg, sell_bg+0.08f, 1.0f);
    renderer_draw_text(px + tab_w + tab_w * 0.5f - 14.0f, tab_y + SW_TAB_H - 10.0f, "SELL");

    // Separator under tabs
    renderer_draw_rect(px, tab_y + SW_TAB_H, SW_W, 1.5f, 0.40f, 0.30f, 0.55f, 0.8f);

    return py + SW_HEADER_H + SW_TAB_H + 2.0f;
}

// ============================================================================
// Render
// ============================================================================

void shop_ui_render(const GameState* game) {
    if (!game->shop.is_open) return;

    int vw, vh;
    // Retrieve viewport size from HUD layout (already computed each frame)
    vw = game->hud.screen_width;
    vh = game->hud.screen_height;
    if (vw <= 0 || vh <= 0) return;

    float px, py;
    float list_y = shop_chrome(game, vw, vh, &px, &py);
    float list_h = SW_H - (list_y - py) - SW_PAD;
    float max_y  = py + SW_H - SW_PAD;

    if (game->shop.sell_tab == 0) {
        // ---- BUY TAB ----
        if (game->shop.item_count == 0) {
            renderer_draw_text(px + SW_PAD, list_y + 20.0f, "No items for sale.");
            return;
        }

        for (int i = 0; i < game->shop.item_count; i++) {
            float row_y = list_y + i * SW_ROW_H;
            if (row_y + SW_ROW_H > max_y) break;

            uint32_t item_id   = game->shop.items[i].item_id;
            uint32_t buy_price = game->shop.items[i].buy_price;
            const char* name   = item_db_get_name(item_id);

            // Row background (alternating)
            float bg = (i % 2 == 0) ? 0.12f : 0.09f;
            renderer_draw_rect(px, row_y, SW_W, SW_ROW_H, bg, bg, bg, 0.8f);

            // Item name
            renderer_draw_text(px + SW_PAD, row_y + SW_ROW_H - 12.0f, name);

            // Price
            char price_str[32];
            snprintf(price_str, sizeof(price_str), "%ug", buy_price);
            renderer_draw_text(px + SW_W - 100.0f, row_y + SW_ROW_H - 12.0f, price_str);

            // Buy button
            renderer_draw_rect(px + SW_W - SW_BTN_W - SW_PAD,
                               row_y + (SW_ROW_H - SW_BTN_H) * 0.5f,
                               SW_BTN_W, SW_BTN_H,
                               0.20f, 0.55f, 0.20f, 0.9f);
            renderer_draw_text(px + SW_W - SW_BTN_W - SW_PAD + 12.0f,
                               row_y + (SW_ROW_H - SW_BTN_H) * 0.5f + SW_BTN_H - 8.0f,
                               "Buy");
        }
    } else {
        // ---- SELL TAB ----
        const InventoryState* inv = game->inventory;
        if (!inv) {
            renderer_draw_text(px + SW_PAD, list_y + 20.0f, "Inventory unavailable.");
            return;
        }

        int shown = 0;
        for (int s = 0; s < INVENTORY_SIZE; s++) {
            uint32_t item_id = inv->slots[s].template_id;
            if (item_id == 0) continue;

            float row_y = list_y + shown * SW_ROW_H;
            if (row_y + SW_ROW_H > max_y) break;

            const ItemTemplate* tmpl = item_db_get(item_id);
            const char* name  = tmpl ? tmpl->name : "Unknown";
            uint32_t sell_val = tmpl ? tmpl->vendor_price : 0;

            float bg = (shown % 2 == 0) ? 0.12f : 0.09f;
            renderer_draw_rect(px, row_y, SW_W, SW_ROW_H, bg, bg, bg, 0.8f);

            renderer_draw_text(px + SW_PAD, row_y + SW_ROW_H - 12.0f, name);

            char price_str[32];
            snprintf(price_str, sizeof(price_str), "%ug", sell_val);
            renderer_draw_text(px + SW_W - 100.0f, row_y + SW_ROW_H - 12.0f, price_str);

            // Sell button
            renderer_draw_rect(px + SW_W - SW_BTN_W - SW_PAD,
                               row_y + (SW_ROW_H - SW_BTN_H) * 0.5f,
                               SW_BTN_W, SW_BTN_H,
                               0.55f, 0.20f, 0.20f, 0.9f);
            renderer_draw_text(px + SW_W - SW_BTN_W - SW_PAD + 10.0f,
                               row_y + (SW_ROW_H - SW_BTN_H) * 0.5f + SW_BTN_H - 8.0f,
                               "Sell");

            shown++;
        }

        if (shown == 0) {
            renderer_draw_text(px + SW_PAD, list_y + 20.0f, "Nothing to sell.");
        }
    }
}

// ============================================================================
// Input
// ============================================================================

int shop_ui_handle_input(GameState* game, float mx, float my, int clicked) {
    if (!game->shop.is_open) return 0;

    int vw = game->hud.screen_width;
    int vh = game->hud.screen_height;
    float px = ((float)vw - SW_W) * 0.5f;
    float py = ((float)vh - SW_H) * 0.5f;

    // Always consume clicks inside the window
    int inside = (mx >= px && mx <= px + SW_W && my >= py && my <= py + SW_H);
    if (!inside) {
        if (clicked) { game->shop.is_open = 0; }
        return clicked && inside ? 1 : 0;
    }

    if (!clicked) return 1; // hover: consume but don't act

    // Close button
    if (mx >= px + SW_W - 30.0f && mx <= px + SW_W - 8.0f &&
        my >= py + 10.0f && my <= py + 32.0f) {
        game->shop.is_open = 0;
        return 1;
    }

    // Tab clicks
    float tab_y  = py + SW_HEADER_H;
    float tab_w  = SW_W * 0.5f;
    if (my >= tab_y && my <= tab_y + SW_TAB_H) {
        if (mx < px + tab_w)
            game->shop.sell_tab = 0;
        else
            game->shop.sell_tab = 1;
        return 1;
    }

    float list_y = py + SW_HEADER_H + SW_TAB_H + 2.0f;
    float max_y  = py + SW_H - SW_PAD;

    if (game->shop.sell_tab == 0) {
        // Buy tab: check each Buy button
        for (int i = 0; i < game->shop.item_count; i++) {
            float row_y  = list_y + i * SW_ROW_H;
            if (row_y + SW_ROW_H > max_y) break;

            float btn_x = px + SW_W - SW_BTN_W - SW_PAD;
            float btn_y = row_y + (SW_ROW_H - SW_BTN_H) * 0.5f;

            if (mx >= btn_x && mx <= btn_x + SW_BTN_W &&
                my >= btn_y && my <= btn_y + SW_BTN_H) {
                // Send buy request
                network_send_shop_buy(game->shop.shop_id, game->shop.items[i].item_id);
                return 1;
            }
        }
    } else {
        // Sell tab: check each Sell button
        const InventoryState* inv = game->inventory;
        if (!inv) return 1;

        int shown = 0;
        for (int s = 0; s < INVENTORY_SIZE; s++) {
            if (inv->slots[s].template_id == 0) continue;

            float row_y  = list_y + shown * SW_ROW_H;
            if (row_y + SW_ROW_H > max_y) break;

            float btn_x = px + SW_W - SW_BTN_W - SW_PAD;
            float btn_y = row_y + (SW_ROW_H - SW_BTN_H) * 0.5f;

            if (mx >= btn_x && mx <= btn_x + SW_BTN_W &&
                my >= btn_y && my <= btn_y + SW_BTN_H) {
                network_send_shop_sell(game->shop.shop_id, (uint8_t)s);
                return 1;
            }
            shown++;
        }
    }

    return 1;
}
