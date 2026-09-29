/**
 * @file
 * Advance gameplay state and publish throttled movement updates.
 *
 * The simulation half of the frame: everything that changes what is true,
 * before anything draws it. It ends with the only place in the client that
 * tells the server where the player is -- rate-limited by MOVE_INTERVAL,
 * suppressed below MOVE_THRESHOLD, and backstopped by HEARTBEAT_INTERVAL so a
 * stationary player is not mistaken for a departed one.
 */
#include "states/state_playing_internal.h"
#include "network.h"
#include "npc.h"
#include "player.h"
#include "combat_system.h"
#include "inventory.h"
#include "character_screen.h"
#include "ui/npc_dialogue.h"
#include "ui/friends_panel.h"

#include <math.h>
#include "core/client_log.h"

/**
 * Advance gameplay state and publish throttled movement updates.
 *
 * @param delta_time  Elapsed frame time in seconds.
 */
void playing_update(GameState* game, float delta_time) {
    game->playing->frame_dt = delta_time;
    double now = glfwGetTime();

    /* The friends panel's result line, which is the only thing in it that
     * expires on its own. */
    friends_panel_update(&game->playing->friends, delta_time);

    // Check for character data
    if (!game->player.info_loaded) {
        CharacterInfo info;
        if (network_get_character_data(&info)) {
            player_load_info(&game->player, &info);

            /* Seed the bar with the form and pool the character entered in. The
             * full stat packet below confirms both; this just avoids a frame of
             * showing the wrong bar. */
            game->playing->ability_bar.active_form   = info.form;
            game->playing->ability_bar.resource_type = info.resource_type;
            ability_bar_on_resource_update(&game->playing->ability_bar,
                                           (int32_t)info.resource, (int32_t)info.max_resource);

            // Request full stats from server (includes move speed, xp_for_next, etc.)
            network_request_player_stats();

            if (game->inventory) {
                inventory_load_from_server(game->inventory, info.inventory);
                CLOG_INFO("[GAME] Inventory loaded from server");
            }
        }
    }

    // Reset movement sync state when player data arrives
    if (game->player.needs_position_reset) {
        game->playing->move_sync.last_sent_x = game->player.x;
        game->playing->move_sync.last_sent_y = game->player.y;
        game->playing->move_sync.last_move_send = now;
        game->playing->move_sync.last_any_send = now;
        game->playing->move_sync.initialized = 1;
        game->player.needs_position_reset = 0;
    }

    /* Check for server corrections.
     *
     * The correction names the proposal it refuses, so the player is rewound
     * to the server's position and the moves sent after that one are replayed
     * from it -- see player_apply_correction(). A correction the client would
     * have agreed with therefore lands where the player already is and is
     * never seen, which is what most of them are.
     *
     * last_sent tracks what the *server* has been told, not where the player
     * now is, so it takes the corrected position rather than the replayed one.
     * The gap between them is the replayed tail, which is exactly the thing
     * that should go out in the next move. */
    float correction_x, correction_y;
    uint32_t correction_seq = 0;
    if (network_get_server_correction(&correction_x, &correction_y, &correction_seq)) {
        player_apply_correction(&game->player, &game->world,
                                correction_seq, correction_x, correction_y);
        game->playing->move_sync.last_sent_x = correction_x;
        game->playing->move_sync.last_sent_y = correction_y;
        game->playing->move_sync.last_move_send = now;
    }

    // Initialize movement state if needed
    if (!game->playing->move_sync.initialized) {
        game->playing->move_sync.last_sent_x = game->player.x;
        game->playing->move_sync.last_sent_y = game->player.y;
        game->playing->move_sync.last_move_send = now;
        game->playing->move_sync.last_any_send = now;
        game->playing->move_sync.initialized = 1;
    }

    camera_update(&game->camera, game->player.x, game->player.y, delta_time);
    combat_update(&game->playing->combat, delta_time);

    // Update inventory
    if (game->inventory) {
        inventory_update(game->inventory,
                        game->input.mouse_x,
                        game->input.mouse_y,
                        game->input.mouse_left_clicked,
                        game->input.mouse_left_down,
                        game->input.mouse_right_clicked);
    }

    // Update character screen
    if (game->character_screen) {
        character_screen_update(game->character_screen,
                               game->input.mouse_x,
                               game->input.mouse_y,
                               game->input.mouse_left_clicked,
                               game->input.mouse_left_down,
                               game->input.mouse_right_clicked);
    }

    // Check for NPC interact response (initial dialogue)
    NPCInteractResponsePacket interact_response;
    if (network_get_npc_interact_response(&interact_response)) {
        dialogue_show(&interact_response);
    }

    // Check for dialogue update (new page)
    DialogueUpdatePacket update_pkt;
    if (network_get_dialogue_update(&update_pkt)) {
        dialogue_update_page(&update_pkt);
    }

    // Check for dialogue close
    DialogueClosePacket close_pkt;
    if (network_get_dialogue_close(&close_pkt)) {
        dialogue_close();
    }

    if (dialogue_is_active())
        dialogue_handle_hover(game->input.mouse_x, game->input.mouse_y);

    // Interpolate NPC positions for smooth movement
    for (int i = 0; i < game->playing->visible_npc_count && i < MAX_VISIBLE_NPCS; i++) {
        VisibleNPC* npc = &game->playing->visible_npcs[i];
        if (npc->interp_t < 1.0f) {
            // Interpolate over ~200ms (typical server tick interval)
            npc->interp_t += delta_time * 5.0f;
            if (npc->interp_t > 1.0f) npc->interp_t = 1.0f;
            npc->pos_x = npc->prev_x + (npc->target_x - npc->prev_x) * npc->interp_t;
            npc->pos_y = npc->prev_y + (npc->target_y - npc->prev_y) * npc->interp_t;
        }
    }

    /* And the same for other players, which is what they always should have
     * had. Player positions arrive on the same 20Hz stream the NPC positions
     * do, but were drawn at whatever value last arrived -- so every other
     * player in the world jumped 50ms at a time next to NPCs that glided.
     *
     * PLAYER_INTERP_RATE is the reciprocal of the broadcast interval: at 20Hz
     * a new target lands every 50ms, and 20.0 walks interp_t from 0 to 1 in
     * exactly that. Reaching 1 early would stall on the target and reintroduce
     * the stutter in miniature; reaching it late would run permanently behind. */
    for (int i = 0; i < game->playing->nearby_player_count && i < MAX_NEARBY_PLAYERS; i++) {
        NearbyPlayer* p = &game->playing->nearby_players[i];
        if (p->interp_t >= 1.0f) continue;

        p->interp_t += delta_time * PLAYER_INTERP_RATE;
        if (p->interp_t > 1.0f) p->interp_t = 1.0f;
        p->pos_x = p->prev_x + (p->target_x - p->prev_x) * p->interp_t;
        p->pos_y = p->prev_y + (p->target_y - p->prev_y) * p->interp_t;
    }

    // Clear target if targeted NPC has died
    if (game->playing->target_npc_id != 0) {
        const VisibleNPC* t = npc_find_by_id(game->playing->visible_npcs, game->playing->visible_npc_count,
                                              game->playing->target_npc_id);
        if (!t || !t->is_alive) {
            game->playing->target_npc_id = 0;
        }
    }

    // Clear player target if targeted player is dead or no longer nearby
    if (game->playing->target_player_id != 0) {
        int still_alive = 0;
        for (int i = 0; i < game->playing->nearby_player_count; i++) {
            if (game->playing->nearby_players[i].player_id == game->playing->target_player_id &&
                !game->playing->nearby_players[i].is_dead) {
                still_alive = 1;
                break;
            }
        }
        if (!still_alive) game->playing->target_player_id = 0;
    }

    // Update telegraph timers
    for (int i = 0; i < MAX_TELEGRAPHS; i++) {
        if (game->playing->telegraphs[i].active) {
            game->playing->telegraphs[i].elapsed += delta_time;
            if (game->playing->telegraphs[i].elapsed >= game->playing->telegraphs[i].cast_time) {
                game->playing->telegraphs[i].active = 0;
            }
        }
    }

    // Kingdom Slime ground pound animation — bounce NPC up during circle telegraph
    for (int i = 0; i < game->playing->visible_npc_count; i++) {
        VisibleNPC* npc = &game->playing->visible_npcs[i];
        if (npc->npc_type_id != 5) continue;
        npc->visual_y_offset = 0.0f;
        for (int t = 0; t < MAX_TELEGRAPHS; t++) {
            if (!game->playing->telegraphs[t].active) continue;
            if (game->playing->telegraphs[t].npc_id != npc->npc_id) continue;
            if (game->playing->telegraphs[t].shape != 0) continue; // circle only
            float progress = (game->playing->telegraphs[t].cast_time > 0.0f)
                ? game->playing->telegraphs[t].elapsed / game->playing->telegraphs[t].cast_time
                : 1.0f;
            if (progress > 1.0f) progress = 1.0f;
            // Rise up then slam down: offset peaks (most negative = highest) at mid-cast
            npc->visual_y_offset = -sinf(progress * 3.14159f) * 60.0f;
            break;
        }
    }

    // Update zone timers
    for (int i = 0; i < MAX_ZONES; i++) {
        if (game->playing->zones[i].active) {
            game->playing->zones[i].elapsed += delta_time;
            // Don't auto-expire - server sends REMOVE_ZONE
        }
    }

    // Update heal VFX timers
    for (int i = 0; i < MAX_HEAL_VFXS; i++) {
        if (game->playing->heal_vfxs[i].active) {
            game->playing->heal_vfxs[i].age += delta_time;
            if (game->playing->heal_vfxs[i].age >= game->playing->heal_vfxs[i].duration) {
                game->playing->heal_vfxs[i].active = 0;
            }
        }
    }

    // Update party invite timer
    if (game->playing->party.has_pending_invite) {
        game->playing->party.invite_timer -= delta_time;
        if (game->playing->party.invite_timer <= 0.0f) {
            game->playing->party.has_pending_invite = 0;
        }
    }

    // Update death timer
    if (game->playing->is_dead) {
        game->playing->death_timer += delta_time;
    }

    // Update level-up timer
    if (game->playing->show_level_up) {
        game->playing->level_up_timer -= delta_time;
        if (game->playing->level_up_timer <= 0.0f) {
            game->playing->show_level_up = 0;
        }
    }

    // Update zone banner timer
    if (game->playing->zone_banner_timer > 0.0f) {
        game->playing->zone_banner_timer -= delta_time;
        if (game->playing->zone_banner_timer < 0.0f)
            game->playing->zone_banner_timer = 0.0f;
    }

    // Update reward notifications
    for (int i = 0; i < MAX_REWARD_POPUPS; i++) {
        if (game->playing->reward_notifications[i].active) {
            game->playing->reward_notifications[i].age += delta_time;
            if (game->playing->reward_notifications[i].age > 2.5f) {
                game->playing->reward_notifications[i].active = 0;
            }
        }
    }

    // Send movement updates
    float dx = game->player.x - game->playing->move_sync.last_sent_x;
    float dy = game->player.y - game->playing->move_sync.last_sent_y;
    float dist = sqrtf(dx * dx + dy * dy);

    if (dist > MOVE_THRESHOLD && (now - game->playing->move_sync.last_move_send) >= MOVE_INTERVAL) {
        /* Numbered and remembered before it goes out. The record is what a
         * refusal is reconciled against, so it has to exist by the time the
         * refusal can arrive -- which is any time after this line. */
        uint32_t sequence = network_next_move_sequence();
        player_record_sent_move(&game->player, sequence,
                                game->player.x, game->player.y);

        network_send_player_move(
            game->player.x, game->player.y,
            game->player.speed,
            game->player.vel_x, game->player.vel_y,
            sequence
        );

        game->playing->move_sync.last_sent_x = game->player.x;
        game->playing->move_sync.last_sent_y = game->player.y;
        game->playing->move_sync.last_move_send = now;
        game->playing->move_sync.last_any_send = now;
    } else if ((now - game->playing->move_sync.last_any_send) >= HEARTBEAT_INTERVAL) {
        game->playing->move_sync.last_any_send = now;  // network_update_with_ping() handles keepalive pings
    }
}
