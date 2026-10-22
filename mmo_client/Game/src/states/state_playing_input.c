/**
 * @file
 * Route gameplay, panel, chat, targeting, movement, and combat input.
 *
 * The one unit that reads the keyboard and the mouse, and the only one that
 * turns either into a request to the server. It is long because input is a
 * flat list of cases and the flatness is the readable form; what it is not is
 * mixed with the drawing it used to sit beside.
 */
#include "states/state_playing_internal.h"
#include "game.h"
#include "input.h"
#include "network.h"
#include "npc.h"
#include "player.h"
#include "hud.h"
#include "inventory.h"
#include "character_screen.h"
#include "ui/npc_dialogue.h"
#include "ui/shop_ui.h"
#include "ui/settings_menu.h"
#include "ui/pause_menu.h"
#include "ui/friends_panel.h"
#include "ui/map_view.h"
#include "states/world_loader.h"
#include "core/keybinds.h"
#include "states/chat_commands.h"
#include "states/chat_channels.h"
#include "states/chat_input.h"
#include "states/emote_client.h"
#include "states/inspect_client.h"
#include "player/emote.h" 
#include "player/emote_slots.h"
#include "ui/emote_hub.h"
#include "ui/weather_window.h"
#include "camera/camera_tuning.h"
#include "render/character_tuning.h"
#include "world/world_pick.h"
#include "world_spans.h"

#include <string.h>
#include "core/client_log.h"

static float deg_to_rad(float deg) { return deg * 3.14159265359f / 180.0f; }

// The living NPC whose body is under the cursor, or NULL
static VisibleNPC* npc_under_cursor(GameState* game) {
	for(int i = 0; i < game->playing->visible_npc_count; i++) {
		VisibleNPC* npc = &game->playing->visible_npcs[i];

		if(!npc->is_alive)
			continue;
		if(camera_hit_standing(&game->camera, npc->pos_x, npc->pos_y, npc->pos_z,
		                       npc_body_height(npc), CHARACTER_PICK_RADIUS,
		                       game->input.mouse_x, game->input.mouse_y))
			return npc;
	}

	return NULL;
}

// A click over an open window or a dialogue belongs to it, not the world
static int cursor_over_panel(const GameState* game) {
	const InventoryState* inv = game->inventory;
	const CharacterScreenState* cs = game->character_screen;

	if(dialogue_is_active())
		return 1;
	if(inv && inv->is_open &&
	   input_mouse_in_rect(&game->input, inv->window_x, inv->window_y,
	                       inv->window_width, inv->window_height))
		return 1;
	if(cs && cs->is_open &&
	   input_mouse_in_rect(&game->input, cs->window_x, cs->window_y,
	                       cs->window_width, cs->window_height))
		return 1;

	return 0;
}

/** Whether a key that walks the body is held. */
static int movement_key_down(const GameState* game) {
	return input_key_pressed(&game->input, g_keybinds.move_up)    ||
	       input_key_pressed(&game->input, g_keybinds.move_down)  ||
	       input_key_pressed(&game->input, g_keybinds.move_left)  ||
	       input_key_pressed(&game->input, g_keybinds.move_right);
}

/**
 * The point under the cursor: on the surface the spans give (a ramp, a bridge),
 * else on the ground plane. Only (x, y) goes to the server, which snaps z itself.
 */
static void pick_cursor_point(const GameState* game, float* out_x, float* out_y) {
	float origin[3], dir[3], hit[3];

	if(world_spans_loaded()) {
		camera_screen_ray(&game->camera, game->input.mouse_x, game->input.mouse_y, origin, dir);
		if(world_pick_ground(origin, dir, 4000.0f, hit)) {
			*out_x = hit[0];
			*out_y = hit[1];
			return;
		}
	}
	camera_screen_to_world(&game->camera, game->input.mouse_x, game->input.mouse_y,
	                       out_x, out_y);
}

/**
 * Turn and reset the camera (D8, D10).
 *
 * Called after the chat early-out, so Q and E type into chat rather than spin
 * the view, and before the death early-out, so a dead player can still look
 * around.
 */
static void playing_input_camera(GameState* game, float delta_time) {
    Camera* camera = &game->camera;

    if (input_key_just_pressed(&game->input, g_keybinds.camera_reset))
        camera_reset_view(camera);

    float step = deg_to_rad(CAMERA_ROTATE_DEG_PER_SEC) * delta_time;
    if (input_key_pressed(&game->input, g_keybinds.camera_rotate_left))  
        camera_rotate(camera, -step);
    if (input_key_pressed(&game->input, g_keybinds.camera_rotate_right)) 
        camera_rotate(camera,  step);

    /* Dragging with the middle button held orbits the camera, FF14-style:
     * the view follows the mouse. Drag right and the view turns right (the
     * eye swings left round the player, the way Q turns it); drag up and it
     * looks up (the eye drops). The right button is the basic attack. */
    int dragging = game->input.mouse_middle_down;
    if (dragging && game->input.mouse_dx != 0.0f)
        camera_rotate(camera, -deg_to_rad(CAMERA_DRAG_DEG_PER_PIXEL) * game->input.mouse_dx);
    if (dragging && game->input.mouse_dy != 0.0f)
        camera_pitch_by(camera, deg_to_rad(CAMERA_DRAG_PITCH_DEG_PER_PIXEL) * game->input.mouse_dy);
}

/**
 * Send whatever the friends panel queued this frame.
 *
 * The panel hit-tests and draws; it does not know what a socket is, the same
 * split the quest log uses for abandoning a quest. This is the one place that
 * turns a click in it into a packet.
 */
static void friends_drain_pending(GameState* game) {
    char name[32];
    switch (friends_panel_take_pending(&game->playing->friends, name, sizeof(name))) {
        case FRIEND_PENDING_ACCEPT:  
            network_send_friend_respond(name, 1); 
            break;
        case FRIEND_PENDING_DECLINE: 
            network_send_friend_respond(name, 0); 
            break;
        case FRIEND_PENDING_REMOVE:  
            network_send_friend_remove(name);     
            break;
        case FRIEND_PENDING_REFRESH: 
            network_send_friend_list_request();   
            break;
        case FRIEND_PENDING_NONE:
        default:                                                  
            break;
    }
}

// Carry the chat box about by its outline; returns 1 while it is held.
static int chat_drag(GameState* game) {
	ChatState* chat = &game->playing->chat;
	chat_box box = chat_box_at(&chat->win, (float)game->camera.viewport_width,
			(float)game->camera.viewport_height);
	float x, y, w, h;
	int on_outline;

	chat_window_rect(&box, &x, &y, &w, &h);
	on_outline = game->input.mouse_left_clicked &&
			window_pos_on_edge(x, y, w, h, CHAT_EDGE_BAND, game->input.mouse_x,
			game->input.mouse_y);

	return window_pos_carry(&chat->win, x, y, (float)game->camera.viewport_width,
			(float)game->camera.viewport_height, w, h, on_outline, game->input.mouse_x,
			game->input.mouse_y, game->input.mouse_left_down);
}

// A click on the chat box opens it; a click on a message whispers its sender.
static void chat_click(GameState* game) {
	ChatState* chat = &game->playing->chat;
	chat_box box = chat_box_at(&chat->win, (float)game->camera.viewport_width,
			(float)game->camera.viewport_height);
	chat_visible visible[CHAT_LINES];
	float mx = game->input.mouse_x;
	float my = game->input.mouse_y;
	int count;
	int line;

	if(mx < box.x || mx > box.x + box.w)
		return;

	if(my >= box.input_y && my <= box.input_y + box.input_h) {
		chat_open(chat);
		return;
	}

	if(my < box.y || my >= box.input_y)
		return;

	count = chat_layout_visible(chat, playing_chat_measure, visible);
	line = chat_hit_line(visible, count, &box, my);
	if(line >= 0 && chat_line_replyable(&chat->lines[line]))
		chat_begin_whisper(chat, chat->lines[line].sender);
}

// Sends what was typed: a command, or a message to the active channel.
static void chat_send_typed(GameState* game) {
	ChatState* chat = &game->playing->chat;
	chat_outgoing out;

	chat_history_add(chat, chat->input_buf);

	// A slash line is a command, or a reported typo: never chat
	if(!chat_command_dispatch(game, chat->input_buf) &&
	   chat_input_submit(chat, &out))
		network_send_chat(out.channel, out.target, out.text);

	chat_close(chat);
}

// The keys that work while the chat input has the keyboard.
static void chat_typing_keys(ChatState* chat, const InputState* input,
		float delta_time) {
	if(input_key_just_pressed(input, GLFW_KEY_ESCAPE)) {
		chat_close(chat);
		return;
	}

	// Backspace uses the held state so it repeats
	chat_backspace_update(chat, input_key_just_pressed(input, GLFW_KEY_BACKSPACE),
			input_key_pressed(input, GLFW_KEY_BACKSPACE), delta_time);

	if(input_key_just_pressed(input, g_keybinds.toggle_channel))
		chat_cycle_channel(chat);
	if(input_key_just_pressed(input, GLFW_KEY_UP))
		chat_history_step(chat, 1);
	if(input_key_just_pressed(input, GLFW_KEY_DOWN))
		chat_history_step(chat, -1);
	if(input_key_just_pressed(input, GLFW_KEY_PAGE_UP))
		chat_scroll_by(chat, CHAT_LINES - 1);
	if(input_key_just_pressed(input, GLFW_KEY_PAGE_DOWN))
		chat_scroll_by(chat, -(CHAT_LINES - 1));
}

// One frame of chat input; returns 1 while chat holds the keyboard.
static int chat_input_frame(GameState* game, float delta_time) {
	ChatState* chat = &game->playing->chat;
	const InputState* input = &game->input;

	if(!chat->is_typing && input_key_just_pressed(input, g_keybinds.toggle_chat))
		chat_open(chat);

	// The slash char event arrives before chat is open, so type it here
	if(!chat->is_typing && input_key_just_pressed(input, GLFW_KEY_SLASH)) {
		chat_open(chat);
		chat_type(chat, '/');
	}

	// The grip takes the mouse while the box is being moved
	if(chat_drag(game))
		return 1;

	if(!chat->is_typing && input->mouse_left_clicked)
		chat_click(game);

	if(input_key_just_pressed(input, GLFW_KEY_ENTER)) {
		if(chat->is_typing)
			chat_send_typed(game);
		else
			chat_open(chat);
	}

	if(!chat->is_typing)
		return 0;

	chat_typing_keys(chat, input, delta_time);

	return 1;
}

/**
 * Route gameplay, panel, chat, targeting, movement, and combat input.
 *
 * @param delta_time  Elapsed frame time in seconds for chat key repeat.
 */
void playing_input(GameState* game, GLFWwindow* window, float delta_time) {
	// While the world loads the only input is the way out of a failed load
	if(world_loader_active()) {
		if(world_loader_failed() && input_key_just_pressed(&game->input, GLFW_KEY_ESCAPE))
			game_change_state(game, GAME_MODE_MAIN_MENU);

		return;
	}

	if(game->playing->is_paused) {
		switch(pm_update(game)) {
		case PM_RESUME:
			game->playing->is_paused = 0;
			break;
		case PM_SETTINGS:
			game->playing->is_paused = 0;
			game->show_settings = 1;
			sm_open(&game->settings, SF_TAB_GAMEPLAY);
			break;
		case PM_QUIT:
			game->playing->is_paused = 0;
			game_change_state(game, GAME_MODE_MAIN_MENU);
			break;
		default:
			break;
		}

		return; // Block all gameplay input while paused
	}

	// Settings menu: closing it returns to the pause menu
	if(game->show_settings) {
		if(sm_update(game)) {
			game->show_settings = 0;
			game->playing->is_paused = 1;
			pm_open();
		}

		return;
	}

	// The emote hub takes this frame's keys and clicks while it is open
	if(emote_hub_is_open())
		emote_hub_update(game);

	// So does the weather window
	if(weather_window_is_open())
		weather_window_update(game);

    // update hovered buff before gameplay input
    {
        float eff_x = game->playing->ability_bar.bar_x;
        float eff_y = game->playing->ability_bar.bar_y - 40.0f;
        float icon_size = 28.0f, icon_pad = 4.0f;
        float mx = game->input.mouse_x, my = game->input.mouse_y;
        game->playing->hovered_effect = -1;
        int drawn = 0;
        for (int i = 0; i < MAX_CLIENT_EFFECTS; i++) {
            if (!game->playing->ability_bar.effects[i].active) continue;
            float ix = eff_x + drawn * (icon_size + icon_pad);
            if (mx >= ix && mx <= ix + icon_size &&
                my >= eff_y && my <= eff_y + icon_size) {
                game->playing->hovered_effect = i;
                break;
            }
            drawn++;
        }
    }

    // Quest log
    quest_log_handle_input_full(&game->playing->quest_log,
                                 game->input.mouse_x, game->input.mouse_y,
                                 game->input.mouse_left_clicked, game->input.mouse_left_down,
                                 !game->playing->chat.is_typing && input_key_just_pressed(&game->input, g_keybinds.toggle_quest_log),
                                 !game->playing->chat.is_typing && input_key_just_pressed(&game->input, GLFW_KEY_ESCAPE),
                                 game->camera.viewport_width,
                                 game->camera.viewport_height);
    /* Drained here rather than sent from the panel: the quest log draws and
     * hit-tests, and knows nothing about sockets. */
    uint32_t abandon_id = quest_log_take_abandon_request(&game->playing->quest_log);
    if (abandon_id != 0) network_send_quest_abandon(abandon_id);

    if (game->playing->quest_log.is_open) return; // Block gameplay input while quest log open


    // Shop window input (blocks gameplay input while open)
    if (game->playing->shop.is_open) {
        shop_ui_handle_input(game, game->input.mouse_x, game->input.mouse_y,
                             game->input.mouse_left_clicked);
        return;
    }

    // Currency panel — read-only, and blocks gameplay input while open
    if (currency_panel_handle_input(&game->playing->currency_panel,
                                    game->input.mouse_x, game->input.mouse_y,
                                    game->input.mouse_left_clicked, game->input.mouse_left_down,
                                    !game->playing->chat.is_typing &&
                                        input_key_just_pressed(&game->input, g_keybinds.toggle_currency),
                                    !game->playing->chat.is_typing &&
                                        input_key_just_pressed(&game->input, GLFW_KEY_ESCAPE),
                                    game->camera.viewport_width,
                                    game->camera.viewport_height))
        return;
    if (game->playing->currency_panel.is_open) return;

    /* Friends panel. Same shape as the currency panel: it consumes the click
     * when it is open, and gameplay never sees it. */
    if (friends_panel_handle_input(&game->playing->friends,
                                   game->input.mouse_x, game->input.mouse_y,
                                   game->input.mouse_left_clicked, game->input.mouse_left_down,
                                   !game->playing->chat.is_typing &&
                                       input_key_just_pressed(&game->input, g_keybinds.toggle_friends),
                                   !game->playing->chat.is_typing &&
                                       input_key_just_pressed(&game->input, GLFW_KEY_ESCAPE),
                                   !game->playing->chat.is_typing &&
                                       input_key_just_pressed(&game->input, GLFW_KEY_PAGE_UP),
                                   !game->playing->chat.is_typing &&
                                       input_key_just_pressed(&game->input, GLFW_KEY_PAGE_DOWN),
                                   game->camera.viewport_width,
                                   game->camera.viewport_height)) {
        friends_drain_pending(game);
        return;
    }
    friends_drain_pending(game);
    if (game->playing->friends.is_open) return;

    // Toggle inventory
    if (!game->playing->chat.is_typing && input_key_just_pressed(&game->input, g_keybinds.toggle_inventory)) {
        if (game->inventory) {
            inventory_toggle(game->inventory);
        }
    }

    // Toggle character screen
    if (!game->playing->chat.is_typing && input_key_just_pressed(&game->input, g_keybinds.toggle_character)) {
        if (game->character_screen) {
            character_screen_toggle(game->character_screen);
        }
    }

	// Inspect the targeted player
	if(!game->playing->chat.is_typing &&
	   input_key_just_pressed(&game->input, g_keybinds.inspect_target))
		inspect_client_request_target(game);

    // Toggle session panel
    if (!game->playing->chat.is_typing && input_key_just_pressed(&game->input, g_keybinds.toggle_session_panel)) {
        game->playing->show_session_panel = !game->playing->show_session_panel;
        if (game->playing->show_session_panel) {
            game->playing->session_current_page = 0;
            network_send_session_list_request(0);
        }
    }

    // The session panel is carried by its title; nothing else sees the mouse meanwhile
    if (game->playing->show_session_panel &&
        hud_session_panel_drag(game, game->input.mouse_x, game->input.mouse_y,
                               game->input.mouse_left_clicked, game->input.mouse_left_down))
        return;

    // Session panel pagination clicks
    if (game->playing->show_session_panel && game->input.mouse_left_clicked) {
        int dir = hud_session_panel_handle_click(game, game->input.mouse_x, game->input.mouse_y);
        if (dir == 2) {
            game->playing->show_session_panel = 0;
        } else if (dir == -1 && game->playing->session_current_page > 0) {
            uint16_t next = game->playing->session_current_page - 1;
            game->playing->session_current_page = next;
            network_send_session_list_request(next);
        } else if (dir == 1 && game->playing->session_current_page + 1 < game->playing->session_total_pages) {
            uint16_t next = game->playing->session_current_page + 1;
            game->playing->session_current_page = next;
            network_send_session_list_request(next);
        }
    }

    // Handle dialogue option clicks (highest priority)
    if (dialogue_is_active() && game->input.mouse_left_clicked) {
        uint8_t option_id = 0;
        if (dialogue_handle_click(game->input.mouse_x, game->input.mouse_y, &option_id)) {
            /* The identifier, not the row: the server hid the options this
             * character fails, so row numbers are not a shared vocabulary. */
            network_send_dialogue_option_select(dialogue_get_current_npc(),
                                                dialogue_get_current_dialogue_id(),
                                                dialogue_get_current_page(),
                                                option_id);
            // Don't close dialogue here - wait for server response (update or close)
            return;  // Don't process other clicks
        }
    }

    // Check close button clicks (priority over other interactions)
    if (game->input.mouse_left_clicked) {
        // Check inventory close button
        if (game->inventory && game->inventory->is_open) {
            if (inventory_check_close_button(game->inventory,
                                            game->input.mouse_x,
                                            game->input.mouse_y)) {
                inventory_toggle(game->inventory);
                return;  // Don't process other clicks
            }
        }

        // Check character screen close button
        if (game->character_screen && game->character_screen->is_open) {
            if (character_screen_check_close_button(game->character_screen,
                                                   game->input.mouse_x,
                                                   game->input.mouse_y)) {
                character_screen_toggle(game->character_screen);
                return;  // Don't process other clicks
            }
        }

		// Check inspect window close button
		if(game->inspect_screen && game->inspect_screen->is_open &&
		   character_screen_check_close_button(game->inspect_screen, game->input.mouse_x,
		                                       game->input.mouse_y)) {
			inspect_client_close(game);
			return; // Don't process other clicks
		}

		// Check the weather and time in the info bar
		if(!cursor_over_panel(game) &&
		   hud_check_weather_clicked(&game->playing->hud, game, game->input.mouse_x,
		                             game->input.mouse_y)) {
			weather_window_open();
			return; // Don't process other clicks
		}

		// Check the Inspect button on the player target bar
		if(hud_check_inspect_button_clicked(game, (float)game->camera.viewport_width,
		                                    game->input.mouse_x, game->input.mouse_y)) {
			inspect_client_request_target(game);
			return; // Don't process other clicks
		}

        // Check inventory button
        if (game->inventory) {
            if (hud_check_inventory_button_clicked(&game->playing->hud,
                                                   game->input.mouse_x,
                                                   game->input.mouse_y)) {
                inventory_toggle(game->inventory);
            }
        }

        // Check character button
        if (game->character_screen) {
            if (hud_check_character_button_clicked(&game->playing->hud,
                                                   game->input.mouse_x,
                                                   game->input.mouse_y)) {
                character_screen_toggle(game->character_screen);
            }
        }
    }

	// Chat: open it, click it, send, and type into it
	if(chat_input_frame(game, delta_time))
		return; // Don't process other input while typing

    playing_input_camera(game, delta_time);

    // Block gameplay input while dead (server auto-respawns)
    if (game->playing->is_dead) 
        return;

    // Party invite accept/decline clicks
    if (game->playing->party.has_pending_invite && game->input.mouse_left_clicked) {
        float w = 300.0f;
        float x = ((float)game->camera.viewport_width - w) / 2.0f;
        float y = 200.0f;
        float mx = game->input.mouse_x;
        float my = game->input.mouse_y;

        // Accept button: (x+30, y+50) to (x+110, y+74)
        if (mx >= x+30 && mx <= x+110 && my >= y+50 && my <= y+74) {
            network_send_party_accept();
            game->playing->party.has_pending_invite = 0;
            return;
        }
        // Decline button: (x+190, y+50) to (x+270, y+74)
        if (mx >= x+190 && mx <= x+270 && my >= y+50 && my <= y+74) {
            network_send_party_decline();
            game->playing->party.has_pending_invite = 0;
            return;
        }
    }

    // While map is open, eat all gameplay input except M and ESC
    if (game->playing->show_map) 
        goto map_input_only;

    // Party frame click = target that member
    if (game->input.mouse_left_clicked && game->playing->party.has_party) {
        const float FRAME_W  = 180.0f;
        const float FRAME_H  = 50.0f;
        const float FRAME_GAP = 5.0f;
        const float START_X  = 10.0f;
        const float START_Y  = 10.0f;
        for (int i = 0; i < game->playing->party.member_count; i++) {
            float fx = START_X;
            float fy = START_Y + i * (FRAME_H + FRAME_GAP);
            if (game->input.mouse_x >= fx && game->input.mouse_x <= fx + FRAME_W &&
                game->input.mouse_y >= fy && game->input.mouse_y <= fy + FRAME_H) {
                game->playing->target_player_id = game->playing->party.members[i].id;
                game->playing->target_npc_id    = 0;
                goto done_click; // Skip world-coord click
            }
        }
    }

    // Left click: player/NPC targeting in world, then ground item pickup
    if (game->input.mouse_left_clicked) {
        /* The ground under the cursor, for item pickup; entities are hit on
         * their bodies, which in 3D stand above that point. */
        float world_x, world_y;
        pick_cursor_point(game, &world_x, &world_y);
        const float mx = game->input.mouse_x, my = game->input.mouse_y;

        // Check if clicked on a nearby player to target them
        int hit_target = 0;
        for (int i = 0; i < game->playing->nearby_player_count; i++) {
            const NearbyPlayer* np = &game->playing->nearby_players[i];
            if (np->is_dead) 
                continue;
            
            if (camera_hit_standing(&game->camera, np->pos_x, np->pos_y, np->pos_z,
                                    CHARACTER_HEIGHT, CHARACTER_PICK_RADIUS, mx, my)) {
                game->playing->target_player_id = np->player_id;
                game->playing->target_npc_id    = 0;  // Clear NPC target
                hit_target = 1;
                break;
            }
        }

        // Check if clicked on an NPC to target it
        if (!hit_target) {
            for (int i = 0; i < game->playing->visible_npc_count; i++) {
                VisibleNPC* npc = &game->playing->visible_npcs[i];
                if (!npc->is_alive) continue;
                if (camera_hit_standing(&game->camera, npc->pos_x, npc->pos_y, npc->pos_z,
                                        npc_body_height(npc), CHARACTER_PICK_RADIUS, mx, my)) {
                    game->playing->target_npc_id    = npc->npc_id;
                    game->playing->target_player_id = 0;  // Clear player target
                    hit_target = 1;
                    break;
                }
            }
        }

        // Click on empty space clears all targets
        if (!hit_target) {
            game->playing->target_npc_id    = 0;
            game->playing->target_player_id = 0;
        }

        // Ground item pickup
        for (int i = 0; i < MAX_GROUND_ITEMS; i++) {
            if (!game->playing->ground_items[i].active) continue;
            GroundItem* item = &game->playing->ground_items[i];
            float dx = world_x - item->pos_x;
            float dy = world_y - item->pos_y;
            if (dx*dx + dy*dy < 20.0f * 20.0f) {
                float pdx = game->player.x - item->pos_x;
                float pdy = game->player.y - item->pos_y;
                if (pdx*pdx + pdy*pdy < 80.0f * 80.0f) {
                    network_send_loot_pickup(item->ground_item_id);
                    break;
                }
            }
        }
    }
	// Interact (left click): talk to the friendly NPC under the cursor
	if(input_key_just_pressed(&game->input, g_keybinds.interact) && !cursor_over_panel(game)) {
		VisibleNPC* npc = npc_under_cursor(game);

		if(npc && npc->category != 1 && npc->is_interactable) {
			CLOG_DEBUG("[INPUT] Interacting with NPC %u at (%.1f, %.1f)",
			           npc->npc_id, npc->pos_x, npc->pos_y);
			network_send_npc_interact_request(npc->npc_id);
		}
	}
    done_click:;

    // Leave party
    if (input_key_just_pressed(&game->input, g_keybinds.party_leave) && game->playing->party.has_party) {
        network_send_party_leave();
    }

	// The hub key, and Emotes 1 to 3
	if(!game->playing->chat.is_typing) {
		if(input_key_just_pressed(&game->input, g_keybinds.toggle_emote_hub))
			emote_hub_open();
		if(input_key_just_pressed(&game->input, g_keybinds.toggle_weather))
			weather_window_open();
		for(int n = 0; n < EMOTE_SLOT_KEYED; n++)
			if(input_key_just_pressed(&game->input, g_keybinds.emote[n]))
				emote_client_request_slot(game, EMOTE_SLOT_EMOTE_1 + n);
	}

	// A movement key ends an emote; a seat or an open dialogue pins the body
	InputState no_keys;
	const InputState* keys = &game->input;
	if(emote_holds_body(&game->player.emote) && movement_key_down(game))
		emote_client_stop(game);
	if(emote_pins_body(&game->player.emote) || dialogue_is_active()) {
		memset(&no_keys, 0, sizeof(no_keys));
		keys = &no_keys;
	}
	game->player.sprint_toggle = game->settings.sprint_toggle;
	player_update_movement(&game->player, keys, &game->world, delta_time,
	                       game->camera.yaw);
	game->playing->move_sync.jumped |= player_update_vertical(&game->player, &game->world,
	                                                          delta_time);
	game->playing->move_sync.vertical_ran = 1;

    /* Form swap. The request is fire-and-forget: the server answers with the
     * authoritative form either way, and refuses while the shared swap cooldown is
     * up or while an action lock is in force. */
    if (input_key_just_pressed(&game->input, g_keybinds.swap_form)) {
        uint8_t current = game->playing->ability_bar.active_form;
        network_request_form_swap(current == FORM_ANIMAL ? FORM_HUMAN : FORM_ANIMAL);
    }

    FormSwapAckPacket swap_ack;
    if (network_get_form_swap_ack(&swap_ack)) {
        if (swap_ack.accepted) {
            ability_bar_set_form(&game->playing->ability_bar, swap_ack.form,
                                 (int32_t)ntohl(swap_ack.resource),
                                 (int32_t)ntohl(swap_ack.max_resource),
                                 swap_ack.resource_type, swap_ack.swap_ready_in,
                                 swap_ack.ability_ready_in);
            game->playing->player_form          = swap_ack.form;
            game->playing->player_resource_type = swap_ack.resource_type;
            CLOG_DEBUG("[FORM] Now in %s form",
                   swap_ack.form == FORM_ANIMAL ? "animal" : "human");
        } else {
            /* A refusal still carries the truth: keep the client in step with it. */
            game->playing->ability_bar.active_form = swap_ack.form;
            game->playing->form_swap_ready_in = swap_ack.swap_ready_in;
        }
    }

    // Ability input (keys 1-5), against whichever form's bar is live
    uint16_t ability_to_cast = ability_bar_update(&game->playing->ability_bar, delta_time,
                                           game->input.keys_just_pressed,
                                           game->player.info.race_id);

    if (ability_to_cast > 0) {
        /* Aim at the ground under the cursor, whichever way the camera faces. */
        float aim_x, aim_y;
        pick_cursor_point(game, &aim_x, &aim_y);

        network_send_ability_cast(ability_to_cast, aim_x, aim_y, 0);
    }

	// Right click while casting cancels the cast instead of attacking
	int cancelled = 0;

	if(game->input.mouse_right_clicked && game->playing->ability_bar.is_casting) {
		network_send_ability_cancel();
		ability_bar_on_cast_cancel(&game->playing->ability_bar,
		                           game->playing->ability_bar.casting_ability_id);
		cancelled = 1;
	}

	// Basic attack (right click): a hostile under the cursor becomes the target
	if(!cancelled && input_key_just_pressed(&game->input, g_keybinds.basic_attack) &&
	   !cursor_over_panel(game)) {
		VisibleNPC* clicked = npc_under_cursor(game);

		if(clicked && clicked->category == 1) {
			game->playing->target_npc_id = clicked->npc_id;
			game->playing->target_player_id = 0;
		}
        float aim_x = game->player.x;
        float aim_y = game->player.y;
        if (game->playing->target_npc_id != 0) {
            float tx, ty;
            if (npc_get_position(game->playing->visible_npcs, game->playing->visible_npc_count,
                                 game->playing->target_npc_id, &tx, &ty)) {
                aim_x = tx;
                aim_y = ty;
                network_update_facing_direction(tx - game->player.x, ty - game->player.y);
            } else {
                network_update_facing_direction(game->player.vel_x, game->player.vel_y);
            }
        } else {
            network_update_facing_direction(game->player.vel_x, game->player.vel_y);
        }
        network_send_attack_intent(aim_x, aim_y);
    }

    map_input_only:;
	map_view_update(game);
    // Toggle full map
    if (input_key_just_pressed(&game->input, g_keybinds.toggle_map)) {
        game->playing->show_map = !game->playing->show_map;
        if (game->playing->show_map && game->playing->map_zoom == 0.0f)
            game->playing->map_zoom = 1.0f;
    }

    // ESC — close map first, then other windows, then pause
    if (input_key_just_pressed(&game->input, GLFW_KEY_ESCAPE)) {
        if (game->playing->show_map) {
            game->playing->show_map = 0;
        } else if (dialogue_is_active()) {
            dialogue_close();
		} else if(game->inspect_screen && game->inspect_screen->is_open) {
			inspect_client_close(game);
        } else if (game->character_screen && game->character_screen->is_open) {
            character_screen_toggle(game->character_screen);
        } else if (game->inventory && game->inventory->is_open) {
            inventory_toggle(game->inventory);
        } else if (game->playing->target_npc_id != 0 || game->playing->target_player_id != 0) {
            game->playing->target_npc_id    = 0;
            game->playing->target_player_id = 0;
        } else {
            game->playing->is_paused = 1;
            pm_open();
        }
    }
    (void)window;
}
