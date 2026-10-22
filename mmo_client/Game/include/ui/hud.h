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
/** Draw the camera's view cone on a north-up map, centred on the player.
 *
 * The maps never rotate (D17); the cone is what says which way the camera
 * faces. Draws nothing in the top-down view, which always faces north.
 */
void hud_draw_view_cone(const Camera* camera, float center_x, float center_y, float radius);

void hud_render_minimap(const HUDLayout* hud, const GameState* game);
void hud_render_health_bar(const HUDLayout* hud, const GameState* game);
void hud_render_mana_bar(const HUDLayout* hud, const GameState* game);
void hud_render_exp_bar(const HUDLayout* hud, const GameState* game);
void hud_render_level(const HUDLayout* hud, const GameState* game);
void hud_render_inventory_button(const HUDLayout* hud, const GameState* game);
void hud_render_character_button(const HUDLayout* hud, const GameState* game);
void hud_render_currencies(const HUDLayout* hud, const GameState* game);

// Render target health bar at top-center of screen (NPC)
void hud_render_target_bar(const GameState* game, float screen_width);

// Render targeted player health bar (below NPC target bar)
void hud_render_player_target_bar(const GameState* game, float screen_width);

// Whether a click landed on the Inspect button beside the player target bar
int hud_check_inspect_button_clicked(const GameState* game, float screen_width,
		float mouse_x, float mouse_y);

// Render party member frames (top-left, stacked vertically)
void hud_render_party_frames(const GameState* game);

// Render the info bar in the upper right above the minimap: the weather and
// the time of day, then the ping (ms)
void hud_render_info_bar(const HUDLayout* hud, const GameState* game, int ping_ms);

// Whether a click landed on the weather and time in the info bar, which open the forecast
int hud_check_weather_clicked(const HUDLayout* hud, const GameState* game, float mouse_x,
		float mouse_y);

// Render the connection-issue icon at the top centre while the link is in trouble
void hud_render_connection_issue(const HUDLayout* hud, const GameState* game);

// Render the session panel (O menu — full server player list, paginated)
void hud_render_session_panel(const HUDLayout* hud, const GameState* game, int own_ping_ms);

// Carry the session panel by its title; nonzero while it is held.
int hud_session_panel_drag(GameState* game, float mx, float my, int clicked, int down);

// Handle a left-click against the session panel buttons.
// Returns -1 (prev page), 1 (next page), 2 (close), 0 (nothing hit).
int hud_session_panel_handle_click(const GameState* game, float mx, float my);

// Render FF14-style zone entry banner (fades in/out over 4 seconds).
void hud_render_zone_banner(const HUDLayout* hud, const GameState* game);

#endif // HUD_H