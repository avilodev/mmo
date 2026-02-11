#ifndef HUD_H
#define HUD_H

#include "core/game_types.h"

// HUDLayout is already defined in game_types.h, just declare functions here

// Initialize HUD
void hud_init(HUDLayout* hud, int screen_width, int screen_height);

// Check if inventory button was clicked
int hud_check_inventory_button_clicked(const HUDLayout* hud, float mouse_x, float mouse_y);

// Check if character button was clicked
int hud_check_character_button_clicked(const HUDLayout* hud, float mouse_x, float mouse_y);

// Render HUD
void hud_render(const HUDLayout* hud, const GameState* game);

// Individual render functions
void hud_render_minimap(const HUDLayout* hud, const GameState* game);
void hud_render_health_bar(const HUDLayout* hud, const GameState* game);
void hud_render_mana_bar(const HUDLayout* hud, const GameState* game);
void hud_render_exp_bar(const HUDLayout* hud, const GameState* game);
void hud_render_level(const HUDLayout* hud, const GameState* game);
void hud_render_inventory_button(const HUDLayout* hud, const GameState* game);
void hud_render_character_button(const HUDLayout* hud, const GameState* game);
void hud_render_currencies(const HUDLayout* hud, const GameState* game);

#endif // HUD_H