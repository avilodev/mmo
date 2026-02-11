#include "hud.h"
#include "renderer.h"
#include "core/game_types.h"
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
    
    // Render all HUD elements
    hud_render_minimap(hud, game);
    hud_render_health_bar(hud, game);
    hud_render_mana_bar(hud, game);
    hud_render_exp_bar(hud, game);
    hud_render_level(hud, game);
    hud_render_inventory_button(hud, game);
    hud_render_character_button(hud, game);
    hud_render_currencies(hud, game);
}

void hud_render_minimap(const HUDLayout* hud, const GameState* game) {
    float x = hud->minimap_x;
    float y = hud->minimap_y;
    float size = hud->minimap_size;
    
    // Background
    renderer_draw_rect(x, y, size, size, 0.1f, 0.1f, 0.15f, 0.9f);
    
    // Border
    renderer_draw_rect(x, y, size, 2, 0.6f, 0.6f, 0.6f, 1.0f);
    renderer_draw_rect(x, y + size - 2, size, 2, 0.6f, 0.6f, 0.6f, 1.0f);
    renderer_draw_rect(x, y, 2, size, 0.6f, 0.6f, 0.6f, 1.0f);
    renderer_draw_rect(x + size - 2, y, 2, size, 0.6f, 0.6f, 0.6f, 1.0f);
    
    // Player dot (center)
    float center_x = x + size / 2.0f;
    float center_y = y + size / 2.0f;
    renderer_draw_rect(center_x - 3, center_y - 3, 6, 6, 0.2f, 1.0f, 0.2f, 1.0f);
    
    // Coordinates below minimap
    char coords[64];
    snprintf(coords, sizeof(coords), "X: %.0f  Y: %.0f", game->player.x, game->player.y);
    renderer_draw_text(x + 10, y + size + 5, coords);
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
    
    // For now, use a placeholder exp calculation
    uint64_t current_exp = game->player.info.experience;
    uint64_t level = game->player.info.level;
    
    // Simple exp calculation: each level needs level * 1000 exp
    uint64_t exp_for_level = level * 1000;
    uint64_t exp_in_level = current_exp % exp_for_level;
    
    float exp_percent = (float)exp_in_level / (float)exp_for_level;
    if (exp_percent > 1.0f) exp_percent = 1.0f;
    
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
             (unsigned long long)exp_in_level, (unsigned long long)exp_for_level);
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

void hud_render_currencies(const HUDLayout* hud, const GameState* game) {
    float x = hud->currency_x;
    float y = hud->currency_y;
    float spacing = hud->currency_spacing;
    
    // Gold
    char gold_text[64];
    snprintf(gold_text, sizeof(gold_text), "Gold: %u", game->player.info.gold);
    renderer_draw_rect(x - 5, y - 15, 180, 25, 0.1f, 0.1f, 0.1f, 0.8f);
    renderer_draw_text(x, y, gold_text);
    
    // Currency 1 (placeholder)
    char curr1_text[64];
    snprintf(curr1_text, sizeof(curr1_text), "Gems: 0");
    renderer_draw_rect(x - 5, y + spacing - 15, 180, 25, 0.1f, 0.1f, 0.1f, 0.8f);
    renderer_draw_text(x, y + spacing, curr1_text);
    
    // Currency 2 (placeholder)
    char curr2_text[64];
    snprintf(curr2_text, sizeof(curr2_text), "Tokens: 0");
    renderer_draw_rect(x - 5, y + spacing * 2 - 15, 180, 25, 0.1f, 0.1f, 0.1f, 0.8f);
    renderer_draw_text(x, y + spacing * 2, curr2_text);
    
    // Currency 3 (placeholder)
    char curr3_text[64];
    snprintf(curr3_text, sizeof(curr3_text), "Credits: 0");
    renderer_draw_rect(x - 5, y + spacing * 3 - 15, 180, 25, 0.1f, 0.1f, 0.1f, 0.8f);
    renderer_draw_text(x, y + spacing * 3, curr3_text);
}