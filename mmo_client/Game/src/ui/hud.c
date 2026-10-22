/**
 * @file
 * Render the in-game heads-up display and handle its panel controls.
 */
#include "hud.h"
#include "ui/map_minimap.h"
#include "core/race_registry.h"
#include "core/session_status.h"
#include "world_regions.h"
#include "renderer.h"
#include "core/game_types.h"
#include "world/npc.h"
#include "camera/camera_tuning.h"
#include "network/network.h"
#include "network/net_ping.h"
#include "network/net_reconnect.h"
#include "world/world_clock.h"
#include "world/weather_state.h"
#include "ui/weather_icon.h"
#include "ui/ui_kit.h"
#include "ui/window_pos.h"
#include <stdio.h>
#include <math.h>
#include <string.h>
#include "core/client_log.h"

/**
 * Initialize HUD element positions for the current viewport.
 */
void hud_init(HUDLayout* hud, int screen_width, int screen_height) {
    CLOG_INFO("[HUD] Initializing %dx%d", screen_width, screen_height);
    hud->screen_width = screen_width;
    hud->screen_height = screen_height;

    // Minimap (upper right corner)
    hud->minimap_size = 200.0f;
    hud->minimap_x = screen_width - hud->minimap_size - 20.0f;
    hud->minimap_y = 20.0f;

    // Health bar (bottom left)
    hud->health_bar_width = 300.0f;
    hud->health_bar_height = 25.0f;
    hud->health_bar_x = 120.0f;
    hud->health_bar_y = screen_height - 105.0f;

    // Mana bar (below health bar)
    hud->mana_bar_width = 300.0f;
    hud->mana_bar_height = 20.0f;
    hud->mana_bar_x = 120.0f;
    hud->mana_bar_y = screen_height - 75.0f;

    // Experience bar (bottom, below mana)
    hud->exp_bar_width = 500.0f;
    hud->exp_bar_height = 20.0f;
    hud->exp_bar_x = 120.0f;
    hud->exp_bar_y = screen_height - 50.0f;

    // Level display (left of exp bar)
    hud->level_x = 20.0f;
    hud->level_y = screen_height - 45.0f;

    // Inventory button (bottom right corner)
    hud->inv_button_size = 50.0f;
    hud->inv_button_x = screen_width - hud->inv_button_size - 20.0f;
    hud->inv_button_y = screen_height - hud->inv_button_size - 20.0f;

    // Character button (to the left of inventory button)
    hud->char_button_size = 50.0f;
    hud->char_button_x = hud->inv_button_x - hud->char_button_size - 10.0f;
    hud->char_button_y = screen_height - hud->char_button_size - 20.0f;

    // Currency displays (right side, below minimap)
    hud->currency_x = screen_width - 200.0f;
    hud->currency_y = hud->minimap_y + hud->minimap_size + 30.0f;
    hud->currency_spacing = 35.0f;
}

/**
 * Test whether a screen position falls within the inventory button.
 *
 * @return      Nonzero when the position is inside the button, otherwise zero.
 */
int hud_check_inventory_button_clicked(const HUDLayout* hud, float mouse_x, float mouse_y) {
    float x = hud->inv_button_x;
    float y = hud->inv_button_y;
    float size = hud->inv_button_size;

    return (mouse_x >= x && mouse_x <= x + size &&
            mouse_y >= y && mouse_y <= y + size);
}

/**
 * Test whether a screen position falls within the character button.
 *
 * @return      Nonzero when the position is inside the button, otherwise zero.
 */
int hud_check_character_button_clicked(const HUDLayout* hud, float mouse_x, float mouse_y) {
    float x = hud->char_button_x;
    float y = hud->char_button_y;
    float size = hud->char_button_size;

    return (mouse_x >= x && mouse_x <= x + size &&
            mouse_y >= y && mouse_y <= y + size);
}

/**
 * Render the complete playing-state HUD in screen coordinates.
 */
void hud_render(const HUDLayout* hud, const GameState* game) {
    if (!game->player.info_loaded) {
        return;
    }

    // Screen space is established by renderer_end_2d before the UI phase; this
    // keeps the HUD self-contained and restores the caller's matrices.
    renderer_begin_screen_space();

    int own_ping = network_get_ping_ms();

    // The full map covers the top-right corner, so its readouts step aside
    int map_open = game->playing->show_map;

    // Render all HUD elements
    if (!map_open) {
        hud_render_minimap(hud, game);
        hud_render_info_bar(hud, game, own_ping);
    }
    hud_render_health_bar(hud, game);
    hud_render_mana_bar(hud, game);
    hud_render_exp_bar(hud, game);
    hud_render_level(hud, game);
    hud_render_inventory_button(hud, game);
    hud_render_character_button(hud, game);
    if (!map_open)
        hud_render_currencies(hud, game);
    hud_render_target_bar(game, (float)hud->screen_width);
    hud_render_player_target_bar(game, (float)hud->screen_width);
    hud_render_party_frames(game);

    if (game->playing->show_session_panel)
        hud_render_session_panel(hud, game, own_ping);

    hud_render_zone_banner(hud, game);
	hud_render_connection_issue(hud, game);

    renderer_end_screen_space();
}

void hud_draw_view_cone(const Camera* camera, float center_x, float center_y, float radius) {
    /* On the ground the camera looks along (-sin yaw, -cos yaw); maps are
     * north-up with y down, the same axes as the world. */
    float dir_x = -sinf(camera->yaw);
    float dir_y = -cosf(camera->yaw);

    /* The horizontal field of view, from the vertical one and the aspect. */
    float aspect = (camera->viewport_height > 0)
                 ? (float)camera->viewport_width / (float)camera->viewport_height : 1.0f;
    float half_v = CAMERA_FOVY_DEG * 0.5f * 3.14159265f / 180.0f;
    float fov_h_deg = 2.0f * atanf(aspect * tanf(half_v)) * 180.0f / 3.14159265f;

    renderer_draw_cone(center_x, center_y, dir_x, dir_y, radius, fov_h_deg,
                       1.0f, 1.0f, 0.85f, 0.22f, 16);
}

/**
 * Render nearby entities and zone information on the minimap.
 */
void hud_render_minimap(const HUDLayout* hud, const GameState* game) {
    float x    = hud->minimap_x;
    float y    = hud->minimap_y;
    float size = hud->minimap_size;
    float px   = game->player.x;
    float py   = game->player.y;

    // Background, then the nearby city and what stands in it
    renderer_draw_rect(x, y, size, size, 0.06f, 0.06f, 0.10f, 0.92f);
    map_minimap_render(game, &(map_rect){ x, y, size, size });

    // Border (drawn on top of the marks so the edges look clean)
    renderer_draw_rect(x,            y,            size, 2.0f, 0.55f, 0.55f, 0.65f, 1.0f);
    renderer_draw_rect(x,            y + size - 2, size, 2.0f, 0.55f, 0.55f, 0.65f, 1.0f);
    renderer_draw_rect(x,            y,            2.0f, size, 0.55f, 0.55f, 0.65f, 1.0f);
    renderer_draw_rect(x + size - 2, y,            2.0f, size, 0.55f, 0.55f, 0.65f, 1.0f);

    // Coordinates below minimap
    char coords[64];
    snprintf(coords, sizeof(coords), "X: %.0f  Y: %.0f", px, py);
    renderer_draw_text(x + 10, y + size + 18, coords);

    // Zone name below coordinates
    if (game->playing->current_zone_name[0] != '\0') {
        renderer_draw_text(x + 10, y + size + 34, game->playing->current_zone_name);
    }
}

/**
 * Render the local player's health bar.
 */
void hud_render_health_bar(const HUDLayout* hud, const GameState* game) {
    float x = hud->health_bar_x;
    float y = hud->health_bar_y;
    float width = hud->health_bar_width;
    float height = hud->health_bar_height;

    // Get health values
    uint32_t current_hp = game->player.info.health;
    uint32_t max_hp = game->player.info.max_health;

    if (max_hp == 0) max_hp = 100; // Default if not loaded yet

    float health_percent = (float)current_hp / (float)max_hp;
    if (health_percent > 1.0f) health_percent = 1.0f;

    // Background (dark)
    renderer_draw_rect(x - 2, y - 2, width + 4, height + 4, 0.0f, 0.0f, 0.0f, 0.8f);
    renderer_draw_rect(x, y, width, height, 0.2f, 0.1f, 0.1f, 1.0f);

    // Health fill (red to yellow based on health)
    float r = 1.0f;
    float g = health_percent * 0.6f;
    float b = 0.0f;
    renderer_draw_rect(x, y, width * health_percent, height, r, g, b, 0.9f);

    // Border
    renderer_draw_rect(x, y, width, 2, 0.6f, 0.6f, 0.6f, 1.0f);
    renderer_draw_rect(x, y + height - 2, width, 2, 0.6f, 0.6f, 0.6f, 1.0f);
    renderer_draw_rect(x, y, 2, height, 0.6f, 0.6f, 0.6f, 1.0f);
    renderer_draw_rect(x + width - 2, y, 2, height, 0.6f, 0.6f, 0.6f, 1.0f);

    // HP Text
    char hp_text[32];
    snprintf(hp_text, sizeof(hp_text), "HP: %u / %u", current_hp, max_hp);
    renderer_draw_text(x + 10, y + height - 5, hp_text);
}

/**
 * Render the local player's resource bar.
 *
 * Which pool this is follows the character's role, not its race, and Human Form
 * carries none at all — so the bar is hidden rather than drawn empty when the
 * character is out of Animal Form.
 */
void hud_render_mana_bar(const HUDLayout* hud, const GameState* game) {
    float x = hud->mana_bar_x;
    float y = hud->mana_bar_y;
    float width = hud->mana_bar_width;
    float height = hud->mana_bar_height;

    const AbilityBarState* bar = &game->playing->ability_bar;
    if (!ability_bar_has_resource(bar)) return;

    int32_t current_mana = bar->resource;
    int32_t max_mana = bar->max_resource;

    float mana_percent = (float)current_mana / (float)max_mana;
    if (mana_percent > 1.0f) mana_percent = 1.0f;
    if (mana_percent < 0.0f) mana_percent = 0.0f;

    // Background (dark)
    renderer_draw_rect(x - 2, y - 2, width + 4, height + 4, 0.0f, 0.0f, 0.0f, 0.8f);
    renderer_draw_rect(x, y, width, height, 0.1f, 0.1f, 0.2f, 1.0f);

    /* Colour and name follow the pool, so a rage tank and a mana healer read
     * differently at a glance without the HUD knowing anything about races. */
    float bar_r, bar_g, bar_b;
    const char* resource_name;

    switch (bar->resource_type) {
        case RESOURCE_RAGE:
            bar_r = 0.85f; bar_g = 0.18f; bar_b = 0.15f;
            resource_name = "Rage";
            break;
        case RESOURCE_STAMINA:
            bar_r = 0.95f; bar_g = 0.75f; bar_b = 0.15f;
            resource_name = "Stamina";
            break;
        case RESOURCE_MANA:
        default:
            bar_r = 0.2f; bar_g = 0.3f; bar_b = 0.9f;
            resource_name = "Mana";
            break;
    }

    // Mana/Energy fill
    renderer_draw_rect(x, y, width * mana_percent, height, bar_r, bar_g, bar_b, 0.9f);

    // Border
    renderer_draw_rect(x, y, width, 2, 0.6f, 0.6f, 0.6f, 1.0f);
    renderer_draw_rect(x, y + height - 2, width, 2, 0.6f, 0.6f, 0.6f, 1.0f);
    renderer_draw_rect(x, y, 2, height, 0.6f, 0.6f, 0.6f, 1.0f);
    renderer_draw_rect(x + width - 2, y, 2, height, 0.6f, 0.6f, 0.6f, 1.0f);

    // Resource Text
    char mana_text[32];
    snprintf(mana_text, sizeof(mana_text), "%s: %d / %d", resource_name, current_mana, max_mana);
    renderer_draw_text(x + 10, y + height - 3, mana_text);
}

/**
 * Render progress toward the server-provided experience threshold.
 */
void hud_render_exp_bar(const HUDLayout* hud, const GameState* game) {
    float x = hud->exp_bar_x;
    float y = hud->exp_bar_y;
    float width = hud->exp_bar_width;
    float height = hud->exp_bar_height;

    uint64_t current_exp = game->player.info.experience;
    uint64_t exp_for_next = game->playing->player_xp_for_next;

    // Use server-provided XP threshold; fallback if not yet received
    if (exp_for_next == 0) exp_for_next = 1000;

    float exp_percent = (float)((double)current_exp / (double)exp_for_next);
    if (exp_percent > 1.0f) exp_percent = 1.0f;
    if (exp_percent < 0.0f) exp_percent = 0.0f;

    // Background
    renderer_draw_rect(x - 2, y - 2, width + 4, height + 4, 0.0f, 0.0f, 0.0f, 0.8f);
    renderer_draw_rect(x, y, width, height, 0.1f, 0.1f, 0.2f, 1.0f);

    // Exp fill (blue/purple gradient)
    renderer_draw_rect(x, y, width * exp_percent, height, 0.4f, 0.6f, 1.0f, 0.9f);

    // Border
    renderer_draw_rect(x, y, width, 2, 0.6f, 0.6f, 0.6f, 1.0f);
    renderer_draw_rect(x, y + height - 2, width, 2, 0.6f, 0.6f, 0.6f, 1.0f);
    renderer_draw_rect(x, y, 2, height, 0.6f, 0.6f, 0.6f, 1.0f);
    renderer_draw_rect(x + width - 2, y, 2, height, 0.6f, 0.6f, 0.6f, 1.0f);

    // XP Text
    char exp_text[64];
    snprintf(exp_text, sizeof(exp_text), "XP: %llu / %llu",
             (unsigned long long)current_exp, (unsigned long long)exp_for_next);
    renderer_draw_text(x + 10, y + height - 3, exp_text);
}

/**
 * Render the local player's level badge.
 */
void hud_render_level(const HUDLayout* hud, const GameState* game) {
    float x = hud->level_x;
    float y = hud->level_y;
    float size = 70.0f;

    // Background circle/square for level
    renderer_draw_rect(x, y, size, size, 0.15f, 0.1f, 0.2f, 0.9f);

    // Border
    renderer_draw_rect(x, y, size, 2, 0.8f, 0.7f, 0.3f, 1.0f);
    renderer_draw_rect(x, y + size - 2, size, 2, 0.8f, 0.7f, 0.3f, 1.0f);
    renderer_draw_rect(x, y, 2, size, 0.8f, 0.7f, 0.3f, 1.0f);
    renderer_draw_rect(x + size - 2, y, 2, size, 0.8f, 0.7f, 0.3f, 1.0f);

    // Level text
    char level_text[16];
    snprintf(level_text, sizeof(level_text), "LVL");
    renderer_draw_text(x + 15, y + 20, level_text);

    char level_num[16];
    snprintf(level_num, sizeof(level_num), "%u", game->player.info.level);
    renderer_draw_text(x + 20, y + 45, level_num);
}

/**
 * Render the inventory button.
 */
void hud_render_inventory_button(const HUDLayout* hud, const GameState* game) {
    (void)game;

    float x = hud->inv_button_x;
    float y = hud->inv_button_y;
    float size = hud->inv_button_size;

    // Button background
    renderer_draw_rect(x, y, size, size, 0.2f, 0.15f, 0.1f, 0.9f);

    // Border (gold color)
    renderer_draw_rect(x, y, size, 2, 0.8f, 0.7f, 0.3f, 1.0f);
    renderer_draw_rect(x, y + size - 2, size, 2, 0.8f, 0.7f, 0.3f, 1.0f);
    renderer_draw_rect(x, y, 2, size, 0.8f, 0.7f, 0.3f, 1.0f);
    renderer_draw_rect(x + size - 2, y, 2, size, 0.8f, 0.7f, 0.3f, 1.0f);

    // Simple bag icon (grid pattern)
    float icon_padding = 12.0f;
    float icon_x = x + icon_padding;
    float icon_y = y + icon_padding;
    float icon_size = size - icon_padding * 2;

    // Draw simple grid for bag
    renderer_draw_rect(icon_x, icon_y + icon_size/3, icon_size, 2, 0.7f, 0.6f, 0.2f, 1.0f);
    renderer_draw_rect(icon_x, icon_y + 2*icon_size/3, icon_size, 2, 0.7f, 0.6f, 0.2f, 1.0f);
    renderer_draw_rect(icon_x + icon_size/3, icon_y, 2, icon_size, 0.7f, 0.6f, 0.2f, 1.0f);
    renderer_draw_rect(icon_x + 2*icon_size/3, icon_y, 2, icon_size, 0.7f, 0.6f, 0.2f, 1.0f);
}

/**
 * Render the character-screen button.
 */
void hud_render_character_button(const HUDLayout* hud, const GameState* game) {
    (void)game;

    float x = hud->char_button_x;
    float y = hud->char_button_y;
    float size = hud->char_button_size;

    // Button background
    renderer_draw_rect(x, y, size, size, 0.15f, 0.2f, 0.15f, 0.9f);

    // Border (green color)
    renderer_draw_rect(x, y, size, 2, 0.3f, 0.8f, 0.3f, 1.0f);
    renderer_draw_rect(x, y + size - 2, size, 2, 0.3f, 0.8f, 0.3f, 1.0f);
    renderer_draw_rect(x, y, 2, size, 0.3f, 0.8f, 0.3f, 1.0f);
    renderer_draw_rect(x + size - 2, y, 2, size, 0.3f, 0.8f, 0.3f, 1.0f);

    // Simple character icon (stick figure)
    float icon_padding = 12.0f;
    float icon_x = x + size / 2;
    float icon_y = y + icon_padding + 8;

    // Head (circle approximated as square)
    renderer_draw_rect(icon_x - 4, icon_y, 8, 8, 0.3f, 0.8f, 0.3f, 1.0f);

    // Body (vertical line)
    renderer_draw_rect(icon_x - 1, icon_y + 8, 2, 12, 0.3f, 0.8f, 0.3f, 1.0f);

    // Arms (horizontal line)
    renderer_draw_rect(icon_x - 8, icon_y + 12, 16, 2, 0.3f, 0.8f, 0.3f, 1.0f);

    // Legs
    renderer_draw_rect(icon_x - 5, icon_y + 20, 2, 8, 0.3f, 0.8f, 0.3f, 1.0f);
    renderer_draw_rect(icon_x + 3, icon_y + 20, 2, 8, 0.3f, 0.8f, 0.3f, 1.0f);
}

/**
 * Render the selected NPC's health panel.
 */
void hud_render_target_bar(const GameState* game, float screen_width) {
    if (game->playing->target_npc_id == 0) return;

    const VisibleNPC* npc = npc_find_by_id(game->playing->visible_npcs, game->playing->visible_npc_count,
                                            game->playing->target_npc_id);
    if (!npc || !npc->is_alive) return;

    float bar_width  = 300.0f;
    float bar_height = 22.0f;
    float x = screen_width / 2.0f - bar_width / 2.0f;
    float y = 16.0f;

    // Background panel
    renderer_draw_rect(x - 4, y - 20, bar_width + 8, bar_height + 28, 0.05f, 0.05f, 0.08f, 0.88f);

    // Panel border
    renderer_draw_rect(x - 4,              y - 20,                 bar_width + 8, 1.5f, 0.5f, 0.5f, 0.6f, 1.0f);
    renderer_draw_rect(x - 4,              y - 20 + bar_height + 28 - 1.5f, bar_width + 8, 1.5f, 0.5f, 0.5f, 0.6f, 1.0f);
    renderer_draw_rect(x - 4,              y - 20,                 1.5f, bar_height + 28, 0.5f, 0.5f, 0.6f, 1.0f);
    renderer_draw_rect(x - 4 + bar_width + 8 - 1.5f, y - 20,      1.5f, bar_height + 28, 0.5f, 0.5f, 0.6f, 1.0f);

    // NPC name above bar
    if (npc->name[0] != '\0') {
        renderer_draw_text(x + bar_width / 2.0f - 20.0f, y - 4.0f, npc->name);
    }

    // Health bar background
    renderer_draw_rect(x, y, bar_width, bar_height, 0.15f, 0.05f, 0.05f, 1.0f);

    // Health fill (red -> yellow -> green)
    if (npc->max_health > 0) {
        float pct = (float)npc->health / (float)npc->max_health;
        if (pct > 1.0f) pct = 1.0f;
        if (pct < 0.0f) pct = 0.0f;
        float hr = (pct < 0.5f) ? 1.0f : (2.0f - pct * 2.0f);
        float hg = (pct > 0.5f) ? 1.0f : (pct * 2.0f);
        renderer_draw_rect(x, y, bar_width * pct, bar_height, hr, hg, 0.05f, 1.0f);
    }

    // Bar border
    renderer_draw_rect(x, y, bar_width, 1.5f, 0.6f, 0.6f, 0.6f, 1.0f);
    renderer_draw_rect(x, y + bar_height - 1.5f, bar_width, 1.5f, 0.6f, 0.6f, 0.6f, 1.0f);
    renderer_draw_rect(x, y, 1.5f, bar_height, 0.6f, 0.6f, 0.6f, 1.0f);
    renderer_draw_rect(x + bar_width - 1.5f, y, 1.5f, bar_height, 0.6f, 0.6f, 0.6f, 1.0f);

    // HP numbers
    char hp_text[32];
    snprintf(hp_text, sizeof(hp_text), "%d / %d", npc->health, npc->max_health);
    renderer_draw_text(x + bar_width / 2.0f - 18.0f, y + bar_height - 4.0f, hp_text);
}

// The Inspect button sits to the right of the player target panel.
#define INSPECT_BUTTON_W 90.0f
#define INSPECT_BUTTON_H 26.0f

static void inspect_button_rect(float screen_width, float* x, float* y) {
	// the panel is 300 wide, centred, with a 4px border and a 12px gap
	*x = screen_width / 2.0f + 150.0f + 4.0f + 12.0f;
	*y = 52.0f + (50.0f - INSPECT_BUTTON_H) / 2.0f;
}

// Whether the targeted player is among those in view, and so the bar is drawn.
static int target_player_in_view(const GameState* game) {
	if(game->playing->target_player_id == 0)
		return 0;

	for(int i = 0; i < game->playing->nearby_player_count; i++) {
		if(game->playing->nearby_players[i].player_id == game->playing->target_player_id)
			return 1;
	}

	return 0;
}

int hud_check_inspect_button_clicked(const GameState* game, float screen_width,
		float mouse_x, float mouse_y) {
	float x;
	float y;

	if(!target_player_in_view(game))
		return 0;

	inspect_button_rect(screen_width, &x, &y);

	return mouse_x >= x && mouse_x <= x + INSPECT_BUTTON_W &&
	       mouse_y >= y && mouse_y <= y + INSPECT_BUTTON_H;
}

static void render_inspect_button(float screen_width) {
	float x;
	float y;

	inspect_button_rect(screen_width, &x, &y);
	renderer_draw_rect(x, y, INSPECT_BUTTON_W, INSPECT_BUTTON_H, 0.05f, 0.08f, 0.10f, 0.88f);
	renderer_draw_rect(x, y, INSPECT_BUTTON_W, 1.5f, 0.30f, 0.65f, 0.70f, 1.0f);
	renderer_draw_rect(x, y + INSPECT_BUTTON_H - 1.5f, INSPECT_BUTTON_W, 1.5f, 0.30f, 0.65f, 0.70f, 1.0f);
	renderer_draw_rect(x, y, 1.5f, INSPECT_BUTTON_H, 0.30f, 0.65f, 0.70f, 1.0f);
	renderer_draw_rect(x + INSPECT_BUTTON_W - 1.5f, y, 1.5f, INSPECT_BUTTON_H, 0.30f, 0.65f, 0.70f, 1.0f);
	renderer_draw_text_centered(x, y, INSPECT_BUTTON_W, INSPECT_BUTTON_H, "Inspect");
}

/**
 * Render the selected nearby player's health panel.
 */
void hud_render_player_target_bar(const GameState* game, float screen_width) {
    if (game->playing->target_player_id == 0) return;

    // Find the targeted player in the nearby list
    const NearbyPlayer* np = NULL;
    for (int i = 0; i < game->playing->nearby_player_count; i++) {
        if (game->playing->nearby_players[i].player_id == game->playing->target_player_id) {
            np = &game->playing->nearby_players[i];
            break;
        }
    }
    if (!np) return;

    float bar_width  = 300.0f;
    float bar_height = 22.0f;
    // Place below the NPC target bar (which is at y=16); offset by bar panel height (50px)
    float x = screen_width / 2.0f - bar_width / 2.0f;
    float y = 72.0f;   // 16 + 28 (panel pad above) + 22 (bar) + 6

    // Background panel (teal tint to distinguish from enemy red)
    renderer_draw_rect(x - 4, y - 20, bar_width + 8, bar_height + 28, 0.05f, 0.08f, 0.10f, 0.88f);

    // Panel border (teal)
    renderer_draw_rect(x - 4,              y - 20,                        bar_width + 8, 1.5f, 0.30f, 0.65f, 0.70f, 1.0f);
    renderer_draw_rect(x - 4,              y - 20 + bar_height + 28 - 1.5f, bar_width + 8, 1.5f, 0.30f, 0.65f, 0.70f, 1.0f);
    renderer_draw_rect(x - 4,              y - 20,                        1.5f, bar_height + 28, 0.30f, 0.65f, 0.70f, 1.0f);
    renderer_draw_rect(x - 4 + bar_width + 8 - 1.5f, y - 20,             1.5f, bar_height + 28, 0.30f, 0.65f, 0.70f, 1.0f);

    // Name above bar
    char label[40];
    snprintf(label, sizeof(label), "%s", np->name);
    renderer_draw_text(x + bar_width / 2.0f - 20.0f, y - 4.0f, label);

    // HP bar background
    renderer_draw_rect(x, y, bar_width, bar_height, 0.10f, 0.05f, 0.05f, 1.0f);

    // HP fill (green tint for friendly)
    if (np->max_health > 0) {
        float pct = (float)np->health / (float)np->max_health;
        if (pct > 1.0f) pct = 1.0f;
        if (pct < 0.0f) pct = 0.0f;
        renderer_draw_rect(x, y, bar_width * pct, bar_height, 0.10f, 0.70f, 0.30f, 1.0f);
    }

    // Bar border
    renderer_draw_rect(x, y, bar_width, 1.5f, 0.6f, 0.6f, 0.6f, 1.0f);
    renderer_draw_rect(x, y + bar_height - 1.5f, bar_width, 1.5f, 0.6f, 0.6f, 0.6f, 1.0f);
    renderer_draw_rect(x, y, 1.5f, bar_height, 0.6f, 0.6f, 0.6f, 1.0f);
    renderer_draw_rect(x + bar_width - 1.5f, y, 1.5f, bar_height, 0.6f, 0.6f, 0.6f, 1.0f);

    // HP numbers
    char hp_text[32];
    snprintf(hp_text, sizeof(hp_text), "%d / %d", np->health, np->max_health);
    renderer_draw_text(x + bar_width / 2.0f - 18.0f, y + bar_height - 4.0f, hp_text);

	render_inspect_button(screen_width);
}

/**
 * Render health and resource frames for the current party.
 */
void hud_render_party_frames(const GameState* game) {
    if (!game->playing->party.has_party || game->playing->party.member_count == 0) return;

    const float FRAME_W  = 180.0f;
    const float FRAME_H  = 50.0f;
    const float FRAME_GAP = 5.0f;
    const float BAR_PAD  = 6.0f;
    const float BAR_W    = FRAME_W - BAR_PAD * 2.0f;
    const float HP_H     = 10.0f;
    const float MP_H     = 8.0f;
    const float START_X  = 10.0f;
    const float START_Y  = 10.0f;

    for (int i = 0; i < game->playing->party.member_count; i++) {
        const PartyMember* m = &game->playing->party.members[i];

        float fx = START_X;
        float fy = START_Y + i * (FRAME_H + FRAME_GAP);

        // Background + border
        renderer_draw_rect(fx, fy, FRAME_W, FRAME_H, 0.05f, 0.05f, 0.08f, 0.85f);
        renderer_draw_rect(fx,            fy,                FRAME_W, 1.5f, 0.45f, 0.45f, 0.55f, 1.0f);
        renderer_draw_rect(fx,            fy + FRAME_H - 1.5f, FRAME_W, 1.5f, 0.45f, 0.45f, 0.55f, 1.0f);
        renderer_draw_rect(fx,            fy,            1.5f, FRAME_H, 0.45f, 0.45f, 0.55f, 1.0f);
        renderer_draw_rect(fx + FRAME_W - 1.5f, fy, 1.5f, FRAME_H, 0.45f, 0.45f, 0.55f, 1.0f);

        // Name + level
        char label[48];
        int is_leader = (m->id == game->playing->party.leader_id);
        snprintf(label, sizeof(label), "%s%s (%u)",
                 is_leader ? "* " : "", m->name, (unsigned)m->level);
        renderer_draw_text(fx + BAR_PAD, fy + 12.0f, label);

        // HP bar
        float hp_y = fy + 18.0f;
        float hp_pct = (m->max_health > 0)
            ? (float)m->health / (float)m->max_health : 0.0f;
        if (hp_pct > 1.0f) hp_pct = 1.0f;
        if (hp_pct < 0.0f) hp_pct = 0.0f;
        renderer_draw_rect(fx + BAR_PAD, hp_y, BAR_W, HP_H, 0.2f, 0.05f, 0.05f, 1.0f);
        float hr = 1.0f, hg = hp_pct * 0.6f;
        renderer_draw_rect(fx + BAR_PAD, hp_y, BAR_W * hp_pct, HP_H, hr, hg, 0.0f, 0.9f);

        // Mana bar
        float mp_y = fy + 32.0f;
        float mp_pct = (m->max_mana > 0)
            ? (float)m->mana / (float)m->max_mana : 0.0f;
        if (mp_pct > 1.0f) mp_pct = 1.0f;
        if (mp_pct < 0.0f) mp_pct = 0.0f;
        renderer_draw_rect(fx + BAR_PAD, mp_y, BAR_W, MP_H, 0.05f, 0.05f, 0.2f, 1.0f);
        /* Blue for mana, yellow for everything else.
         *
         * This was `player_class == 2` -- one hardcoded race number -- so the
         * second race to run on something other than mana would have drawn a
         * mana bar for a resource that is not mana. The registry knows which
         * role each race starts in; ask it. */
        float mr = 0.2f, mg = 0.3f, mb = 0.9f;
        /* PartyMember.player_class carries the fused race/class identifier --
         * the server fills class and race from the same race_id -- so it is
         * what the registry is keyed on. */
        if (client_race_uses_energy(m->player_class)) { mr = 1.0f; mg = 0.9f; mb = 0.2f; }
        renderer_draw_rect(fx + BAR_PAD, mp_y, BAR_W * mp_pct, MP_H, mr, mg, mb, 0.9f);
    }
}

/**
 * Render the local player's currency display.
 *
 * Only the coin of the nearest kingdom appears here, because that is the one
 * local trade uses. The currency panel holds every balance.
 */
void hud_render_currencies(const HUDLayout* hud, const GameState* game) {
    float x = hud->currency_x;
    float y = hud->currency_y;

    int local = (int)world_local_currency_px(game->player.x, game->player.y);

    // Balances arrive from the server via PLAYER_DATA and the shop responses.
    char gold_text[64];
    snprintf(gold_text, sizeof(gold_text), "%s: %u",
             world_currency_city_name(local),
             world_currency_valid(local) ? game->player.info.currency[local] : 0u);
    float tw = 160.0f, th = 22.0f;
    renderer_draw_rect(x - 6, y - 16, tw, th, 0.10f, 0.09f, 0.04f, 0.85f);
    renderer_draw_rect(x - 6, y - 16, tw, 1.5f, 0.70f, 0.55f, 0.10f, 0.8f);
    renderer_draw_rect(x - 6, y - 16 + th - 1.5f, tw, 1.5f, 0.70f, 0.55f, 0.10f, 0.8f);
    renderer_draw_rect(x - 6, y - 16, 1.5f, th, 0.70f, 0.55f, 0.10f, 0.8f);
    renderer_draw_rect(x - 6 + tw - 1.5f, y - 16, 1.5f, th, 0.70f, 0.55f, 0.10f, 0.8f);
    renderer_draw_text(x, y, gold_text);
}

// Pixels the text of the upper right info bar is drawn below its place in the bar
#define BADGE_TEXT_DROP 3.0f

// Height of a capital letter or digit in the 22 px font, which the icons centre on
#define BADGE_CAP_HEIGHT 14.0f

// Side of an info bar icon. The art is 32 px; half size fits the strip above the minimap.
#define INFO_ICON_SIZE 16.0f

// Space between an icon and its text
#define INFO_ICON_PAD 4.0f

// Space between one item and the next
#define INFO_ITEM_GAP 8.0f

// Width of the box the ping used to be drawn in, which its right edge still follows
#define INFO_BAR_W 100.0f

// Where the info bar sits: the upper right, just above the minimap
static void info_bar_rect(const HUDLayout* hud, float* x, float* y, float* h) {
	*h = 18.0f;
	*x = hud->minimap_x + hud->minimap_size - INFO_BAR_W;
	*y = hud->minimap_y - *h - 4.0f;
}

// The baseline of the bar's text; y and h are the bar's
static float info_baseline(float y, float h) {
	return y + h - 3 + BADGE_TEXT_DROP;
}

// An icon with its left edge at x, centred on the height of a capital letter
static void draw_info_icon(unsigned int texture, float x, float y, float h) {
	float centre = info_baseline(y, h) - BADGE_CAP_HEIGHT / 2.0f;

	if(!texture)
		return;

	renderer_draw_sprite(x, centre - INFO_ICON_SIZE / 2.0f, INFO_ICON_SIZE, INFO_ICON_SIZE,
	                     texture);
}

// Where the parts of the info bar sit; the weather and the clock lead
typedef struct {
	float box_x;
	float y;
	float h;
	float baseline;
	float weather_x;
	float clock_text_x;
	float clock_w;
	float ping_icon_x;
	float ping_text_x;
} InfoBarLayout;

// Lay the bar out for the clock text it is about to show
static InfoBarLayout info_bar_layout(const HUDLayout* hud, const GameState* game,
                                     char* clock_text, size_t clock_size) {
	InfoBarLayout bar;
	int hour24 = game->settings.clock_24h;

	info_bar_rect(hud, &bar.box_x, &bar.y, &bar.h);
	bar.baseline = info_baseline(bar.y, bar.h);

	// Wide enough for the widest reading, so the item does not change size as the minutes pass
	world_clock_text_now(hour24, clock_text, clock_size);
	bar.clock_w = renderer_text_width(hour24 ? "00:00" : "12:00 AM");
	if(renderer_text_width(clock_text) > bar.clock_w)
		bar.clock_w = renderer_text_width(clock_text);

	bar.ping_text_x = bar.box_x + 2.0f + renderer_text_width("Ping: ");
	bar.ping_icon_x = bar.ping_text_x - INFO_ICON_PAD - INFO_ICON_SIZE;
	bar.clock_text_x = bar.ping_icon_x - INFO_ITEM_GAP - bar.clock_w;
	bar.weather_x = bar.clock_text_x - INFO_ICON_PAD - INFO_ICON_SIZE;
	return bar;
}

// Today's condition in the player's region, or the sun before a forecast
static int info_bar_condition(const GameState* game) {
	WeatherDayData today;
	int region = weather_region_at(game->player.x, game->player.y);

	if(weather_state_day_now(region, 0, &today))
		return today.condition;
	return WEATHER_CLEAR;
}

int hud_check_weather_clicked(const HUDLayout* hud, const GameState* game, float mouse_x,
                              float mouse_y) {
	char clock_text[16];
	InfoBarLayout bar = info_bar_layout(hud, game, clock_text, sizeof(clock_text));
	float right = bar.clock_text_x + bar.clock_w;

	return mouse_x >= bar.weather_x && mouse_x < right && mouse_y >= bar.y &&
	       mouse_y < bar.y + bar.h;
}

/**
 * Render the info bar in the upper right, left to right: the weather and the
 * time of day, then the ping bars and the ping.
 *
 * The ping's digits stay where they were when "Ping: " stood in front of them,
 * and the time follows from there, a fixed gap before the ping icon.
 * The weather is today's in the region the player stands in, and the sun until
 * the server has sent a forecast.
 *
 * @param ping_ms  One-way latency in milliseconds, or a negative value when unavailable.
 */
void hud_render_info_bar(const HUDLayout* hud, const GameState* game, int ping_ms) {
	const GameTextures* textures = &game->textures;
	char clock_text[16];
	char ping_text[16];
	InfoBarLayout bar = info_bar_layout(hud, game, clock_text, sizeof(clock_text));
	float icon_y = bar.baseline - BADGE_CAP_HEIGHT / 2.0f - INFO_ICON_SIZE / 2.0f;
	int night = world_clock_synced() && weather_is_night(world_clock_hour_now());

	if(ping_ms < 0)
		snprintf(ping_text, sizeof(ping_text), "--ms");
	else
		snprintf(ping_text, sizeof(ping_text), "%dms", ping_ms);

	weather_icon_draw(textures, info_bar_condition(game), night, bar.weather_x, icon_y,
	                  INFO_ICON_SIZE);
	renderer_draw_text(bar.clock_text_x, bar.baseline, clock_text);

	draw_info_icon(textures->hud_ping[net_ping_level(ping_ms)], bar.ping_icon_x, bar.y, bar.h);
	renderer_draw_text(bar.ping_text_x, bar.baseline, ping_text);
}

// Seconds of nothing from the server before the connection icon shows
#define CONNECTION_ISSUE_SILENCE 3.0

// Side of the connection icon, in the HUD's own units
#define CONNECTION_ICON_SIZE 32.0f

// Gap between the top of the screen and the connection icon
#define CONNECTION_ICON_TOP 12.0f

/**
 * Render the connection-issue icon at the top centre of the screen.
 *
 * Shown while the server has gone quiet or while recovery is under way, and
 * gone as soon as data flows again. It is the worst ping level's icon, so it
 * reads as the same family as the ping bars in the corner. Nothing is locked:
 * the player can keep moving while the client tries to get back in.
 */
void hud_render_connection_issue(const HUDLayout* hud, const GameState* game) {
	unsigned int icon = game->textures.hud_ping[0];

	if(!icon)
		return;

	if(net_reconnect_phase() == RECONNECT_IDLE &&
	   network_silence_seconds() < CONNECTION_ISSUE_SILENCE)
		return;

	renderer_draw_sprite((float)hud->screen_width * 0.5f - CONNECTION_ICON_SIZE * 0.5f,
	                     CONNECTION_ICON_TOP, CONNECTION_ICON_SIZE, CONNECTION_ICON_SIZE, icon);
}

/** Map a protocol race identifier to the name the server's registry gives it. */
static const char* session_race_name(uint8_t race) {
	const RaceInfo* info = client_race_find(race);

	return info ? info->name : "Unknown";
}

// Layout constants shared between render and click-handler, in window pixels
#define SESSION_PANEL_W    540.0f
#define SESSION_ROW_H       24.0f
#define SESSION_HEADER_H    46.0f
#define SESSION_COL_H       28.0f
#define SESSION_FOOTER_H    48.0f
#define SESSION_PAD         16.0f
#define SESSION_ROWS        SESSION_LIST_PAGE_SIZE  // 30 rows per page
#define SESSION_CLOSE       24.0f
#define SESSION_TEXT        14.0f
#define SESSION_HEAD_TEXT   13.0f

// Column X offsets (relative to panel left edge)
#define SESSION_COL_LV      SESSION_PAD
#define SESSION_COL_NAME    64.0f
#define SESSION_COL_RACE   224.0f
#define SESSION_COL_STATUS 360.0f
#define SESSION_COL_PING   (SESSION_PANEL_W - 76.0f)

// Total fixed panel height: header + col-header row + 30 rows + footer
#define SESSION_PANEL_H    (SESSION_HEADER_H + SESSION_COL_H + \
                            SESSION_ROWS * SESSION_ROW_H + SESSION_FOOTER_H)

// Prev/Next button dimensions (inside footer)
#define SESSION_BTN_W  88.0f
#define SESSION_BTN_H  28.0f

// The panel's top left: where it was dragged, else centred
static void session_panel_origin(const HUDLayout* hud, const window_pos* pos, float* out_x,
                                 float* out_y) {
    window_pos_origin(pos, (float)hud->screen_width, (float)hud->screen_height, SESSION_PANEL_W,
                      SESSION_PANEL_H, out_x, out_y);
}

// Where the pagination buttons and the close button sit
static float session_btn_y(float py) {
    return py + SESSION_PANEL_H - SESSION_FOOTER_H + (SESSION_FOOTER_H - SESSION_BTN_H) / 2.0f;
}

static float session_close_x(float px) {
    return px + SESSION_PANEL_W - SESSION_CLOSE - 10.0f;
}

static int can_page_prev(const GameState* game) {
    return game->playing->session_current_page > 0;
}

static int can_page_next(const GameState* game) {
    return game->playing->session_total_pages > 0 &&
           game->playing->session_current_page + 1 < game->playing->session_total_pages;
}

// One player's row
static void render_session_row(const GameState* game, int i, float px, float row_y) {
    const SessionPlayer* p = &game->playing->session_list[i];
    float cy = row_y + SESSION_ROW_H * 0.5f;
    char lv_buf[8];
    char ping_buf[16];

    // Self highlight (page 0, first entry is self), else alternating shade
    if (i == 0 && game->playing->session_current_page == 0)
        ui_kit_px_rect(px + 8.0f, row_y, SESSION_PANEL_W - 16.0f, SESSION_ROW_H - 1.0f,
                       0.35f, 0.75f, 0.45f, 0.16f);
    else if (i % 2 == 1)
        ui_kit_px_rect(px + 8.0f, row_y, SESSION_PANEL_W - 16.0f, SESSION_ROW_H - 1.0f,
                       1.0f, 1.0f, 1.0f, 0.05f);

    snprintf(lv_buf, sizeof(lv_buf), "%u", (unsigned)p->level);
    if (p->ping_ms == NET_PING_UNKNOWN)
        snprintf(ping_buf, sizeof(ping_buf), "--");
    else
        snprintf(ping_buf, sizeof(ping_buf), "%dms", (int)p->ping_ms);

    ui_kit_px_text(px + SESSION_COL_LV, cy, lv_buf, SESSION_TEXT, KIT_DIM, KIT_LEFT);
    ui_kit_px_text(px + SESSION_COL_NAME, cy, p->name, SESSION_TEXT, KIT_WHITE, KIT_LEFT);
    ui_kit_px_text(px + SESSION_COL_RACE, cy, session_race_name(p->player_race), SESSION_TEXT,
                   KIT_WHITE, KIT_LEFT);
    ui_kit_px_text(px + SESSION_COL_STATUS, cy, session_status_label(p->status), SESSION_TEXT,
                   KIT_WHITE, KIT_LEFT);
    ui_kit_px_text(px + SESSION_COL_PING, cy, ping_buf, SESSION_TEXT, KIT_DIM, KIT_LEFT);
}

// The Prev and Next buttons and the page they are on
static void render_session_footer(const GameState* game, float px, float py) {
    float btn_y = session_btn_y(py);
    float next_x = px + SESSION_PANEL_W - SESSION_PAD - SESSION_BTN_W;
    float prev_x = px + SESSION_PAD;
    char page_buf[48];

    ui_kit_px_divider(px + SESSION_PAD, py + SESSION_PANEL_H - SESSION_FOOTER_H,
                      SESSION_PANEL_W - 2.0f * SESSION_PAD);
    ui_kit_px_button(prev_x, btn_y, SESSION_BTN_W, SESSION_BTN_H, "< Prev", 0, KIT_BTN_NORMAL);
    ui_kit_px_button(next_x, btn_y, SESSION_BTN_W, SESSION_BTN_H, "Next >", 0, KIT_BTN_NORMAL);

    // A button that cannot be used is dimmed
    if (!can_page_prev(game))
        ui_kit_px_rect(prev_x, btn_y, SESSION_BTN_W, SESSION_BTN_H, 0.05f, 0.05f, 0.09f, 0.65f);
    if (!can_page_next(game))
        ui_kit_px_rect(next_x, btn_y, SESSION_BTN_W, SESSION_BTN_H, 0.05f, 0.05f, 0.09f, 0.65f);

    if (game->playing->session_total_pages > 0)
        snprintf(page_buf, sizeof(page_buf), "Page %u / %u",
                 (unsigned)game->playing->session_current_page + 1,
                 (unsigned)game->playing->session_total_pages);
    else
        snprintf(page_buf, sizeof(page_buf), "Loading...");
    ui_kit_px_text(px + SESSION_PANEL_W * 0.5f, btn_y + SESSION_BTN_H * 0.5f, page_buf,
                   SESSION_TEXT, KIT_DIM, KIT_CENTER);
}

/**
 * Render the paginated server session roster.
 *
 * @param own_ping_ms  Retained for the caller interface but currently unused.
 */
void hud_render_session_panel(const HUDLayout* hud, const GameState* game, int own_ping_ms) {
    // Each row carries its own ping from the server, so the viewer's own value
    // is not needed here. Kept in the signature for callers.
    (void)own_ping_ms;

    float px;
    float py;
    float col_y;
    float row_y;
    char header[64];

    session_panel_origin(hud, &game->playing->session_win, &px, &py);
    col_y = py + SESSION_HEADER_H + SESSION_COL_H * 0.5f;
    row_y = py + SESSION_HEADER_H + SESSION_COL_H;
    snprintf(header, sizeof(header), "Players Online: %u", game->playing->session_total_players);

    ui_kit_px_begin(hud->screen_width, hud->screen_height);
    ui_kit_px_rect(px, py, SESSION_PANEL_W, SESSION_PANEL_H, 0.05f, 0.05f, 0.09f, 0.97f);
    ui_kit_px_panel(px, py, SESSION_PANEL_W, SESSION_PANEL_H);
    ui_kit_px_text(px + SESSION_PAD, py + SESSION_HEADER_H * 0.5f, header, 20.0f, KIT_GOLD,
                   KIT_LEFT);
    ui_kit_px_close_button(session_close_x(px), py + 11.0f, SESSION_CLOSE, 0);

    ui_kit_px_text(px + SESSION_COL_LV, col_y, "Lv", SESSION_HEAD_TEXT, KIT_HEAD, KIT_LEFT);
    ui_kit_px_text(px + SESSION_COL_NAME, col_y, "Name", SESSION_HEAD_TEXT, KIT_HEAD, KIT_LEFT);
    ui_kit_px_text(px + SESSION_COL_RACE, col_y, "Race", SESSION_HEAD_TEXT, KIT_HEAD, KIT_LEFT);
    ui_kit_px_text(px + SESSION_COL_STATUS, col_y, "Status", SESSION_HEAD_TEXT, KIT_HEAD,
                   KIT_LEFT);
    ui_kit_px_text(px + SESSION_COL_PING, col_y, "Ping", SESSION_HEAD_TEXT, KIT_HEAD, KIT_LEFT);
    ui_kit_px_divider(px + SESSION_PAD, row_y - 2.0f, SESSION_PANEL_W - 2.0f * SESSION_PAD);

    for (int i = 0; i < game->playing->session_list_count; i++) {
        render_session_row(game, i, px, row_y);
        row_y += SESSION_ROW_H;
    }

    render_session_footer(game, px, py);
    ui_kit_px_end();
}

/** Total lifetime of a zone-entry banner, in seconds. */
#define ZONE_BANNER_DURATION   4.0f
/** Duration of each zone-banner fade, in seconds. */
#define ZONE_BANNER_FADE_TIME  0.5f
#define ZONE_BANNER_BAR_H      2.0f
#define ZONE_BANNER_TEXT_Y_OFF 0.38f   // fraction of screen height

/**
 * Render the timed zone-entry banner.
 */
void hud_render_zone_banner(const HUDLayout* hud, const GameState* game) {
    if (!game->playing) return;
    float t = game->playing->zone_banner_timer;
    if (t <= 0.0f) return;

    float alpha;
    if (t < ZONE_BANNER_FADE_TIME) {
        alpha = t / ZONE_BANNER_FADE_TIME;
    } else if (t > ZONE_BANNER_DURATION - ZONE_BANNER_FADE_TIME) {
        alpha = (ZONE_BANNER_DURATION - t) / ZONE_BANNER_FADE_TIME;
    } else {
        alpha = 1.0f;
    }
    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;

    float sw = (float)hud->screen_width;
    float sh = (float)hud->screen_height;

    // Horizontal accent bars flanking the text
    float bar_y = sh * ZONE_BANNER_TEXT_Y_OFF - 18.0f;
    float bar_w = sw * 0.28f;
    float gap    = sw * 0.06f;
    float cx     = sw * 0.5f;
    // Left bar
    renderer_draw_rect(cx - gap - bar_w, bar_y, bar_w, ZONE_BANNER_BAR_H,
                       0.78f, 0.68f, 0.45f, alpha);
    // Right bar
    renderer_draw_rect(cx + gap, bar_y, bar_w, ZONE_BANNER_BAR_H,
                       0.78f, 0.68f, 0.45f, alpha);

    // Zone name — gold-white tint, faded by alpha
    float text_y = sh * ZONE_BANNER_TEXT_Y_OFF;
    renderer_draw_text_primitive(cx - (float)(strlen(game->playing->zone_banner_name) * 4),
                                 text_y,
                                 game->playing->zone_banner_name,
                                 0.95f * alpha, 0.88f * alpha, 0.55f * alpha);
}

/**
 * Resolve a click against the session panel's controls.
 *
 * @return      -1 for the previous page, 1 for the next page, 2 for close, or 0.
 */
int hud_session_panel_handle_click(const GameState* game, float mx, float my) {
    // Must mirror the layout in hud_render_session_panel
    const HUDLayout* hud = &game->playing->hud;
    float px;
    float py;
    float btn_y;
    float prev_x;
    float next_x;
    float close_x;

    session_panel_origin(hud, &game->playing->session_win, &px, &py);
    btn_y = session_btn_y(py);
    prev_x = px + SESSION_PAD;
    next_x = px + SESSION_PANEL_W - SESSION_PAD - SESSION_BTN_W;
    close_x = session_close_x(px);

    if (mx >= close_x && mx <= close_x + SESSION_CLOSE &&
        my >= py + 11.0f && my <= py + 11.0f + SESSION_CLOSE)
        return 2;

    if (mx >= prev_x && mx <= prev_x + SESSION_BTN_W &&
        my >= btn_y  && my <= btn_y + SESSION_BTN_H)
        return -1;

    if (mx >= next_x && mx <= next_x + SESSION_BTN_W &&
        my >= btn_y  && my <= btn_y + SESSION_BTN_H)
        return 1;

    return 0;
}

/**
 * Carry the session panel about by its title strip.
 *
 * @return      Nonzero while the panel is held, so the click goes no further.
 */
int hud_session_panel_drag(GameState* game, float mx, float my, int clicked, int down) {
    const HUDLayout* hud = &game->playing->hud;

    // The close button is left out of the strip
    return window_pos_drag(&game->playing->session_win, (float)hud->screen_width,
                           (float)hud->screen_height, SESSION_PANEL_W, SESSION_PANEL_H,
                           SESSION_PANEL_W - SESSION_CLOSE - 20.0f, SESSION_HEADER_H, mx, my,
                           clicked, down);
}
