#include "game_types.h"
#include "game.h"
#include "texture/texture.h"
#include "state_handler.h"
#include "renderer.h"
#include "input.h"
#include "network.h"
#include "world.h"
#include "npc.h"
#include "player.h"
#include "combat_system.h"
#include "combat_render.h"
#include "ability_bar.h"
#include "hud.h"
#include "inventory.h"
#include "character_screen.h"
#include "ui/npc_dialogue.h"
#include "ui/quest_log.h"
#include "ui/shop_ui.h"
#include "ui/settings_panel.h"
#include "audio/audio.h"
#include "core/keybinds.h"

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <GLFW/glfw3.h>

// Movement sync constants
#define MOVE_INTERVAL       0.05f
#define MOVE_THRESHOLD      0.5f
#define HEARTBEAT_INTERVAL  15.0f

// Chat layout constants (shared between render_chat and input handler)
#define CHAT_X      14.0f
#define CHAT_W      420.0f
#define CHAT_LINE_H 20.0f
#define CHAT_LINES  8
#define CHAT_PAD    8.0f
#define CHAT_INBOX_H 30.0f

// Static state for movement sync
static double s_last_move_send = 0.0;
static double s_last_any_send = 0.0;
static float s_last_sent_x = 0.0f;
static float s_last_sent_y = 0.0f;
static int s_move_state_init = 0;
// Ability bar is now populated by PACKET_ABILITY_DATA from the server (network.c)

// ENTER / EXIT
static void playing_enter(GameState* game) {
    printf("[STATE] Entering gameplay\n");
    s_move_state_init = 0;
    combat_init(&game->combat);

    // Load gameplay-only textures
    game->textures.player = texture_load("Game/Sprites/Player/player.png");
    game->textures.grass  = texture_load("Game/Sprites/World/grass.png");
    game->textures.water  = texture_load("Game/Sprites/World/water.png");
    game->textures.rock   = texture_load("Game/Sprites/World/rock.png");
    game->textures.tree1             = texture_load("Game/Sprites/Decoration/Tree/tree1.png");
    game->textures.shrub1            = texture_load("Game/Sprites/Decoration/Bush/bush1.png");
    game->textures.session_panel_bg  = texture_load("Game/Sprites/UI/session_panel_bg.png");
    game->textures.session_entry_bg  = texture_load("Game/Sprites/UI/session_entry_bg.png");

    if (!game->textures.player)
        fprintf(stderr, "[GAME] Warning: failed to load player texture\n");

    ability_bar_init(&game->ability_bar,
                     game->camera.viewport_width,
                     game->camera.viewport_height);

    hud_init(&game->hud, 1920, 1080);

    // Initialize dialogue system with JSON data
    if (!dialogue_system_init("Game/Data/dialogues")) {
        printf("[WARNING] Failed to load dialogues directory\n");
    }

    extern GameState* g_current_game;
    g_current_game = game;
}

static void playing_exit(GameState* game) {
    printf("[STATE] Exiting gameplay\n");

    if (game->network_connected) {
        network_disconnect();
        game->network_connected = 0;
    }

    // Unload gameplay-only textures
    if (game->textures.player) { texture_unload(game->textures.player); game->textures.player = 0; }
    if (game->textures.grass)  { texture_unload(game->textures.grass);  game->textures.grass  = 0; }
    if (game->textures.water)  { texture_unload(game->textures.water);  game->textures.water  = 0; }
    if (game->textures.rock)   { texture_unload(game->textures.rock);   game->textures.rock   = 0; }
    if (game->textures.tree1)            { texture_unload(game->textures.tree1);            game->textures.tree1            = 0; }
    if (game->textures.shrub1)           { texture_unload(game->textures.shrub1);           game->textures.shrub1           = 0; }
    if (game->textures.session_panel_bg) { texture_unload(game->textures.session_panel_bg); game->textures.session_panel_bg = 0; }
    if (game->textures.session_entry_bg) { texture_unload(game->textures.session_entry_bg); game->textures.session_entry_bg = 0; }

    // Unload ability icon textures
    ability_bar_cleanup(&game->ability_bar);
}

// UPDATE
static void playing_update(GameState* game, float delta_time) {
    double now = glfwGetTime();
     
    // Check for character data
    if (!game->player.info_loaded) {
        CharacterInfo info;
        if (network_get_character_data(&info)) {
            player_load_info(&game->player, &info);

            // Sync initial mana to ability bar
            ability_bar_on_mana_update(&game->ability_bar,
                                       (int32_t)info.mana, (int32_t)info.max_mana);

            // Request full stats from server (includes move speed, xp_for_next, etc.)
            network_request_player_stats();

            if (game->inventory) {
                inventory_load_from_server(game->inventory, info.inventory);
                printf("[GAME] Inventory loaded from server\n");
            }
        }
    }
    
    // Reset movement sync state when player data arrives
    if (game->player.needs_position_reset) {
        s_last_sent_x = game->player.x;
        s_last_sent_y = game->player.y;
        s_last_move_send = now;
        s_last_any_send = now;
        s_move_state_init = 1;
        game->player.needs_position_reset = 0;
    }
    
    // Check for server corrections
    float correction_x, correction_y;
    if (network_get_server_correction(&correction_x, &correction_y)) {
        player_apply_correction(&game->player, correction_x, correction_y);
        s_last_sent_x = correction_x;
        s_last_sent_y = correction_y;
        s_last_move_send = now;
    }
    
    // Initialize movement state if needed
    if (!s_move_state_init) {
        s_last_sent_x = game->player.x;
        s_last_sent_y = game->player.y;
        s_last_move_send = now;
        s_last_any_send = now;
        s_move_state_init = 1;
    }
    
    camera_update(&game->camera, game->player.x, game->player.y, delta_time);
    combat_update(&game->combat, delta_time);
    
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
        uint32_t npc_id = ntohl(interact_response.npc_id);
        uint32_t dialogue_id = ntohl(interact_response.dialogue_id);
        dialogue_show(npc_id, interact_response.npc_name, dialogue_id,
                     interact_response.page_num, interact_response.option_count,
                     interact_response.option_ids);
    }

    // Check for dialogue update (new page)
    DialogueUpdatePacket update_pkt;
    if (network_get_dialogue_update(&update_pkt)) {
        uint32_t dialogue_id = ntohl(update_pkt.dialogue_id);
        dialogue_update_page(dialogue_id, update_pkt.page_num,
                            update_pkt.option_count, update_pkt.option_ids);
    }

    // Check for dialogue close
    DialogueClosePacket close_pkt;
    if (network_get_dialogue_close(&close_pkt)) {
        dialogue_close();
    }

    // Update dialogue
    dialogue_update_state(delta_time);

    // Interpolate NPC positions for smooth movement
    for (int i = 0; i < game->visible_npc_count && i < MAX_VISIBLE_NPCS; i++) {
        VisibleNPC* npc = &game->visible_npcs[i];
        if (npc->interp_t < 1.0f) {
            // Interpolate over ~200ms (typical server tick interval)
            npc->interp_t += delta_time * 5.0f;
            if (npc->interp_t > 1.0f) npc->interp_t = 1.0f;
            npc->pos_x = npc->prev_x + (npc->target_x - npc->prev_x) * npc->interp_t;
            npc->pos_y = npc->prev_y + (npc->target_y - npc->prev_y) * npc->interp_t;
        }
    }

    // Clear target if targeted NPC has died
    if (game->target_npc_id != 0) {
        const VisibleNPC* t = npc_find_by_id(game->visible_npcs, game->visible_npc_count,
                                              game->target_npc_id);
        if (!t || !t->is_alive) {
            game->target_npc_id = 0;
        }
    }

    // Clear player target if targeted player is dead or no longer nearby
    if (game->target_player_id != 0) {
        int still_alive = 0;
        for (int i = 0; i < game->nearby_player_count; i++) {
            if (game->nearby_players[i].player_id == game->target_player_id &&
                !game->nearby_players[i].is_dead) {
                still_alive = 1;
                break;
            }
        }
        if (!still_alive) game->target_player_id = 0;
    }

    // Update telegraph timers
    for (int i = 0; i < MAX_TELEGRAPHS; i++) {
        if (game->telegraphs[i].active) {
            game->telegraphs[i].elapsed += delta_time;
            if (game->telegraphs[i].elapsed >= game->telegraphs[i].cast_time) {
                game->telegraphs[i].active = 0;
            }
        }
    }

    // Kingdom Slime ground pound animation — bounce NPC up during circle telegraph
    for (int i = 0; i < game->visible_npc_count; i++) {
        VisibleNPC* npc = &game->visible_npcs[i];
        if (npc->npc_type_id != 5) continue;
        npc->visual_y_offset = 0.0f;
        for (int t = 0; t < MAX_TELEGRAPHS; t++) {
            if (!game->telegraphs[t].active) continue;
            if (game->telegraphs[t].npc_id != npc->npc_id) continue;
            if (game->telegraphs[t].shape != 0) continue; // circle only
            float progress = (game->telegraphs[t].cast_time > 0.0f)
                ? game->telegraphs[t].elapsed / game->telegraphs[t].cast_time
                : 1.0f;
            if (progress > 1.0f) progress = 1.0f;
            // Rise up then slam down: offset peaks (most negative = highest) at mid-cast
            npc->visual_y_offset = -sinf(progress * 3.14159f) * 60.0f;
            break;
        }
    }

    // Update zone timers
    for (int i = 0; i < MAX_ZONES; i++) {
        if (game->zones[i].active) {
            game->zones[i].elapsed += delta_time;
            // Don't auto-expire - server sends REMOVE_ZONE
        }
    }

    // Update heal VFX timers
    for (int i = 0; i < MAX_HEAL_VFXS; i++) {
        if (game->heal_vfxs[i].active) {
            game->heal_vfxs[i].age += delta_time;
            if (game->heal_vfxs[i].age >= game->heal_vfxs[i].duration) {
                game->heal_vfxs[i].active = 0;
            }
        }
    }

    // Update party invite timer
    if (game->party.has_pending_invite) {
        game->party.invite_timer -= delta_time;
        if (game->party.invite_timer <= 0.0f) {
            game->party.has_pending_invite = 0;
        }
    }

    // Update death timer
    if (game->is_dead) {
        game->death_timer += delta_time;
    }

    // Update level-up timer
    if (game->show_level_up) {
        game->level_up_timer -= delta_time;
        if (game->level_up_timer <= 0.0f) {
            game->show_level_up = 0;
        }
    }

    // Update reward notifications
    for (int i = 0; i < MAX_REWARD_POPUPS; i++) {
        if (game->reward_notifications[i].active) {
            game->reward_notifications[i].age += delta_time;
            if (game->reward_notifications[i].age > 2.5f) {
                game->reward_notifications[i].active = 0;
            }
        }
    }

    // Send movement updates
    float dx = game->player.x - s_last_sent_x;
    float dy = game->player.y - s_last_sent_y;
    float dist = sqrtf(dx * dx + dy * dy);
    
    if (dist > MOVE_THRESHOLD && (now - s_last_move_send) >= MOVE_INTERVAL) {
        network_send_player_move(
            game->player.x, game->player.y,
            game->player.speed,
            game->player.vel_x, game->player.vel_y
        );
        
        s_last_sent_x = game->player.x;
        s_last_sent_y = game->player.y;
        s_last_move_send = now;
        s_last_any_send = now;
    } else if ((now - s_last_any_send) >= HEARTBEAT_INTERVAL) {
        s_last_any_send = now;  // network_update_with_ping() handles keepalive pings
    }
}

// ============================================================================
// WORLD-SPACE RENDER HELPERS
// ============================================================================

static void render_nearby_players(GameState* game) {
    int size = game->world.tile_size * 2;
    for (int i = 0; i < game->nearby_player_count; i++) {
        NearbyPlayer* p = &game->nearby_players[i];
        if (p->is_dead) continue;

        // Class-based color
        float r = 0.3f, g = 0.7f, b = 0.3f;
        switch (p->player_class) {
            case 1: r=0.9f; g=0.3f; b=0.3f; break; // Gladiator - red
            case 2: r=0.5f; g=0.2f; b=0.7f; break; // Ninja - purple
            case 3: r=0.6f; g=0.4f; b=0.2f; break; // Landweaver - brown
            case 4: r=0.3f; g=0.8f; b=0.9f; break; // Spirit - cyan
        }

        renderer_draw_rect(p->pos_x - size/2, p->pos_y - size/2,
                          (float)size, (float)size, r, g, b, 1.0f);

        // Health bar
        float bar_w = 40.0f, bar_h = 4.0f;
        float bar_x = p->pos_x - bar_w/2;
        float bar_y = p->pos_y - size/2 - 8;
        renderer_draw_rect(bar_x, bar_y, bar_w, bar_h, 0.2f, 0.2f, 0.2f, 1.0f);
        if (p->max_health > 0) {
            float pct = (float)p->health / (float)p->max_health;
            renderer_draw_rect(bar_x, bar_y, bar_w * pct, bar_h, 0.2f, 0.8f, 0.2f, 1.0f);
        }
    }
}

static void render_projectiles(GameState* game) {
    for (int i = 0; i < MAX_VISIBLE_PROJECTILES; i++) {
        if (!game->projectiles[i].active) continue;
        VisibleProjectile* proj = &game->projectiles[i];
        renderer_draw_rect(proj->pos_x - 3, proj->pos_y - 3,
                          6.0f, 6.0f, 1.0f, 0.8f, 0.2f, 1.0f);
    }
}

static void render_telegraphs(GameState* game) {
    for (int i = 0; i < MAX_TELEGRAPHS; i++) {
        if (!game->telegraphs[i].active) continue;
        VisibleTelegraph* t = &game->telegraphs[i];

        // Progress: 0.0 to 1.0 (more opaque as cast completes)
        float progress = (t->cast_time > 0) ? t->elapsed / t->cast_time : 1.0f;
        float alpha = 0.15f + progress * 0.25f;

        switch (t->shape) {
            case 0: // Circle
                renderer_draw_circle(t->pos_x, t->pos_y, t->radius,
                                    1.0f, 0.2f, 0.2f, alpha, 24);
                break;
            case 1: // Cone
                renderer_draw_cone(t->pos_x, t->pos_y, t->dir_x, t->dir_y,
                                  t->radius, t->angle,
                                  1.0f, 0.5f, 0.1f, alpha, 16);
                break;
            case 2: // Rectangle
            {
                float dx = t->dir_x, dy = t->dir_y;
                float len = sqrtf(dx*dx + dy*dy);
                if (len > 0.001f) { dx /= len; dy /= len; }
                else { dx = 1.0f; dy = 0.0f; }
                // Perpendicular
                float px = -dy, py = dx;
                float hw = t->width / 2.0f;
                float hl = t->length;

                glDisable(GL_TEXTURE_2D);
                glColor4f(1.0f, 0.3f, 0.1f, alpha);
                glBegin(GL_QUADS);
                glVertex2f(t->pos_x - px*hw, t->pos_y - py*hw);
                glVertex2f(t->pos_x + px*hw, t->pos_y + py*hw);
                glVertex2f(t->pos_x + dx*hl + px*hw, t->pos_y + dy*hl + py*hw);
                glVertex2f(t->pos_x + dx*hl - px*hw, t->pos_y + dy*hl - py*hw);
                glEnd();
                glEnable(GL_TEXTURE_2D);
                break;
            }
            case 3: // Line (thin rectangle)
            {
                float dx = t->dir_x, dy = t->dir_y;
                float len = sqrtf(dx*dx + dy*dy);
                if (len > 0.001f) { dx /= len; dy /= len; }
                else { dx = 1.0f; dy = 0.0f; }
                float px = -dy, py = dx;
                float hw = 4.0f; // thin line

                glDisable(GL_TEXTURE_2D);
                glColor4f(1.0f, 0.1f, 0.1f, alpha);
                glBegin(GL_QUADS);
                glVertex2f(t->pos_x - px*hw, t->pos_y - py*hw);
                glVertex2f(t->pos_x + px*hw, t->pos_y + py*hw);
                glVertex2f(t->pos_x + dx*t->length + px*hw, t->pos_y + dy*t->length + py*hw);
                glVertex2f(t->pos_x + dx*t->length - px*hw, t->pos_y + dy*t->length - py*hw);
                glEnd();
                glEnable(GL_TEXTURE_2D);
                break;
            }
        }
    }
}

static void render_zones(GameState* game) {
    for (int i = 0; i < MAX_ZONES; i++) {
        if (!game->zones[i].active) continue;
        VisibleZone* z = &game->zones[i];
        renderer_draw_circle(z->pos_x, z->pos_y, z->radius,
                            0.3f, 0.8f, 0.3f, 0.2f, 24);
    }
}

static void render_heal_vfxs(GameState* game) {
    for (int i = 0; i < MAX_HEAL_VFXS; i++) {
        HealVFX* vfx = &game->heal_vfxs[i];
        if (!vfx->active) continue;

        float t = vfx->age / vfx->duration;
        if (t > 1.0f) t = 1.0f;

        // Outer expanding ring fades out
        float outer_r = vfx->max_radius * t;
        float outer_a = (1.0f - t) * 0.7f;
        renderer_draw_circle(vfx->pos_x, vfx->pos_y, outer_r,
                             0.15f, 1.0f, 0.35f, outer_a, 32);

        // Second inner ring slightly delayed
        if (t > 0.15f) {
            float t2 = (t - 0.15f) / 0.85f;
            float inner_r = vfx->max_radius * 0.6f * t2;
            float inner_a = (1.0f - t2) * 0.45f;
            renderer_draw_circle(vfx->pos_x, vfx->pos_y, inner_r,
                                 0.3f, 1.0f, 0.5f, inner_a, 24);
        }
    }
}

static void render_ground_items(GameState* game) {
    for (int i = 0; i < MAX_GROUND_ITEMS; i++) {
        if (!game->ground_items[i].active) continue;
        GroundItem* item = &game->ground_items[i];
        // Sparkle border (drawn first, behind item)
        renderer_draw_rect(item->pos_x - 7, item->pos_y - 7,
                          14.0f, 14.0f, 1.0f, 0.9f, 0.3f, 0.4f);
        // Gold/yellow square (on top)
        renderer_draw_rect(item->pos_x - 6, item->pos_y - 6,
                          12.0f, 12.0f, 0.9f, 0.8f, 0.1f, 1.0f);
    }
}

// ============================================================================
// SCREEN-SPACE RENDER HELPERS
// ============================================================================

static void render_chat(GameState* game) {
    ChatState* chat = &game->chat;

    float chat_h  = (float)CHAT_LINES * CHAT_LINE_H + CHAT_PAD * 2.0f;
    float chat_y  = (float)game->camera.viewport_height - chat_h - CHAT_INBOX_H - 20.0f;

    int has_messages = (chat->line_count > 0);

    // Message panel: always show when typing, show when there are messages,
    // and show a very faint panel otherwise so the user knows chat is here.
    {
        float bg_alpha;
        if (chat->is_typing)   bg_alpha = 0.65f;
        else if (has_messages) bg_alpha = 0.35f;
        else                   bg_alpha = 0.10f;  // faint "empty" state

        renderer_draw_rect(CHAT_X, chat_y, CHAT_W, chat_h, 0.04f, 0.04f, 0.08f, bg_alpha);

        float border_a = chat->is_typing ? 0.6f : (has_messages ? 0.4f : 0.15f);
        // Top border
        renderer_draw_rect(CHAT_X, chat_y, CHAT_W, 1.5f, 0.4f, 0.5f, 0.7f, border_a);
        // Left border
        renderer_draw_rect(CHAT_X, chat_y, 1.5f, chat_h, 0.4f, 0.5f, 0.7f, border_a);
    }

    // Messages (newest at the bottom)
    int start = chat->line_count - CHAT_LINES;
    if (start < 0) start = 0;
    for (int i = start; i < chat->line_count; i++) {
        const ChatLine* ln = &chat->lines[i];

        const char* prefix = "";
        switch (ln->channel) {
            case 0: prefix = "[L] "; break;
            case 1: prefix = "[G] "; break;
            case 2: prefix = "[W] "; break;
            case 3: prefix = "[P] "; break;
        }

        char line[320];
        snprintf(line, sizeof(line), "%s%s: %s", prefix, ln->sender, ln->text);

        float line_y = chat_y + CHAT_PAD + (float)(i - start) * CHAT_LINE_H + CHAT_LINE_H;
        renderer_draw_text(CHAT_X + CHAT_PAD, line_y, line);
    }

    // Input box
    float input_y = chat_y + chat_h;

    if (chat->is_typing) {
        static const char* ch_names[] = {"Local", "Global", "Whisper", "Party"};
        int ch = (int)chat->active_channel;
        if (ch < 0 || ch > 3) ch = 0;

        // Input box background
        renderer_draw_rect(CHAT_X, input_y, CHAT_W, CHAT_INBOX_H, 0.08f, 0.08f, 0.14f, 0.90f);
        // Bottom and left borders
        renderer_draw_rect(CHAT_X, input_y + CHAT_INBOX_H - 1.5f, CHAT_W, 1.5f, 0.4f, 0.5f, 0.7f, 0.6f);
        renderer_draw_rect(CHAT_X, input_y, 1.5f, CHAT_INBOX_H, 0.4f, 0.5f, 0.7f, 0.6f);

        // Channel badge e.g. "[Local]" or "[-> Name]" for whisper with a known target
        char badge[48];
        if (ch == 2 && chat->whisper_reply_target[0] != '\0') {
            snprintf(badge, sizeof(badge), "[-> %s] ", chat->whisper_reply_target);
        } else {
            snprintf(badge, sizeof(badge), "[%s] ", ch_names[ch]);
        }
        renderer_draw_text(CHAT_X + CHAT_PAD, input_y + CHAT_INBOX_H - 8.0f, badge);

        // Typed text with blinking cursor
        char input_display[270];
        snprintf(input_display, sizeof(input_display), "%s_", chat->input_buf);
        renderer_draw_text(CHAT_X + CHAT_PAD + 72.0f, input_y + CHAT_INBOX_H - 8.0f, input_display);
    } else {
        // Idle input area — always show a faint clickable hint
        float a = has_messages ? 0.25f : 0.15f;
        renderer_draw_rect(CHAT_X, input_y, CHAT_W, CHAT_INBOX_H, 0.04f, 0.04f, 0.08f, a);
        renderer_draw_rect(CHAT_X, input_y + CHAT_INBOX_H - 1.5f, CHAT_W, 1.5f, 0.4f, 0.5f, 0.7f, a * 1.2f);
        renderer_draw_rect(CHAT_X, input_y, 1.5f, CHAT_INBOX_H, 0.4f, 0.5f, 0.7f, a * 1.2f);
        renderer_draw_text(CHAT_X + CHAT_PAD, input_y + CHAT_INBOX_H - 8.0f, "Press T to chat");
    }
}

static void render_party_frames(GameState* game) {
    PartyState* ps = &game->party;
    if (!ps->has_party) return;

    float frame_x = 10.0f;
    float frame_y = 80.0f;
    float frame_w = 160.0f;
    float frame_h = 50.0f;

    for (int i = 0; i < ps->member_count && i < MAX_PARTY_SIZE; i++) {
        PartyMember* m = &ps->members[i];
        float y = frame_y + (float)i * (frame_h + 4.0f);

        // Background
        renderer_draw_rect(frame_x, y, frame_w, frame_h, 0.1f, 0.1f, 0.15f, 0.7f);

        // Name
        renderer_draw_text(frame_x + 4.0f, y + 16.0f, m->name);

        // HP bar
        float hp_y = y + 22.0f;
        renderer_draw_rect(frame_x + 4, hp_y, frame_w - 8, 10.0f, 0.2f, 0.2f, 0.2f, 1.0f);
        if (m->max_health > 0) {
            float pct = (float)m->health / (float)m->max_health;
            renderer_draw_rect(frame_x + 4, hp_y, (frame_w - 8) * pct, 10.0f,
                             0.2f, 0.8f, 0.2f, 1.0f);
        }

        // Mana bar
        float mp_y = y + 34.0f;
        renderer_draw_rect(frame_x + 4, mp_y, frame_w - 8, 8.0f, 0.1f, 0.1f, 0.2f, 1.0f);
        if (m->max_mana > 0) {
            float pct = (float)m->mana / (float)m->max_mana;
            renderer_draw_rect(frame_x + 4, mp_y, (frame_w - 8) * pct, 8.0f,
                             0.2f, 0.3f, 0.9f, 1.0f);
        }
    }
}

static void render_party_invite(GameState* game) {
    PartyState* ps = &game->party;
    if (!ps->has_pending_invite) return;

    float w = 300.0f, h = 80.0f;
    float x = ((float)game->camera.viewport_width - w) / 2.0f;
    float y = 200.0f;

    // Background
    renderer_draw_rect(x, y, w, h, 0.1f, 0.1f, 0.2f, 0.85f);
    // Border
    renderer_draw_rect(x, y, w, 2.0f, 0.5f, 0.5f, 0.8f, 1.0f);

    char msg[96];
    snprintf(msg, sizeof(msg), "%s invited you to a party", ps->invite_from_name);
    renderer_draw_text(x + 10.0f, y + 22.0f, msg);

    char timer_str[32];
    snprintf(timer_str, sizeof(timer_str), "%.0fs remaining", ps->invite_timer);
    renderer_draw_text(x + 10.0f, y + 42.0f, timer_str);

    // Accept button
    renderer_draw_rect(x + 30, y + 50, 80, 24, 0.2f, 0.6f, 0.2f, 0.9f);
    renderer_draw_text(x + 42, y + 68, "Accept");

    // Decline button
    renderer_draw_rect(x + 190, y + 50, 80, 24, 0.6f, 0.2f, 0.2f, 0.9f);
    renderer_draw_text(x + 198, y + 68, "Decline");
}

static void render_death_screen(GameState* game) {
    if (!game->is_dead) return;

    float vw = (float)game->camera.viewport_width;
    float vh = (float)game->camera.viewport_height;

    // Dark red-tinted overlay
    renderer_draw_rect(0, 0, vw, vh, 0.15f, 0.0f, 0.0f, 0.72f);

    float cx = vw / 2.0f;
    float cy = vh / 2.0f;

    // Panel
    float pw = 260.0f, ph = 90.0f;
    float px = cx - pw * 0.5f, py = cy - ph * 0.5f;
    renderer_draw_rect(px, py, pw, ph, 0.08f, 0.05f, 0.05f, 0.95f);
    // Dark red border
    renderer_draw_rect(px,          py,          pw,   2.0f, 0.6f, 0.1f, 0.1f, 1.0f);
    renderer_draw_rect(px,          py + ph - 2, pw,   2.0f, 0.6f, 0.1f, 0.1f, 1.0f);
    renderer_draw_rect(px,          py,          2.0f, ph,   0.6f, 0.1f, 0.1f, 1.0f);
    renderer_draw_rect(px + pw - 2, py,          2.0f, ph,   0.6f, 0.1f, 0.1f, 1.0f);

    // "YOU DIED" title
    renderer_draw_text(cx - 38, py + 16, "YOU DIED");

    // Animated "Respawning..." dots based on death_timer
    int dots = (int)(game->death_timer * 2.0f) % 4;
    static const char* dot_labels[] = { "Respawning", "Respawning.", "Respawning..", "Respawning..." };
    renderer_draw_text(cx - 52, py + 52, dot_labels[dots]);
}

static void render_level_up(GameState* game) {
    if (!game->show_level_up) return;

    float cx = (float)game->camera.viewport_width / 2.0f;
    char msg[64];
    snprintf(msg, sizeof(msg), "LEVEL UP! You are now level %d!", game->level_up_new_level);
    renderer_draw_rect(cx - 150, 100, 300, 40, 0.1f, 0.1f, 0.3f, 0.8f);
    renderer_draw_text(cx - 140, 126, msg);
}

static void render_reward_notifications(GameState* game) {
    // Notifications stack upward from center-right
    float base_x = (float)game->camera.viewport_width * 0.5f + 60.0f;
    float base_y = (float)game->camera.viewport_height * 0.5f - 60.0f;

    for (int i = 0; i < MAX_REWARD_POPUPS; i++) {
        RewardNotification* notif = &game->reward_notifications[i];
        if (!notif->active) continue;

        float t     = notif->age / 2.5f;          // 0..1 over lifetime
        float alpha = 1.0f - t;
        if (alpha < 0.0f) alpha = 0.0f;

        // Float up 80 px over lifetime
        float y = base_y - notif->age * 32.0f + (float)i * 44.0f;
        float x = base_x;

        // Panel background with slight border
        float pw = 190.0f, ph = 36.0f;
        renderer_draw_rect(x - 2, y - 2, pw + 4, ph + 4,
                           0.6f, 0.5f, 0.1f, alpha * 0.4f);      // gold glow border
        renderer_draw_rect(x, y, pw, ph,
                           0.06f, 0.05f, 0.10f, alpha * 0.82f);  // dark bg

        float text_y = y + ph - 8.0f;

        // XP pill
        if (notif->xp_gained > 0) {
            renderer_draw_rect(x + 6, y + 8, 14, 14, 0.35f, 0.55f, 1.0f, alpha);
            char xp_text[32];
            snprintf(xp_text, sizeof(xp_text), "+%u XP", notif->xp_gained);
            renderer_draw_text(x + 24, text_y, xp_text);
        }

        // Gold pill
        if (notif->gold_gained > 0) {
            renderer_draw_rect(x + 100, y + 8, 14, 14, 1.0f, 0.80f, 0.05f, alpha);
            char gold_text[32];
            snprintf(gold_text, sizeof(gold_text), "+%u g", notif->gold_gained);
            renderer_draw_text(x + 118, text_y, gold_text);
        }
    }
}

// Hovered buff icon index (-1 = none), updated each input frame
static int s_hovered_effect = -1;

static void render_buff_tooltip(const GameState* game) {
    if (s_hovered_effect < 0 || s_hovered_effect >= MAX_CLIENT_EFFECTS) return;
    const ClientStatusEffect* e = &game->ability_bar.effects[s_hovered_effect];
    if (!e->active) return;

    static const char* effect_names[] = {
        "Unknown", "Damage Over Time", "Heal Over Time", "Stunned",
        "Slowed", "Buff", "Stealth", "Knock Up", "Linked", "Cleanse"
    };
    const char* name = (e->effect_type < 10) ? effect_names[e->effect_type] : "Unknown";

    float tw = 170.0f, th = 54.0f;
    float tx = game->input.mouse_x + 12.0f;
    float ty = game->input.mouse_y - th - 4.0f;
    if (tx + tw > (float)game->camera.viewport_width)  tx = game->input.mouse_x - tw - 4.0f;
    if (ty < 0) ty = game->input.mouse_y + 12.0f;

    renderer_draw_rect(tx, ty, tw, th, 0.06f, 0.06f, 0.12f, 0.96f);
    renderer_draw_rect(tx, ty, tw, 1.5f, 0.45f,0.45f,0.75f,1.0f);
    renderer_draw_rect(tx, ty+th-1.5f, tw, 1.5f, 0.45f,0.45f,0.75f,1.0f);
    renderer_draw_rect(tx, ty, 1.5f, th, 0.45f,0.45f,0.75f,1.0f);
    renderer_draw_rect(tx+tw-1.5f, ty, 1.5f, th, 0.45f,0.45f,0.75f,1.0f);

    renderer_draw_text(tx + 8.0f, ty + 18.0f, name);

    char line2[48];
    snprintf(line2, sizeof(line2), "%.1fs remaining", e->duration_remaining);
    renderer_draw_text(tx + 8.0f, ty + 36.0f, line2);

    if (e->value != 0) {
        char line3[32];
        snprintf(line3, sizeof(line3), "Value: %d", e->value);
        renderer_draw_text(tx + 8.0f, ty + 48.0f, line3);
    }
}

static void render_settings_overlay(GameState* game) {
    if (!game->show_settings) return;

    float vw = (float)game->camera.viewport_width;
    float vh = (float)game->camera.viewport_height;

    // Semi-transparent dim (lighter than pause so world is visible)
    renderer_draw_rect(0, 0, vw, vh, 0.0f, 0.0f, 0.0f, 0.40f);

    float scale = game->settings.ui_scale;
    float pw = SP_PW * scale;
    float ph = SP_PH * scale;
    float px = (vw - pw) * 0.5f;
    float py = (vh - ph) * 0.5f;

    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glTranslatef(px, py, 0.0f);
    glScalef(scale, scale, 1.0f);

    // Panel background (in scaled space at 0,0)
    renderer_draw_rect(0, 0, SP_PW, SP_PH, 0.10f, 0.10f, 0.16f, 0.97f);
    // Panel border
    renderer_draw_rect(0,         0,         SP_PW, 2.0f, 0.35f,0.50f,0.70f,1.0f);
    renderer_draw_rect(0,         SP_PH-2,   SP_PW, 2.0f, 0.35f,0.50f,0.70f,1.0f);
    renderer_draw_rect(0,         0,         2.0f,  SP_PH, 0.35f,0.50f,0.70f,1.0f);
    renderer_draw_rect(SP_PW-2,   0,         2.0f,  SP_PH, 0.35f,0.50f,0.70f,1.0f);

    sp_draw_content(0, 0, &game->settings);

    // Close button
    float btn_w = 140.0f, btn_h = 36.0f;
    float btn_x = (SP_PW - btn_w) * 0.5f;
    float btn_y = SP_PH - 54.0f;
    // Inverse-transform mouse for hover detection
    float rel_mx = (game->input.mouse_x - px) / scale;
    float rel_my = (game->input.mouse_y - py) / scale;
    int hov = (rel_mx >= btn_x && rel_mx <= btn_x + btn_w &&
               rel_my >= btn_y && rel_my <= btn_y + btn_h);
    float bc = hov ? 0.30f : 0.18f;
    renderer_draw_rect(btn_x, btn_y, btn_w, btn_h, bc, bc, bc+0.12f, 0.95f);
    renderer_draw_rect(btn_x,            btn_y,             btn_w, 1.5f, 0.35f,0.50f,0.70f,0.8f);
    renderer_draw_rect(btn_x,            btn_y+btn_h-1.5f,  btn_w, 1.5f, 0.35f,0.50f,0.70f,0.8f);
    renderer_draw_rect(btn_x,            btn_y,             1.5f, btn_h, 0.35f,0.50f,0.70f,0.8f);
    renderer_draw_rect(btn_x+btn_w-1.5f, btn_y,             1.5f, btn_h, 0.35f,0.50f,0.70f,0.8f);
    renderer_draw_text(btn_x + btn_w*0.5f - 22.0f, btn_y+btn_h-10.0f, "Close");

    glPopMatrix();
}

static void render_pause_overlay(GameState* game) {
    if (!game->is_paused) return;

    float vw = (float)game->camera.viewport_width;
    float vh = (float)game->camera.viewport_height;

    // Dim the world
    renderer_draw_rect(0, 0, vw, vh, 0.0f, 0.0f, 0.0f, 0.55f);

    // Panel dimensions
    float pw = 340.0f, ph = 230.0f;
    float px = (vw - pw) * 0.5f;
    float py = (vh - ph) * 0.5f;

    // Panel background
    renderer_draw_rect(px, py, pw, ph, 0.07f, 0.07f, 0.12f, 0.96f);

    // Panel border (top/bottom/left/right)
    renderer_draw_rect(px,          py,          pw,   2.0f, 0.45f, 0.45f, 0.75f, 1.0f);
    renderer_draw_rect(px,          py + ph - 2, pw,   2.0f, 0.45f, 0.45f, 0.75f, 1.0f);
    renderer_draw_rect(px,          py,          2.0f, ph,   0.45f, 0.45f, 0.75f, 1.0f);
    renderer_draw_rect(px + pw - 2, py,          2.0f, ph,   0.45f, 0.45f, 0.75f, 1.0f);

    // Title
    renderer_draw_text(px + pw * 0.5f - 32.0f, py + 36.0f, "PAUSED");

    // Divider below title
    renderer_draw_rect(px + 20, py + 48, pw - 40, 1.5f, 0.35f, 0.35f, 0.55f, 0.8f);

    // Button layout
    float bw = 220.0f, bh = 38.0f;
    float bx = px + (pw - bw) * 0.5f;

    // Resume
    renderer_draw_rect(bx, py + 66,  bw, bh, 0.12f, 0.38f, 0.12f, 0.92f);
    renderer_draw_text(bx + bw * 0.5f - 28.0f, py + 91, "Resume");

    // Settings (placeholder)
    renderer_draw_rect(bx, py + 116, bw, bh, 0.18f, 0.18f, 0.32f, 0.92f);
    renderer_draw_text(bx + bw * 0.5f - 32.0f, py + 141, "Settings");

    // Quit to Menu
    renderer_draw_rect(bx, py + 166, bw, bh, 0.32f, 0.08f, 0.08f, 0.92f);
    renderer_draw_text(bx + bw * 0.5f - 50.0f, py + 191, "Quit to Menu");
}


// RENDER
static void playing_render(GameState* game) {
    renderer_begin_2d();
    camera_apply(&game->camera);

    world_render(&game->world, &game->camera, &game->textures);
    world_render_decorations(&game->world, &game->camera, &game->textures);

    // World-space entities
    render_zones(game);
    render_heal_vfxs(game);
    render_telegraphs(game);
    render_ground_items(game);
    npc_render_all(game->visible_npcs, game->visible_npc_count, game->world.tile_size);
    npc_render_target_indicator(game->visible_npcs, game->visible_npc_count,
                                 game->world.tile_size, game->target_npc_id);
    render_nearby_players(game);
    render_projectiles(game);
    combat_render_indicator(&game->combat);
    combat_render_damage_numbers(&game->combat);
    player_render(&game->player, game->textures.player, game->world.tile_size);

    renderer_end_2d();

    // Screen-space UI
    hud_render(&game->hud, game);
    ability_bar_render(&game->ability_bar);
    ability_bar_render_effects(&game->ability_bar,
                               game->ability_bar.bar_x,
                               game->ability_bar.bar_y - 40.0f);
    ability_bar_render_cast_bar(&game->ability_bar,
                                (float)game->camera.viewport_width,
                                (float)game->camera.viewport_height);
    combat_render_cast_bar(&game->combat,
                            (float)game->camera.viewport_width,
                            (float)game->camera.viewport_height);

    // Render inventory and character screen (on top of everything)
    if (game->inventory) {
        inventory_render(game->inventory);
    }

    if (game->character_screen) {
        character_screen_render(game->character_screen, game);
    }

    // Render dialogue (on top of everything)
    dialogue_render();

    // Chat, party, death, level-up overlays
    render_chat(game);
    render_party_frames(game);
    render_party_invite(game);
    render_death_screen(game);
    render_level_up(game);
    render_reward_notifications(game);

    // Quest log panel (before overlays)
    quest_log_render(&game->quest_log,
                     game->camera.viewport_width,
                     game->camera.viewport_height);

    // Shop window (on top of game world, below pause)
    shop_ui_render(game);

    // Buff icon tooltip (before pause/settings overlays)
    render_buff_tooltip(game);

    // Pause overlay
    render_pause_overlay(game);

    // In-game settings overlay (on top of pause if both somehow active)
    render_settings_overlay(game);

    char debug[128];
    snprintf(debug, sizeof(debug), "Pos: %.0f, %.0f  Mana: %d/%d",
             game->player.x, game->player.y,
             game->ability_bar.mana, game->ability_bar.max_mana);
    renderer_draw_text(10, 20, debug);
}

// INPUT
static void playing_input(GameState* game, GLFWwindow* window, float delta_time) {

    // -------------------------------------------------------------------------
    // PAUSE MENU  (handle first — blocks everything else)
    // -------------------------------------------------------------------------
    if (game->is_paused) {
        // ESC or R unpause
        if (input_key_just_pressed(&game->input, GLFW_KEY_ESCAPE) ||
            input_key_just_pressed(&game->input, GLFW_KEY_R)) {
            game->is_paused = 0;
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
                game->is_paused = 0;
            }
            // Settings button — open in-game overlay, unpause so world keeps running
            if (mx >= bx && mx <= bx + bw && my >= py + 116 && my <= py + 116 + bh) {
                game->is_paused = 0;
                game->show_settings = 1;
            }
            // Quit to Menu button
            if (mx >= bx && mx <= bx + bw && my >= py + 166 && my <= py + 166 + bh) {
                game->is_paused = 0;
                game_change_state(game, GAME_MODE_MAIN_MENU);
            }
        }
        return; // Block all gameplay input while paused
    }

    // -------------------------------------------------------------------------
    // SETTINGS OVERLAY  (gameplay continues, but input routed to settings)
    // -------------------------------------------------------------------------
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

    // -------------------------------------------------------------------------
    // BUFF ICON HOVER  (update each frame before gameplay input)
    // -------------------------------------------------------------------------
    {
        float eff_x = game->ability_bar.bar_x;
        float eff_y = game->ability_bar.bar_y - 40.0f;
        float icon_size = 28.0f, icon_pad = 4.0f;
        float mx = game->input.mouse_x, my = game->input.mouse_y;
        s_hovered_effect = -1;
        int drawn = 0;
        for (int i = 0; i < MAX_CLIENT_EFFECTS; i++) {
            if (!game->ability_bar.effects[i].active) continue;
            float ix = eff_x + drawn * (icon_size + icon_pad);
            if (mx >= ix && mx <= ix + icon_size &&
                my >= eff_y && my <= eff_y + icon_size) {
                s_hovered_effect = i;
                break;
            }
            drawn++;
        }
    }

    // Quest log
    quest_log_handle_input_full(&game->quest_log,
                                 game->input.mouse_x, game->input.mouse_y,
                                 game->input.mouse_left_clicked,
                                 !game->chat.is_typing && input_key_just_pressed(&game->input, g_keybinds.toggle_quest_log),
                                 !game->chat.is_typing && input_key_just_pressed(&game->input, GLFW_KEY_ESCAPE),
                                 game->camera.viewport_width,
                                 game->camera.viewport_height);
    if (game->quest_log.is_open) return; // Block gameplay input while quest log open

    // Shop window input (blocks gameplay input while open)
    if (game->shop.is_open) {
        shop_ui_handle_input(game, game->input.mouse_x, game->input.mouse_y,
                             game->input.mouse_left_clicked);
        return;
    }

    // Toggle inventory
    if (!game->chat.is_typing && input_key_just_pressed(&game->input, g_keybinds.toggle_inventory)) {
        if (game->inventory) {
            inventory_toggle(game->inventory);
        }
    }
    
    // Toggle character screen
    if (!game->chat.is_typing && input_key_just_pressed(&game->input, g_keybinds.toggle_character)) {
        if (game->character_screen) {
            character_screen_toggle(game->character_screen);
        }
    }

    // Toggle session panel (O key)
    if (!game->chat.is_typing && input_key_just_pressed(&game->input, GLFW_KEY_O)) {
        game->show_session_panel = !game->show_session_panel;
        if (game->show_session_panel) {
            game->session_current_page = 0;
            network_send_session_list_request(0);
        }
    }

    // Session panel pagination clicks
    if (game->show_session_panel && game->input.mouse_left_clicked) {
        int dir = hud_session_panel_handle_click(game, game->input.mouse_x, game->input.mouse_y);
        if (dir == -1 && game->session_current_page > 0) {
            uint16_t next = game->session_current_page - 1;
            game->session_current_page = next;
            network_send_session_list_request(next);
        } else if (dir == 1 && game->session_current_page + 1 < game->session_total_pages) {
            uint16_t next = game->session_current_page + 1;
            game->session_current_page = next;
            network_send_session_list_request(next);
        }
    }
    
    // Handle dialogue option clicks (highest priority)
    if (dialogue_is_active() && game->input.mouse_left_clicked) {
        int selected_option = dialogue_handle_click(game->input.mouse_x, game->input.mouse_y);
        if (selected_option >= 0) {
            uint32_t npc_id = dialogue_get_current_npc();
            uint32_t dialogue_id = dialogue_get_current_dialogue_id();
            uint8_t current_page = dialogue_get_current_page();
            network_send_dialogue_option_select(npc_id, dialogue_id, current_page, (uint8_t)selected_option);
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
            if (hud_check_inventory_button_clicked(&game->hud, 
                                                   game->input.mouse_x, 
                                                   game->input.mouse_y)) {
                inventory_toggle(game->inventory);
            }
        }
        
        // Check character button
        if (game->character_screen) {
            if (hud_check_character_button_clicked(&game->hud,
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
        for (int i = 0; i < game->visible_npc_count; i++) {
            VisibleNPC* npc = &game->visible_npcs[i];
            if (!npc->is_alive) continue;

            float dx = world_x - npc->pos_x;
            float dy = world_y - npc->pos_y;

            if (dx * dx + dy * dy < 32.0f * 32.0f) {
                if (npc->category == 1) {
                    // Hostile NPC: target + basic attack
                    game->target_npc_id = npc->npc_id;
                    printf("[INPUT] Attacking NPC %u at (%.1f, %.1f)\n",
                           npc->npc_id, npc->pos_x, npc->pos_y);
                    network_update_facing_direction(npc->pos_x - game->player.x,
                                                   npc->pos_y - game->player.y);
                    network_send_attack_intent(npc->pos_x, npc->pos_y);
                } else if (npc->is_interactable) {
                    // Quest/passive NPC: interact as before
                    printf("[INPUT] Clicked on NPC %u at (%.1f, %.1f)\n",
                           npc->npc_id, npc->pos_x, npc->pos_y);
                    network_send_npc_interact_request(npc->npc_id);
                }
                return;  // Don't process other clicks
            }
        }
    }

    // T key opens chat
    if (!game->chat.is_typing && input_key_just_pressed(&game->input, GLFW_KEY_T)) {
        game->chat.is_typing = 1;
    }

    // Click on chat area: input box opens chat; clicking a message line starts a whisper
    if (!game->chat.is_typing && game->input.mouse_left_clicked) {
        float chat_h  = (float)CHAT_LINES * CHAT_LINE_H + CHAT_PAD * 2.0f;
        float chat_y  = (float)game->camera.viewport_height - chat_h - CHAT_INBOX_H - 20.0f;
        float input_y = chat_y + chat_h;
        float mx = game->input.mouse_x;
        float my = game->input.mouse_y;

        if (mx >= CHAT_X && mx <= CHAT_X + CHAT_W) {
            if (my >= input_y && my <= input_y + CHAT_INBOX_H) {
                // Clicked the input box — just open chat
                game->chat.is_typing = 1;
            } else if (my >= chat_y && my < input_y) {
                // Clicked a message line — pre-fill "/w SenderName " for that line
                ChatState* chat = &game->chat;
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
        if (game->chat.is_typing) {
            // Send message or handle slash commands
            if (game->chat.input_len > 0) {
                const char* msg = game->chat.input_buf;
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
                        if (game->chat.whisper_reply_target[0] != '\0') {
                            char whisper_msg[MAX_CHAT_INPUT_LEN + 32];
                            snprintf(whisper_msg, sizeof(whisper_msg), "%s %s",
                                     game->chat.whisper_reply_target, msg + 3);
                            network_send_chat(2, whisper_msg);
                        }
                        // If no reply target yet, silently drop (no one has whispered us)
                    }
                    // Unknown commands are silently dropped (no server echo)
                } else {
                    // When active channel is WHISPER, prepend the reply target so
                    // the server knows who to route to
                    if (game->chat.active_channel == 2 &&
                        game->chat.whisper_reply_target[0] != '\0') {
                        char whisper_msg[MAX_CHAT_INPUT_LEN + 32];
                        snprintf(whisper_msg, sizeof(whisper_msg), "%s %s",
                                 game->chat.whisper_reply_target, msg);
                        network_send_chat(2, whisper_msg);
                    } else {
                        network_send_chat(game->chat.active_channel, msg);
                    }
                }
            }
            game->chat.is_typing = 0;
            game->chat.input_len = 0;
            game->chat.input_buf[0] = '\0';
        } else {
            game->chat.is_typing = 1;
        }
    }

    // Don't process movement/combat input while typing in chat
    if (game->chat.is_typing) {
        // Escape to cancel chat
        if (input_key_just_pressed(&game->input, GLFW_KEY_ESCAPE)) {
            game->chat.is_typing = 0;
            game->chat.input_len = 0;
            game->chat.input_buf[0] = '\0';
        }
        // Backspace (use held state for key repeat)
        if (input_key_pressed(&game->input, GLFW_KEY_BACKSPACE)) {
            if (input_key_just_pressed(&game->input, GLFW_KEY_BACKSPACE)) {
                if (game->chat.input_len > 0) {
                    game->chat.input_len--;
                    game->chat.input_buf[game->chat.input_len] = '\0';
                }
                game->chat.backspace_timer = 0.0f;
                game->chat.backspace_first = 1;
            } else {
                game->chat.backspace_timer += delta_time;
                float threshold = game->chat.backspace_first ? 0.4f : 0.05f;
                if (game->chat.backspace_timer >= threshold) {
                    game->chat.backspace_timer = 0.0f;
                    game->chat.backspace_first = 0;
                    if (game->chat.input_len > 0) {
                        game->chat.input_len--;
                        game->chat.input_buf[game->chat.input_len] = '\0';
                    }
                }
            }
        }
        // Tab to cycle channels
        if (input_key_just_pressed(&game->input, GLFW_KEY_TAB)) {
            game->chat.active_channel = (game->chat.active_channel + 1) % 4;
        }
        return; // Don't process other input while typing
    }

    // Block gameplay input while dead (server auto-respawns)
    if (game->is_dead) return;

    // Party invite accept/decline clicks
    if (game->party.has_pending_invite && game->input.mouse_left_clicked) {
        float w = 300.0f;
        float x = ((float)game->camera.viewport_width - w) / 2.0f;
        float y = 200.0f;
        float mx = game->input.mouse_x;
        float my = game->input.mouse_y;

        // Accept button: (x+30, y+50) to (x+110, y+74)
        if (mx >= x+30 && mx <= x+110 && my >= y+50 && my <= y+74) {
            network_send_party_accept();
            game->party.has_pending_invite = 0;
            return;
        }
        // Decline button: (x+190, y+50) to (x+270, y+74)
        if (mx >= x+190 && mx <= x+270 && my >= y+50 && my <= y+74) {
            network_send_party_decline();
            game->party.has_pending_invite = 0;
            return;
        }
    }

    // Party frame click = target that member
    if (game->input.mouse_left_clicked && game->party.has_party) {
        const float FRAME_W  = 180.0f;
        const float FRAME_H  = 50.0f;
        const float FRAME_GAP = 5.0f;
        const float START_X  = 10.0f;
        const float START_Y  = 10.0f;
        for (int i = 0; i < game->party.member_count; i++) {
            float fx = START_X;
            float fy = START_Y + i * (FRAME_H + FRAME_GAP);
            if (game->input.mouse_x >= fx && game->input.mouse_x <= fx + FRAME_W &&
                game->input.mouse_y >= fy && game->input.mouse_y <= fy + FRAME_H) {
                game->target_player_id = game->party.members[i].id;
                game->target_npc_id    = 0;
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
        for (int i = 0; i < game->nearby_player_count; i++) {
            const NearbyPlayer* np = &game->nearby_players[i];
            if (np->is_dead) continue;
            float dx = world_x - np->pos_x;
            float dy = world_y - np->pos_y;
            if (dx * dx + dy * dy < 32.0f * 32.0f) {
                game->target_player_id = np->player_id;
                game->target_npc_id    = 0;  // Clear NPC target
                hit_target = 1;
                break;
            }
        }

        // Check if clicked on an NPC to target it
        if (!hit_target) {
            for (int i = 0; i < game->visible_npc_count; i++) {
                VisibleNPC* npc = &game->visible_npcs[i];
                if (!npc->is_alive) continue;
                float dx = world_x - npc->pos_x;
                float dy = world_y - npc->pos_y;
                if (dx * dx + dy * dy < 32.0f * 32.0f) {
                    game->target_npc_id    = npc->npc_id;
                    game->target_player_id = 0;  // Clear player target
                    hit_target = 1;
                    break;
                }
            }
        }

        // Click on empty space clears all targets
        if (!hit_target) {
            game->target_npc_id    = 0;
            game->target_player_id = 0;
        }

        // Ground item pickup
        for (int i = 0; i < MAX_GROUND_ITEMS; i++) {
            if (!game->ground_items[i].active) continue;
            GroundItem* item = &game->ground_items[i];
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
    if (input_key_just_pressed(&game->input, g_keybinds.party_leave) && game->party.has_party) {
        network_send_party_leave();
    }

    // Player movement (always allowed)
    player_update_movement(&game->player, &game->input, &game->world, delta_time);
    
    // Ability input (keys 1-5)
    uint16_t ability_to_cast = ability_bar_update(&game->ability_bar, delta_time, 
                                           game->input.keys_just_pressed, 
                                           game->player.info.player_class);

    if (ability_to_cast > 0) {
        float aim_x = (game->input.mouse_x - game->camera.viewport_width / 2.0f) / game->camera.zoom + game->camera.x;
        float aim_y = (game->input.mouse_y - game->camera.viewport_height / 2.0f) / game->camera.zoom + game->camera.y;

        network_send_ability_cast(ability_to_cast, aim_x, aim_y, 0);
    }

    // Basic attack
    if (input_key_just_pressed(&game->input, g_keybinds.basic_attack)) {
        float aim_x = game->player.x;
        float aim_y = game->player.y;
        if (game->target_npc_id != 0) {
            float tx, ty;
            if (npc_get_position(game->visible_npcs, game->visible_npc_count,
                                 game->target_npc_id, &tx, &ty)) {
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
    if (game->input.mouse_right_clicked && game->ability_bar.is_casting) {
        network_send_ability_cancel();
        ability_bar_on_cast_cancel(&game->ability_bar, game->ability_bar.casting_ability_id);
    }
    
    // ESC — close open windows, clear target, or open pause menu
    if (input_key_just_pressed(&game->input, GLFW_KEY_ESCAPE)) {
        if (dialogue_is_active()) {
            dialogue_close();
        } else if (game->character_screen && game->character_screen->is_open) {
            character_screen_toggle(game->character_screen);
        } else if (game->inventory && game->inventory->is_open) {
            inventory_toggle(game->inventory);
        } else if (game->target_npc_id != 0 || game->target_player_id != 0) {
            game->target_npc_id    = 0;
            game->target_player_id = 0;
        } else {
            game->is_paused = 1;
        }
    }
    (void)window;
}

const StateHandler g_state_playing = {
    .enter = playing_enter,
    .exit = playing_exit,
    .update = playing_update,
    .render = playing_render,
    .handle_input = playing_input
};