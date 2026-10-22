/**
 * @file
 * Render the player's balance in every kingdom's currency.
 *
 * The panel sizes itself from the shared city table rather than from a fixed
 * list, so adding a kingdom adds a row here with no change to this file.
 */

#include "ui/currency_panel.h"
#include "ui/ui_kit.h"
#include "world_regions.h"
#include "renderer.h"
#include "texture/texture.h"

#include <stdio.h>
#include <string.h>

#define CP_PW        230.0f   // Panel width
#define CP_TITLE_H    44.0f   // Title bar height
#define CP_ROW_H      32.0f   // Height per currency row
#define CP_PAD        16.0f   // Inner padding
#define CP_ICON       22.0f   // Side of a coin picture
#define CP_CLOSE      24.0f   // Side of the close button
#define CP_TEXT       17.0f   // Size of the numbers
#define CP_TITLE      20.0f   // Size of the title

// The coin picture of each currency; a missing file draws a placeholder coin
static const char* const COIN_FILES[CURRENCY_COUNT] = {
	"Game/Sprites/UI/Hud/currency_ennara.png",
	"Game/Sprites/UI/Hud/currency_k1.png",
	"Game/Sprites/UI/Hud/currency_k2.png",
	"Game/Sprites/UI/Hud/currency_k3.png"
};

// The colour of each currency's placeholder coin
static const float COIN_COLORS[CURRENCY_COUNT][3] = {
	{ 1.00f, 0.80f, 0.20f },
	{ 0.85f, 0.85f, 0.90f },
	{ 0.55f, 0.80f, 1.00f },
	{ 0.95f, 0.55f, 0.30f }
};

static unsigned int g_coin_texture[CURRENCY_COUNT];
static int g_coins_loaded;

/** Report the panel's height for the current currency count. */
static float panel_height(void) {
	return CP_TITLE_H + (float)CURRENCY_COUNT * CP_ROW_H + CP_PAD;
}

/** Report the panel's top-left corner, centred in the viewport. */
static void panel_origin(const CurrencyPanelState* cp, int vw, int vh, float* out_x,
		float* out_y) {
	window_pos_origin(&cp->win, (float)vw, (float)vh, CP_PW, panel_height(), out_x, out_y);
}

/** Report where the close button is. */
static void close_origin(float px, float py, float* out_x, float* out_y) {
	*out_x = px + CP_PW - CP_CLOSE - 10.0f;
	*out_y = py + 10.0f;
}

/** Load the coin pictures once; a missing one stays 0. */
static void load_coins(void) {
	if(g_coins_loaded)
		return;

	g_coins_loaded = 1;
	for(int c = 0; c < CURRENCY_COUNT; c++)
		g_coin_texture[c] = texture_load_icon(COIN_FILES[c]);
}

/** Draw one coin, its picture or a placeholder disc. */
static void draw_coin(int currency, float x, float y, float lit) {
	if(g_coin_texture[currency]) {
		renderer_draw_sprite(x, y, CP_ICON, CP_ICON, g_coin_texture[currency]);
		return;
	}

	renderer_draw_circle(x + CP_ICON * 0.5f, y + CP_ICON * 0.5f, CP_ICON * 0.5f,
	                     0.10f, 0.08f, 0.04f, 1.0f, 20);
	renderer_draw_circle(x + CP_ICON * 0.5f, y + CP_ICON * 0.5f, CP_ICON * 0.5f - 2.0f,
	                     COIN_COLORS[currency][0] * lit, COIN_COLORS[currency][1] * lit,
	                     COIN_COLORS[currency][2] * lit, 1.0f, 20);
}

/**
 * Initialize a closed currency panel.
 */
void currency_panel_init(CurrencyPanelState* cp) {
	if(!cp)
		return;

	memset(cp, 0, sizeof(*cp));
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
	float ph = panel_height();
	float px;
	float py;
	float close_x;
	float close_y;
	float row_y;

	if(!cp || !cp->is_open)
		return;

	load_coins();
	panel_origin(cp, vw, vh, &px, &py);
	close_origin(px, py, &close_x, &close_y);
	row_y = py + CP_TITLE_H;

	ui_kit_px_begin(vw, vh);
	ui_kit_px_rect(px, py, CP_PW, ph, 0.05f, 0.05f, 0.09f, 0.97f);
	ui_kit_px_panel(px, py, CP_PW, ph);
	ui_kit_px_text(px + CP_PAD, py + CP_TITLE_H * 0.5f, "Currency", CP_TITLE, KIT_GOLD, KIT_LEFT);
	ui_kit_px_close_button(close_x, close_y, CP_CLOSE, 0);
	ui_kit_px_end();

	for(int c = 0; c < CURRENCY_COUNT; c++) {
		uint32_t amount = balances ? balances[c] : 0u;
		int is_local = (c == local_currency);
		char amount_text[32];

		// The kingdom the player is standing in reads brighter than the rest
		ui_kit_px_begin(vw, vh);
		if(is_local)
			ui_kit_px_rect(px + 8.0f, row_y + 2.0f, CP_PW - 16.0f, CP_ROW_H - 4.0f, 1.0f, 1.0f,
			               1.0f, 0.08f);
		snprintf(amount_text, sizeof(amount_text), "%u", amount);
		ui_kit_px_text(px + CP_PAD + CP_ICON + 10.0f, row_y + CP_ROW_H * 0.5f, amount_text,
		               CP_TEXT, is_local ? KIT_WHITE : KIT_DIM, KIT_LEFT);
		ui_kit_px_end();

		draw_coin(c, px + CP_PAD, row_y + (CP_ROW_H - CP_ICON) * 0.5f, is_local ? 1.0f : 0.7f);

		row_y += CP_ROW_H;
	}
}

/**
 * Handle one frame of panel input.
 *
 * A click outside the panel closes it, matching the quest log. The title strip
 * drags the panel about.
 *
 * @return Nonzero when the panel consumed the click.
 */
int currency_panel_handle_input(CurrencyPanelState* cp,
                                float mx, float my, int clicked, int down,
                                int key_toggle, int key_esc,
                                int vw, int vh) {
	float ph = panel_height();
	float px;
	float py;
	float close_x;
	float close_y;

	if(!cp)
		return 0;

	if(key_toggle) {
		cp->is_open = !cp->is_open;
		return 1;
	}

	if(!cp->is_open)
		return 0;

	if(key_esc) {
		cp->is_open = 0;
		return 1;
	}

	// The close button is left out of the strip that carries the panel
	if(window_pos_drag(&cp->win, (float)vw, (float)vh, CP_PW, ph, CP_PW - CP_CLOSE - 20.0f,
	                   CP_TITLE_H, mx, my, clicked, down))
		return 1;

	if(!clicked)
		return 0;

	panel_origin(cp, vw, vh, &px, &py);
	close_origin(px, py, &close_x, &close_y);
	if(mx >= close_x && mx <= close_x + CP_CLOSE && my >= close_y && my <= close_y + CP_CLOSE) {
		cp->is_open = 0;
		return 1;
	}

	if(mx < px || mx > px + CP_PW || my < py || my > py + ph)
		cp->is_open = 0;

	// The panel is read-only, so a click inside it is swallowed and ignored
	return 1;
}
