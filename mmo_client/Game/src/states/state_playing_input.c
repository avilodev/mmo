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
#include "ui/settings_panel.h"
#include "core/keybinds.h"

#include <string.h>
#include "core/client_log.h"

/**
 * Route gameplay, panel, chat, targeting, movement, and combat input.
 *
 * @param delta_time  Elapsed frame time in seconds for chat key repeat.
 */
void playing_input(GameState* game, GLFWwindow* window, float delta_time) {


    if (game->playing->is_paused) {
        // ESC or R unpause
        if (input_key_just_pressed(&game->input, GLFW_KEY_ESCAPE) ||
            input_key_just_pressed(&game->input, GLFW_KEY_R)) {
            game->playing->is_paused = 0;
            return;
        }
        if (game->input.mouse_left_clicked) {
            float vw   = (float)game->camera.viewport_width;
            float vh   = (float)game->camera.viewport_height;
            float pw   = 340.0f, ph = 230.0f;
            float px   = (vw - pw) * 0.5f;
            float py   = (vh - ph) * 0.5f;
            float bw   = 220.0f, bh = 38.0f;
            float bx   = px + (pw - bw) * 0.5f;
            float mx   = game->input.mouse_x;
            float my   = game->input.mouse_y;

            // Resume button
            if (mx >= bx && mx <= bx + bw && my >= py + 66 && my <= py + 66 + bh) {
                game->playing->is_paused = 0;
            }
            // Settings button — open in-game overlay, unpause so world keeps running
            if (mx >= bx && mx <= bx + bw && my >= py + 116 && my <= py + 116 + bh) {
                game->playing->is_paused = 0;
                game->show_settings = 1;
            }
            // Quit to Menu button
            if (mx >= bx && mx <= bx + bw && my >= py + 166 && my <= py + 166 + bh) {
                game->playing->is_paused = 0;
                game_change_state(game, GAME_MODE_MAIN_MENU);
            }
        }
        return; // Block all gameplay input while paused
    }

    // route input to settings overlay
    if (game->show_settings) {
        if (input_key_just_pressed(&game->input, GLFW_KEY_ESCAPE)) {
            game->show_settings = 0;
            return;
        }
        float vw = (float)game->camera.viewport_width;
        float vh = (float)game->camera.viewport_height;
        float scale = game->settings.ui_scale;
        float pw = SP_PW * scale;
        float ph = SP_PH * scale;
        float px = (vw - pw) * 0.5f;
        float py = (vh - ph) * 0.5f;
        float rel_mx = (game->input.mouse_x - px) / scale;
        float rel_my = (game->input.mouse_y - py) / scale;
        float btn_y = SP_PH - 54.0f;
        int closed = sp_handle_mouse(0, 0,
                                     rel_mx, rel_my,
                                     game->input.mouse_left_clicked,
                                     game->input.mouse_left_down,
                                     &game->settings, btn_y);
        if (closed) {
            game->show_settings = 0;
            game_settings_save(&game->settings, SETTINGS_PATH);
            game_settings_apply(&game->settings);
        }
        return;
    }

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
                                 game->input.mouse_left_clicked,
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
                                    game->input.mouse_left_clicked,
                                    !game->playing->chat.is_typing &&
                                        input_key_just_pressed(&game->input, g_keybinds.toggle_currency),
                                    !game->playing->chat.is_typing &&
                                        input_key_just_pressed(&game->input, GLFW_KEY_ESCAPE),
                                    game->camera.viewport_width,
                                    game->camera.viewport_height))
        return;
    if (game->playing->currency_panel.is_open) return;

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

    // Toggle session panel (O key)
    if (!game->playing->chat.is_typing && input_key_just_pressed(&game->input, GLFW_KEY_O)) {
        game->playing->show_session_panel = !game->playing->show_session_panel;
        if (game->playing->show_session_panel) {
            game->playing->session_current_page = 0;
            network_send_session_list_request(0);
        }
    }

    // Session panel pagination clicks
    if (game->playing->show_session_panel && game->input.mouse_left_clicked) {
        int dir = hud_session_panel_handle_click(game, game->input.mouse_x, game->input.mouse_y);
        if (dir == -1 && game->playing->session_current_page > 0) {
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

    // NPC interaction/attack with right-click
    if (game->input.mouse_right_clicked && !dialogue_is_active()) {
        // Convert mouse position to world coordinates (zoom-aware)
        float world_x = (game->input.mouse_x - game->camera.viewport_width / 2.0f) / game->camera.zoom + game->camera.x;
        float world_y = (game->input.mouse_y - game->camera.viewport_height / 2.0f) / game->camera.zoom + game->camera.y;

        // Check if clicked on an NPC
        for (int i = 0; i < game->playing->visible_npc_count; i++) {
            VisibleNPC* npc = &game->playing->visible_npcs[i];
            if (!npc->is_alive) continue;

            float dx = world_x - npc->pos_x;
            float dy = world_y - npc->pos_y;

            if (dx * dx + dy * dy < 32.0f * 32.0f) {
                if (npc->category == 1) {
                    // Hostile NPC: target + basic attack
                    game->playing->target_npc_id = npc->npc_id;
                    CLOG_DEBUG("[INPUT] Attacking NPC %u at (%.1f, %.1f)",
                           npc->npc_id, npc->pos_x, npc->pos_y);
                    network_update_facing_direction(npc->pos_x - game->player.x,
                                                   npc->pos_y - game->player.y);
                    network_send_attack_intent(npc->pos_x, npc->pos_y);
                } else if (npc->is_interactable) {
                    // Quest/passive NPC: interact as before
                    CLOG_DEBUG("[INPUT] Clicked on NPC %u at (%.1f, %.1f)",
                           npc->npc_id, npc->pos_x, npc->pos_y);
                    network_send_npc_interact_request(npc->npc_id);
                }
                return;  // Don't process other clicks
            }
        }
    }

    // T key opens chat
    if (!game->playing->chat.is_typing && input_key_just_pressed(&game->input, GLFW_KEY_T)) {
        game->playing->chat.is_typing = 1;
    }

    // Click on chat area: input box opens chat; clicking a message line starts a whisper
    if (!game->playing->chat.is_typing && game->input.mouse_left_clicked) {
        float chat_h  = (float)CHAT_LINES * CHAT_LINE_H + CHAT_PAD * 2.0f;
        float chat_y  = (float)game->camera.viewport_height - chat_h - CHAT_INBOX_H - 20.0f;
        float input_y = chat_y + chat_h;
        float mx = game->input.mouse_x;
        float my = game->input.mouse_y;

        if (mx >= CHAT_X && mx <= CHAT_X + CHAT_W) {
            if (my >= input_y && my <= input_y + CHAT_INBOX_H) {
                // Clicked the input box — just open chat
                game->playing->chat.is_typing = 1;
            } else if (my >= chat_y && my < input_y) {
                // Clicked a message line — pre-fill "/w SenderName " for that line
                ChatState* chat = &game->playing->chat;
                int start = chat->line_count - CHAT_LINES;
                if (start < 0) start = 0;
                for (int i = start; i < chat->line_count; i++) {
                    float line_y = chat_y + CHAT_PAD + (float)(i - start) * CHAT_LINE_H;
                    if (my >= line_y && my < line_y + CHAT_LINE_H) {
                        const ChatLine* ln = &chat->lines[i];
                        // Only pre-fill for lines that have a real sender
                        if (ln->sender[0] != '\0' && strncmp(ln->sender, "-> ", 3) != 0) {
                            snprintf(chat->input_buf, sizeof(chat->input_buf),
                                     "/w %s ", ln->sender);
                            chat->input_len = (int)strlen(chat->input_buf);
                            chat->is_typing = 1;
                        }
                        break;
                    }
                }
            }
        }
    }

    // Chat input - Enter key toggles typing mode
    if (input_key_just_pressed(&game->input, GLFW_KEY_ENTER)) {
        if (game->playing->chat.is_typing) {
            // Send message or handle slash commands
            if (game->playing->chat.input_len > 0) {
                const char* msg = game->playing->chat.input_buf;
                if (msg[0] == '/') {
                    // Slash command parsing
                    if (strncmp(msg, "/invite ", 8) == 0 && msg[8] != '\0') {
                        network_send_party_invite(msg + 8);
                    } else if (strcmp(msg, "/leave") == 0) {
                        network_send_party_leave();
                    } else if (strncmp(msg, "/g ", 3) == 0 && msg[3] != '\0') {
                        network_send_chat(1, msg + 3); // channel 1 = global
                    } else if (strncmp(msg, "/p ", 3) == 0 && msg[3] != '\0') {
                        network_send_chat(3, msg + 3); // channel 3 = party
                    } else if (strncmp(msg, "/w ", 3) == 0 && msg[3] != '\0') {
                        network_send_chat(2, msg + 3); // channel 2 = whisper
                    } else if (strncmp(msg, "/r ", 3) == 0 && msg[3] != '\0') {
                        // Reply to last whisper sender
                        if (game->playing->chat.whisper_reply_target[0] != '\0') {
                            char whisper_msg[MAX_CHAT_INPUT_LEN + 32];
                            snprintf(whisper_msg, sizeof(whisper_msg), "%s %s",
                                     game->playing->chat.whisper_reply_target, msg + 3);
                            network_send_chat(2, whisper_msg);
                        }
                        // If no reply target yet, silently drop (no one has whispered us)
                    }
                    // Unknown commands are silently dropped (no server echo)
                } else {
                    // When active channel is WHISPER, prepend the reply target so
                    // the server knows who to route to
                    if (game->playing->chat.active_channel == 2 &&
                        game->playing->chat.whisper_reply_target[0] != '\0') {
                        char whisper_msg[MAX_CHAT_INPUT_LEN + 32];
                        snprintf(whisper_msg, sizeof(whisper_msg), "%s %s",
                                 game->playing->chat.whisper_reply_target, msg);
                        network_send_chat(2, whisper_msg);
                    } else {
                        network_send_chat(game->playing->chat.active_channel, msg);
                    }
                }
            }
            game->playing->chat.is_typing = 0;
            game->playing->chat.input_len = 0;
            game->playing->chat.input_buf[0] = '\0';
        } else {
            game->playing->chat.is_typing = 1;
        }
    }

    // Don't process movement/combat input while typing in chat
    if (game->playing->chat.is_typing) {
        // Escape to cancel chat
        if (input_key_just_pressed(&game->input, GLFW_KEY_ESCAPE)) {
            game->playing->chat.is_typing = 0;
            game->playing->chat.input_len = 0;
            game->playing->chat.input_buf[0] = '\0';
        }
        // Backspace (use held state for key repeat)
        if (input_key_pressed(&game->input, GLFW_KEY_BACKSPACE)) {
            if (input_key_just_pressed(&game->input, GLFW_KEY_BACKSPACE)) {
                if (game->playing->chat.input_len > 0) {
                    game->playing->chat.input_len--;
                    game->playing->chat.input_buf[game->playing->chat.input_len] = '\0';
                }
                game->playing->chat.backspace_timer = 0.0f;
                game->playing->chat.backspace_first = 1;
            } else {
                game->playing->chat.backspace_timer += delta_time;
                float threshold = game->playing->chat.backspace_first ? 0.4f : 0.05f;
                if (game->playing->chat.backspace_timer >= threshold) {
                    game->playing->chat.backspace_timer = 0.0f;
                    game->playing->chat.backspace_first = 0;
                    if (game->playing->chat.input_len > 0) {
                        game->playing->chat.input_len--;
                        game->playing->chat.input_buf[game->playing->chat.input_len] = '\0';
                    }
                }
            }
        }
        // Tab to cycle channels
        if (input_key_just_pressed(&game->input, GLFW_KEY_TAB)) {
            game->playing->chat.active_channel = (game->playing->chat.active_channel + 1) % 4;
        }
        return; // Don't process other input while typing
    }

    // Block gameplay input while dead (server auto-respawns)
    if (game->playing->is_dead) return;

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
    if (game->playing->show_map) goto map_input_only;

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
        float world_x = (game->input.mouse_x - game->camera.viewport_width / 2.0f) / game->camera.zoom + game->camera.x;
        float world_y = (game->input.mouse_y - game->camera.viewport_height / 2.0f) / game->camera.zoom + game->camera.y;

        // Check if clicked on a nearby player to target them
        int hit_target = 0;
        for (int i = 0; i < game->playing->nearby_player_count; i++) {
            const NearbyPlayer* np = &game->playing->nearby_players[i];
            if (np->is_dead) continue;
            float dx = world_x - np->pos_x;
            float dy = world_y - np->pos_y;
            if (dx * dx + dy * dy < 32.0f * 32.0f) {
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
                float dx = world_x - npc->pos_x;
                float dy = world_y - npc->pos_y;
                if (dx * dx + dy * dy < 32.0f * 32.0f) {
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
    done_click:;

    // Leave party
    if (input_key_just_pressed(&game->input, g_keybinds.party_leave) && game->playing->party.has_party) {
        network_send_party_leave();
    }

    player_update_movement(&game->player, &game->input, &game->world, delta_time);

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
        float aim_x = (game->input.mouse_x - game->camera.viewport_width / 2.0f) / game->camera.zoom + game->camera.x;
        float aim_y = (game->input.mouse_y - game->camera.viewport_height / 2.0f) / game->camera.zoom + game->camera.y;

        network_send_ability_cast(ability_to_cast, aim_x, aim_y, 0);
    }

    // Basic attack
    if (input_key_just_pressed(&game->input, g_keybinds.basic_attack)) {
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

    // Cancel ability cast with right-click
    if (game->input.mouse_right_clicked && game->playing->ability_bar.is_casting) {
        network_send_ability_cancel();
        ability_bar_on_cast_cancel(&game->playing->ability_bar, game->playing->ability_bar.casting_ability_id);
    }

    map_input_only:;
    // M — toggle full map
    if (input_key_just_pressed(&game->input, GLFW_KEY_M)) {
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
        } else if (game->character_screen && game->character_screen->is_open) {
            character_screen_toggle(game->character_screen);
        } else if (game->inventory && game->inventory->is_open) {
            inventory_toggle(game->inventory);
        } else if (game->playing->target_npc_id != 0 || game->playing->target_player_id != 0) {
            game->playing->target_npc_id    = 0;
            game->playing->target_player_id = 0;
        } else {
            game->playing->is_paused = 1;
        }
    }
    (void)window;
}
