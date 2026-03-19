#include "hud.h"
#include "renderer.h"
#include "core/game_types.h"
#include "world/npc.h"
#include "network/network.h"
#include <stdio.h>
#include <math.h>

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

int hud_check_inventory_button_clicked(const HUDLayout* hud, float mouse_x, float mouse_y) {
    float x = hud->inv_button_x;
    float y = hud->inv_button_y;
    float size = hud->inv_button_size;
    
    return (mouse_x >= x && mouse_x <= x + size &&
            mouse_y >= y && mouse_y <= y + size);
}

int hud_check_character_button_clicked(const HUDLayout* hud, float mouse_x, float mouse_y) {
    float x = hud->char_button_x;
    float y = hud->char_button_y;
    float size = hud->char_button_size;
    
    return (mouse_x >= x && mouse_x <= x + size &&
            mouse_y >= y && mouse_y <= y + size);
}

void hud_render(const HUDLayout* hud, const GameState* game) {
    if (!game->player.info_loaded) {
        return;
    }

    // Don't touch projection - just set up ortho for screen space
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, hud->screen_width, hud->screen_height, 0, -1, 1);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

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

    if (game->show_session_panel)
        hud_render_session_panel(hud, game, own_ping);
}

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
    for (int i = 0; i < game->visible_npc_count; i++) {
        const VisibleNPC* npc = &game->visible_npcs[i];
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
    for (int i = 0; i < game->nearby_player_count; i++) {
        const NearbyPlayer* p = &game->nearby_players[i];
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
}

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

void hud_render_mana_bar(const HUDLayout* hud, const GameState* game) {
    float x = hud->mana_bar_x;
    float y = hud->mana_bar_y;
    float width = hud->mana_bar_width;
    float height = hud->mana_bar_height;
    
    // Read from ability bar state instead of player info
    int32_t current_mana = game->ability_bar.mana;
    int32_t max_mana = game->ability_bar.max_mana;
    
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

void hud_render_exp_bar(const HUDLayout* hud, const GameState* game) {
    float x = hud->exp_bar_x;
    float y = hud->exp_bar_y;
    float width = hud->exp_bar_width;
    float height = hud->exp_bar_height;
    
    uint64_t current_exp = game->player.info.experience;
    uint64_t exp_for_next = game->player_xp_for_next;

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

void hud_render_target_bar(const GameState* game, float screen_width) {
    if (game->target_npc_id == 0) return;

    const VisibleNPC* npc = npc_find_by_id(game->visible_npcs, game->visible_npc_count,
                                            game->target_npc_id);
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

void hud_render_player_target_bar(const GameState* game, float screen_width) {
    if (game->target_player_id == 0) return;

    // Find the targeted player in the nearby list
    const NearbyPlayer* np = NULL;
    for (int i = 0; i < game->nearby_player_count; i++) {
        if (game->nearby_players[i].player_id == game->target_player_id) {
            np = &game->nearby_players[i];
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

void hud_render_party_frames(const GameState* game) {
    if (!game->party.has_party || game->party.member_count == 0) return;

    const float FRAME_W  = 180.0f;
    const float FRAME_H  = 50.0f;
    const float FRAME_GAP = 5.0f;
    const float BAR_PAD  = 6.0f;
    const float BAR_W    = FRAME_W - BAR_PAD * 2.0f;
    const float HP_H     = 10.0f;
    const float MP_H     = 8.0f;
    const float START_X  = 10.0f;
    const float START_Y  = 10.0f;

    for (int i = 0; i < game->party.member_count; i++) {
        const PartyMember* m = &game->party.members[i];

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
        int is_leader = (m->id == game->party.leader_id);
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

// Class name helper (mirrors server-side Class enum)
static const char* session_class_name(uint8_t cls) {
    switch (cls) {
        case 1: return "Gladiator";
        case 2: return "Ninja";
        case 3: return "Landweaver";
        case 4: return "Spirit";
        default: return "Unknown";
    }
}

void hud_render_session_panel(const HUDLayout* hud, const GameState* game, int own_ping_ms) {
    const float PANEL_W  = 340.0f;
    const float ROW_H    = 20.0f;
    const float HEADER_H = 24.0f;
    const float PAD      = 8.0f;

    // Total rows = self + nearby players (capped for display)
    int nearby = game->nearby_player_count;
    int total_rows = 1 + nearby;  // +1 for self

    float panel_h = HEADER_H + PAD + total_rows * ROW_H + PAD;

    // Position: center-right, below minimap area
    float px = (float)hud->screen_width / 2.0f - PANEL_W / 2.0f;
    float py = 80.0f;

    // Panel background + border
    renderer_draw_rect(px, py, PANEL_W, panel_h, 0.04f, 0.04f, 0.07f, 0.92f);
    renderer_draw_rect(px,              py,              PANEL_W, 1.5f, 0.40f, 0.50f, 0.65f, 1.0f);
    renderer_draw_rect(px,              py + panel_h - 1.5f, PANEL_W, 1.5f, 0.40f, 0.50f, 0.65f, 1.0f);
    renderer_draw_rect(px,              py,              1.5f, panel_h, 0.40f, 0.50f, 0.65f, 1.0f);
    renderer_draw_rect(px + PANEL_W - 1.5f, py,         1.5f, panel_h, 0.40f, 0.50f, 0.65f, 1.0f);

    // Header bar
    renderer_draw_rect(px, py, PANEL_W, HEADER_H, 0.10f, 0.14f, 0.22f, 1.0f);

    char header[64];
    snprintf(header, sizeof(header), "Session  (%d player%s)",
             total_rows, total_rows == 1 ? "" : "s");
    renderer_draw_text(px + PAD, py + HEADER_H - 5, header);

    // Column header hints (small, muted)
    renderer_draw_text(px + PAD,          py + HEADER_H + PAD - 2,  "Name");
    renderer_draw_text(px + 140.0f,       py + HEADER_H + PAD - 2,  "Class");
    renderer_draw_text(px + 230.0f,       py + HEADER_H + PAD - 2,  "HP");
    renderer_draw_text(px + PANEL_W - 60.0f, py + HEADER_H + PAD - 2, "Ping");

    // Separator under column headers
    float col_y = py + HEADER_H + PAD + ROW_H - 2;
    renderer_draw_rect(px + PAD, col_y, PANEL_W - PAD * 2, 1.0f, 0.30f, 0.35f, 0.45f, 0.8f);

    float row_y = col_y + 3.0f;

    // Helper: draw one row
    // Self row first
    {
        uint32_t max_hp = game->player.info.max_health;
        uint32_t cur_hp = game->player.info.health;
        float hp_pct = (max_hp > 0) ? (float)cur_hp / (float)max_hp : 1.0f;
        if (hp_pct > 1.0f) hp_pct = 1.0f;

        // Row bg (slightly lighter for self)
        renderer_draw_rect(px + 2, row_y, PANEL_W - 4, ROW_H - 1, 0.10f, 0.18f, 0.14f, 0.70f);

        char name_buf[40];
        snprintf(name_buf, sizeof(name_buf), "%s (you)", game->player.info.name);
        renderer_draw_text(px + PAD, row_y + ROW_H - 5, name_buf);

        renderer_draw_text(px + 140.0f, row_y + ROW_H - 5,
                           session_class_name(game->player.info.player_class));

        // HP bar (mini)
        float bar_x = px + 230.0f;
        float bar_w = 60.0f;
        float bar_h = 8.0f;
        float bar_by = row_y + (ROW_H - bar_h) / 2.0f;
        renderer_draw_rect(bar_x, bar_by, bar_w, bar_h, 0.15f, 0.05f, 0.05f, 1.0f);
        float hr = 1.0f, hg = hp_pct * 0.6f;
        renderer_draw_rect(bar_x, bar_by, bar_w * hp_pct, bar_h, hr, hg, 0.0f, 0.9f);

        // Ping
        char ping_buf[16];
        if (own_ping_ms <= 0) snprintf(ping_buf, sizeof(ping_buf), "--");
        else                  snprintf(ping_buf, sizeof(ping_buf), "%dms", own_ping_ms);
        renderer_draw_text(px + PANEL_W - 60.0f, row_y + ROW_H - 5, ping_buf);

        row_y += ROW_H;
    }

    // Nearby player rows
    for (int i = 0; i < nearby; i++) {
        const NearbyPlayer* p = &game->nearby_players[i];

        // Alternate row shading
        float bg_a = (i % 2 == 0) ? 0.0f : 0.05f;
        if (bg_a > 0.0f)
            renderer_draw_rect(px + 2, row_y, PANEL_W - 4, ROW_H - 1,
                               0.08f, 0.08f, 0.12f, bg_a);

        // Name (truncated to ~20 chars)
        char name_buf[24];
        snprintf(name_buf, sizeof(name_buf), "%.20s", p->name);
        renderer_draw_text(px + PAD, row_y + ROW_H - 5, name_buf);

        // Class
        renderer_draw_text(px + 140.0f, row_y + ROW_H - 5,
                           session_class_name(p->player_class));

        // HP bar
        float bar_x = px + 230.0f;
        float bar_w = 60.0f;
        float bar_h = 8.0f;
        float bar_by = row_y + (ROW_H - bar_h) / 2.0f;
        renderer_draw_rect(bar_x, bar_by, bar_w, bar_h, 0.15f, 0.05f, 0.05f, 1.0f);
        if (p->max_health > 0) {
            float hp_pct = (float)p->health / (float)p->max_health;
            if (hp_pct > 1.0f) hp_pct = 1.0f;
            if (hp_pct < 0.0f) hp_pct = 0.0f;
            float hr = 1.0f, hg = hp_pct * 0.6f;
            if (p->is_dead) { hr = 0.35f; hg = 0.35f; }
            renderer_draw_rect(bar_x, bar_by, bar_w * (p->is_dead ? 0.0f : hp_pct),
                               bar_h, hr, hg, 0.0f, 0.9f);
        }

        // Ping
        char ping_buf[16];
        if (p->ping_ms == 0) snprintf(ping_buf, sizeof(ping_buf), "--");
        else                 snprintf(ping_buf, sizeof(ping_buf), "%dms", (int)p->ping_ms);
        renderer_draw_text(px + PANEL_W - 60.0f, row_y + ROW_H - 5, ping_buf);

        row_y += ROW_H;
    }

    // Hint at bottom of panel header area
    char hint[32];
    snprintf(hint, sizeof(hint), "[O] close");
    renderer_draw_text(px + PANEL_W - 72.0f, py + HEADER_H - 5, hint);
}