/**
 * @file
 * Render the in-game heads-up display and handle its panel controls.
 */
#include "hud.h"
#include "renderer.h"
#include "core/game_types.h"
#include "world/npc.h"
#include "network/network.h"
#include <stdio.h>
#include <math.h>
#include <string.h>

/**
 * Initialize HUD element positions for the current viewport.
 */
void hud_init(HUDLayout* hud, int screen_width, int screen_height) {
    printf("[HUD] Initializing %dx%d\n", screen_width, screen_height);
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

    // Render all HUD elements
    hud_render_minimap(hud, game);
    hud_render_ping(hud, own_ping);
    hud_render_health_bar(hud, game);
    hud_render_mana_bar(hud, game);
    hud_render_exp_bar(hud, game);
    hud_render_level(hud, game);
    hud_render_inventory_button(hud, game);
    hud_render_character_button(hud, game);
    hud_render_currencies(hud, game);
    hud_render_target_bar(game, (float)hud->screen_width);
    hud_render_player_target_bar(game, (float)hud->screen_width);
    hud_render_party_frames(game);

    if (game->playing->show_session_panel)
        hud_render_session_panel(hud, game, own_ping);

    hud_render_zone_banner(hud, game);

    renderer_end_screen_space();
}

/**
 * Render nearby entities and zone information on the minimap.
 */
void hud_render_minimap(const HUDLayout* hud, const GameState* game) {
    float x    = hud->minimap_x;
    float y    = hud->minimap_y;
    float size = hud->minimap_size;

    // Background
    renderer_draw_rect(x, y, size, size, 0.06f, 0.06f, 0.10f, 0.92f);

    // Player world position — everything is drawn relative to this
    float px = game->player.x;
    float py = game->player.y;

    // View radius: how many world units are shown to each edge of the minimap
    float view_radius = 350.0f;
    float scale       = (size * 0.5f) / view_radius;
    float cx          = x + size * 0.5f;
    float cy          = y + size * 0.5f;

    // Helper: world position -> minimap pixel (returns 0 if off-map)
    // We'll inline it below.

    // --- NPC dots ---
    for (int i = 0; i < game->playing->visible_npc_count; i++) {
        const VisibleNPC* npc = &game->playing->visible_npcs[i];
        if (!npc->is_alive) continue;

        float dx = (npc->pos_x - px) * scale;
        float dy = (npc->pos_y - py) * scale;
        float dot_x = cx + dx - 2.5f;
        float dot_y = cy + dy - 2.5f;
        if (dot_x < x || dot_x > x + size - 5 ||
            dot_y < y || dot_y > y + size - 5) continue;

        float dr, dg, db;
        switch (npc->category) {
            case 1:  dr=0.90f; dg=0.20f; db=0.20f; break; // hostile  - red
            case 2:  dr=0.90f; dg=0.78f; db=0.10f; break; // quest    - gold
            default: dr=0.20f; dg=0.55f; db=0.90f; break; // passive  - blue
        }
        renderer_draw_rect(dot_x, dot_y, 5.0f, 5.0f, dr, dg, db, 0.95f);
    }

    // --- Nearby player dots ---
    for (int i = 0; i < game->playing->nearby_player_count; i++) {
        const NearbyPlayer* p = &game->playing->nearby_players[i];
        if (p->is_dead) continue;

        float dx = (p->pos_x - px) * scale;
        float dy = (p->pos_y - py) * scale;
        float dot_x = cx + dx - 3.0f;
        float dot_y = cy + dy - 3.0f;
        if (dot_x < x || dot_x > x + size - 6 ||
            dot_y < y || dot_y > y + size - 6) continue;

        renderer_draw_rect(dot_x, dot_y, 6.0f, 6.0f, 0.25f, 0.90f, 0.45f, 1.0f);
    }

    // --- Player dot (always at center, drawn last so it's on top) ---
    renderer_draw_rect(cx - 5.0f, cy - 5.0f, 10.0f, 10.0f, 1.0f, 1.0f, 0.25f, 1.0f);

    // Border (drawn on top of dots so the edges look clean)
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
 * Render the local player's class resource bar.
 */
void hud_render_mana_bar(const HUDLayout* hud, const GameState* game) {
    float x = hud->mana_bar_x;
    float y = hud->mana_bar_y;
    float width = hud->mana_bar_width;
    float height = hud->mana_bar_height;
    
    // Read from ability bar state instead of player info
    int32_t current_mana = game->playing->ability_bar.mana;
    int32_t max_mana = game->playing->ability_bar.max_mana;
    
    if (max_mana <= 0) return; // Don't draw if no max mana
    
    float mana_percent = (float)current_mana / (float)max_mana;
    if (mana_percent > 1.0f) mana_percent = 1.0f;
    if (mana_percent < 0.0f) mana_percent = 0.0f;
    
    // Background (dark)
    renderer_draw_rect(x - 2, y - 2, width + 4, height + 4, 0.0f, 0.0f, 0.0f, 0.8f);
    renderer_draw_rect(x, y, width, height, 0.1f, 0.1f, 0.2f, 1.0f);
    
    // Determine color based on class (Ninja = yellow/energy, others = blue/mana)
    float bar_r, bar_g, bar_b;
    const char* resource_name;
    
    if (game->player.info.player_class == 2) { // NINJA class
        bar_r = 1.0f; bar_g = 0.9f; bar_b = 0.2f; // Yellow
        resource_name = "Energy";
    } else {
        bar_r = 0.2f; bar_g = 0.3f; bar_b = 0.9f; // Blue
        resource_name = "Mana";
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
        // Ninja (class 2) gets yellow energy bar, others get blue mana
        float mr = 0.2f, mg = 0.3f, mb = 0.9f;
        if (m->player_class == 2) { mr = 1.0f; mg = 0.9f; mb = 0.2f; }
        renderer_draw_rect(fx + BAR_PAD, mp_y, BAR_W * mp_pct, MP_H, mr, mg, mb, 0.9f);
    }
}

/**
 * Render the local player's currency display.
 */
void hud_render_currencies(const HUDLayout* hud, const GameState* game) {
    float x = hud->currency_x;
    float y = hud->currency_y;

    // Gold — received from server via KILL_REWARD / PLAYER_DATA
    char gold_text[64];
    snprintf(gold_text, sizeof(gold_text), "Gold: %u", game->player.info.gold);
    float tw = 130.0f, th = 22.0f;
    renderer_draw_rect(x - 6, y - 16, tw, th, 0.10f, 0.09f, 0.04f, 0.85f);
    renderer_draw_rect(x - 6, y - 16, tw, 1.5f, 0.70f, 0.55f, 0.10f, 0.8f);
    renderer_draw_rect(x - 6, y - 16 + th - 1.5f, tw, 1.5f, 0.70f, 0.55f, 0.10f, 0.8f);
    renderer_draw_rect(x - 6, y - 16, 1.5f, th, 0.70f, 0.55f, 0.10f, 0.8f);
    renderer_draw_rect(x - 6 + tw - 1.5f, y - 16, 1.5f, th, 0.70f, 0.55f, 0.10f, 0.8f);
    renderer_draw_text(x, y, gold_text);
}

/**
 * Render the connection latency badge above the minimap.
 *
 * @param ping_ms  Round-trip latency in milliseconds, or a nonpositive value when unavailable.
 */
void hud_render_ping(const HUDLayout* hud, int ping_ms) {
    // Render ping badge just above the minimap in the upper-right
    char text[32];
    if (ping_ms <= 0)
        snprintf(text, sizeof(text), "Ping: --");
    else
        snprintf(text, sizeof(text), "Ping: %dms", ping_ms);

    // Color: green <=80ms, yellow <=150ms, red >150ms
    float r, g, b;
    if (ping_ms <= 0)       { r = 0.6f; g = 0.6f; b = 0.6f; }
    else if (ping_ms <= 80) { r = 0.2f; g = 0.9f; b = 0.3f; }
    else if (ping_ms <= 150){ r = 0.9f; g = 0.85f; b = 0.1f; }
    else                    { r = 0.9f; g = 0.2f;  b = 0.2f; }

    float w = 100.0f, h = 18.0f;
    float x = hud->minimap_x + hud->minimap_size - w;
    float y = hud->minimap_y - h - 4.0f;

    renderer_draw_rect(x - 4, y - 2, w + 8, h + 4, 0.05f, 0.05f, 0.08f, 0.82f);
    // colored left accent bar
    renderer_draw_rect(x - 4, y - 2, 3.0f, h + 4, r, g, b, 1.0f);

    // Use OpenGL color for the text (approximate by drawing colored rect behind it)
    // Since renderer_draw_text doesn't take a color, we draw a subtle tinted bg instead
    renderer_draw_text(x + 2, y + h - 3, text);
}

/** Map a protocol class identifier to its display name. */
static const char* session_class_name(uint8_t cls) {
    switch (cls) {
        case 1: return "Gladiator";
        case 2: return "Ninja";
        case 3: return "Landweaver";
        case 4: return "Spirit";
        default: return "Unknown";
    }
}

/** Map a protocol race identifier to its display name. */
static const char* session_race_name(uint8_t race) {
    switch (race) {
        case 1: return "Human";
        case 2: return "Pyseck";
        case 3: return "Infor";
        default: return "Unknown";
    }
}

// Layout constants shared between render and click-handler
#define SESSION_PANEL_W    450.0f
#define SESSION_ROW_H       16.0f
#define SESSION_HEADER_H    32.0f
#define SESSION_COL_H       18.0f
#define SESSION_FOOTER_H    30.0f
#define SESSION_PAD          8.0f
#define SESSION_ROWS        SESSION_LIST_PAGE_SIZE  // 30 rows per page

// Column X offsets (relative to panel left edge)
#define SESSION_COL_LV      SESSION_PAD
#define SESSION_COL_NAME    44.0f
#define SESSION_COL_CLASS  164.0f
#define SESSION_COL_RACE   264.0f
#define SESSION_COL_PING   (SESSION_PANEL_W - 54.0f)

// Total fixed panel height: header + col-header row + 30 rows + footer
#define SESSION_PANEL_H    (SESSION_HEADER_H + SESSION_COL_H + \
                            SESSION_ROWS * SESSION_ROW_H + SESSION_FOOTER_H)

// Prev/Next button dimensions (inside footer)
#define SESSION_BTN_W  60.0f
#define SESSION_BTN_H  18.0f

static float session_panel_x(const HUDLayout* hud) {
    return (float)hud->screen_width / 2.0f - SESSION_PANEL_W / 2.0f;
}
static float session_panel_y(void) { return 60.0f; }

/**
 * Render the paginated server session roster.
 *
 * @param own_ping_ms  Retained for the caller interface but currently unused.
 */
void hud_render_session_panel(const HUDLayout* hud, const GameState* game, int own_ping_ms) {
    // Each row carries its own ping from the server, so the viewer's own value
    // is not needed here. Kept in the signature for callers.
    (void)own_ping_ms;

    float px = session_panel_x(hud);
    float py = session_panel_y();

    // fall back to a flat panel until the texture loads
    if (game->textures.session_panel_bg) {
        renderer_draw_sprite(px, py, SESSION_PANEL_W, SESSION_PANEL_H,
                             game->textures.session_panel_bg);
    } else {
        renderer_draw_rect(px, py, SESSION_PANEL_W, SESSION_PANEL_H,
                           0.04f, 0.04f, 0.08f, 0.94f);
        // Border
        renderer_draw_rect(px, py, SESSION_PANEL_W, 1.5f, 0.40f, 0.50f, 0.65f, 1.0f);
        renderer_draw_rect(px, py + SESSION_PANEL_H - 1.5f, SESSION_PANEL_W, 1.5f, 0.40f, 0.50f, 0.65f, 1.0f);
        renderer_draw_rect(px, py, 1.5f, SESSION_PANEL_H, 0.40f, 0.50f, 0.65f, 1.0f);
        renderer_draw_rect(px + SESSION_PANEL_W - 1.5f, py, 1.5f, SESSION_PANEL_H, 0.40f, 0.50f, 0.65f, 1.0f);
    }

    // Header bar
    renderer_draw_rect(px, py, SESSION_PANEL_W, SESSION_HEADER_H, 0.08f, 0.12f, 0.20f, 0.95f);

    char header[64];
    snprintf(header, sizeof(header), "Players Online: %u",
             game->playing->session_total_players);
    renderer_draw_text(px + SESSION_PAD, py + SESSION_HEADER_H - 8, header);

    renderer_draw_text(px + SESSION_PANEL_W - 72.0f, py + SESSION_HEADER_H - 8, "[O] close");

    // Column headers
    float col_label_y = py + SESSION_HEADER_H + SESSION_COL_H - 4;
    renderer_draw_text(px + SESSION_COL_LV,   col_label_y, "Lv");
    renderer_draw_text(px + SESSION_COL_NAME,  col_label_y, "Name");
    renderer_draw_text(px + SESSION_COL_CLASS, col_label_y, "Class");
    renderer_draw_text(px + SESSION_COL_RACE,  col_label_y, "Race");
    renderer_draw_text(px + SESSION_COL_PING,  col_label_y, "Ping");

    // Thin separator under column headers
    float sep_y = py + SESSION_HEADER_H + SESSION_COL_H;
    renderer_draw_rect(px + SESSION_PAD, sep_y, SESSION_PANEL_W - SESSION_PAD * 2, 1.0f,
                       0.30f, 0.36f, 0.48f, 0.8f);

    // draw one optional background texture per player row
    float row_y = sep_y + 2.0f;

    for (int i = 0; i < game->playing->session_list_count; i++) {
        const SessionPlayer* p = &game->playing->session_list[i];

        // Entry background: texture if loaded, else alternating shaded rect
        if (game->textures.session_entry_bg) {
            renderer_draw_sprite(px + SESSION_PAD, row_y,
                                 SESSION_PANEL_W - SESSION_PAD * 2, SESSION_ROW_H - 1,
                                 game->textures.session_entry_bg);
        } else {
            // Self highlight (page 0, first entry is self)
            if (i == 0 && game->playing->session_current_page == 0) {
                renderer_draw_rect(px + 2, row_y, SESSION_PANEL_W - 4, SESSION_ROW_H - 1,
                                   0.08f, 0.18f, 0.10f, 0.70f);
            } else if (i % 2 == 1) {
                renderer_draw_rect(px + 2, row_y, SESSION_PANEL_W - 4, SESSION_ROW_H - 1,
                                   0.08f, 0.08f, 0.13f, 0.45f);
            }
        }

        float text_y = row_y + SESSION_ROW_H - 4;

        char lv_buf[8];
        snprintf(lv_buf, sizeof(lv_buf), "%u", (unsigned)p->level);
        renderer_draw_text(px + SESSION_COL_LV,    text_y, lv_buf);
        renderer_draw_text(px + SESSION_COL_NAME,  text_y, p->name);
        renderer_draw_text(px + SESSION_COL_CLASS, text_y, session_class_name(p->player_class));
        renderer_draw_text(px + SESSION_COL_RACE,  text_y, session_race_name(p->player_race));

        char ping_buf[16];
        if (p->ping_ms == 0) snprintf(ping_buf, sizeof(ping_buf), "--");
        else                 snprintf(ping_buf, sizeof(ping_buf), "%dms", (int)p->ping_ms);
        renderer_draw_text(px + SESSION_COL_PING, text_y, ping_buf);

        row_y += SESSION_ROW_H;
    }

    // pagination footer
    float footer_y = py + SESSION_PANEL_H - SESSION_FOOTER_H;
    renderer_draw_rect(px, footer_y, SESSION_PANEL_W, 1.0f, 0.30f, 0.36f, 0.48f, 0.6f);

    float btn_y = footer_y + (SESSION_FOOTER_H - SESSION_BTN_H) / 2.0f;

    // Prev button
    float prev_x = px + SESSION_PAD;
    int can_prev = game->playing->session_current_page > 0;
    renderer_draw_rect(prev_x, btn_y, SESSION_BTN_W, SESSION_BTN_H,
                       can_prev ? 0.15f : 0.08f,
                       can_prev ? 0.20f : 0.08f,
                       can_prev ? 0.30f : 0.08f, 0.9f);
    renderer_draw_text_centered(prev_x, btn_y, SESSION_BTN_W, SESSION_BTN_H, "< Prev");

    // Next button
    float next_x = px + SESSION_PANEL_W - SESSION_PAD - SESSION_BTN_W;
    int can_next = game->playing->session_total_pages > 0 &&
                   game->playing->session_current_page + 1 < game->playing->session_total_pages;
    renderer_draw_rect(next_x, btn_y, SESSION_BTN_W, SESSION_BTN_H,
                       can_next ? 0.15f : 0.08f,
                       can_next ? 0.20f : 0.08f,
                       can_next ? 0.30f : 0.08f, 0.9f);
    renderer_draw_text_centered(next_x, btn_y, SESSION_BTN_W, SESSION_BTN_H, "Next >");

    // Page indicator (centered)
    char page_buf[48];
    if (game->playing->session_total_pages > 0)
        snprintf(page_buf, sizeof(page_buf), "Page %u / %u",
                 (unsigned)game->playing->session_current_page + 1,
                 (unsigned)game->playing->session_total_pages);
    else
        snprintf(page_buf, sizeof(page_buf), "Loading...");
    renderer_draw_text_centered(prev_x + SESSION_BTN_W, btn_y,
                                next_x - prev_x - SESSION_BTN_W, SESSION_BTN_H,
                                page_buf);
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
 * Resolve a click against the session panel's pagination controls.
 *
 * @return      -1 for the previous page, 1 for the next page, or 0 for neither.
 */
int hud_session_panel_handle_click(const GameState* game, float mx, float my) {
    // Must mirror the footer layout in hud_render_session_panel
    extern void hud_init(HUDLayout*, int, int);  // just to get screen_width via game->playing->hud
    float px = (float)game->playing->hud.screen_width / 2.0f - SESSION_PANEL_W / 2.0f;
    float py = session_panel_y();

    float footer_y = py + SESSION_PANEL_H - SESSION_FOOTER_H;
    float btn_y    = footer_y + (SESSION_FOOTER_H - SESSION_BTN_H) / 2.0f;

    // Prev button bounds
    float prev_x = px + SESSION_PAD;
    if (mx >= prev_x && mx <= prev_x + SESSION_BTN_W &&
        my >= btn_y  && my <= btn_y + SESSION_BTN_H)
        return -1;

    // Next button bounds
    float next_x = px + SESSION_PANEL_W - SESSION_PAD - SESSION_BTN_W;
    if (mx >= next_x && mx <= next_x + SESSION_BTN_W &&
        my >= btn_y  && my <= btn_y + SESSION_BTN_H)
        return 1;

    return 0;
}
