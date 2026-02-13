#include "game_types.h"
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

#include <stdio.h>
#include <math.h>
#include <GLFW/glfw3.h>

// Movement sync constants
#define MOVE_INTERVAL       0.05f
#define MOVE_THRESHOLD      0.5f
#define HEARTBEAT_INTERVAL  15.0f

// Static state for movement sync
static double s_last_move_send = 0.0;
static double s_last_any_send = 0.0;
static float s_last_sent_x = 0.0f;
static float s_last_sent_y = 0.0f;
static int s_move_state_init = 0;
static int s_ability_setup_attempted = 0;

// Ability bar setup (hardcoded per-class ability data)
static void setup_ability_bar_for_class(AbilityBarState* bar, uint8_t player_class, int level) {
    uint16_t ids[5] = {0};
    const char* names[5] = {"", "", "", "", ""};
    float cds[5] = {0};
    float casts[5] = {0};
    int costs[5] = {0};
    int count = 0;

    switch (player_class) {
        case 1: // Gladiator
            ids[0]=1; names[0]="Cleave";       cds[0]=3;  casts[0]=0.5f; costs[0]=0;
            ids[1]=2; names[1]="Rage";         cds[1]=8;  casts[1]=0;    costs[1]=0;
            ids[2]=3; names[2]="Shield Bash";  cds[2]=10; casts[2]=0;    costs[2]=0;
            ids[3]=4; names[3]="Slam";         cds[3]=16; casts[3]=0.5f; costs[3]=0;
            ids[4]=5; names[4]="War Cry";      cds[4]=20; casts[4]=0;    costs[4]=0;
            count = 5;
            if (level < 7) count = 4;
            if (level < 5) count = 3;
            if (level < 3) count = 2;
            if (level < 2) count = 1;
            break;

        case 2: // Ninja
            ids[0]=16; names[0]="Dash Strike";   cds[0]=6;  casts[0]=0;    costs[0]=20;
            ids[1]=17; names[1]="Smoke Bomb";    cds[1]=14; casts[1]=0;    costs[1]=30;
            ids[2]=18; names[2]="Shuriken";      cds[2]=2;  casts[2]=0;    costs[2]=15;
            ids[3]=19; names[3]="Shadow Clone";  cds[3]=20; casts[3]=0;    costs[3]=45;
            ids[4]=20; names[4]="Execute";       cds[4]=16; casts[4]=0;    costs[4]=50;
            count = 5;
            if (level < 7) count = 4;
            if (level < 5) count = 3;
            if (level < 3) count = 2;
            if (level < 2) count = 1;
            break;

        case 3: // Landweaver
            ids[0]=6;  names[0]="Shake";       cds[0]=5;  casts[0]=1.0f; costs[0]=60;
            ids[1]=7;  names[1]="Stone Spike"; cds[1]=4;  casts[1]=0.6f; costs[1]=35;
            ids[2]=8;  names[2]="Rock Wall";   cds[2]=16; casts[2]=1.5f; costs[2]=60;
            ids[3]=9;  names[3]="Mud Pit";     cds[3]=14; casts[3]=1.0f; costs[3]=50;
            ids[4]=10; names[4]="Eruption";    cds[4]=22; casts[4]=2.0f; costs[4]=80;
            count = 5;
            if (level < 7) count = 4;
            if (level < 5) count = 3;
            if (level < 3) count = 2;
            if (level < 2) count = 1;
            break;

        case 4: // Spirit
            ids[0]=11; names[0]="Heal Pulse";   cds[0]=5;  casts[0]=0.8f; costs[0]=30;
            ids[1]=12; names[1]="Cleanse";      cds[1]=10; casts[1]=0;    costs[1]=25;
            ids[2]=13; names[2]="Sanctuary";    cds[2]=18; casts[2]=1.0f; costs[2]=50;
            ids[3]=14; names[3]="Spirit Link";  cds[3]=20; casts[3]=0.5f; costs[3]=40;
            ids[4]=15; names[4]="Revitalize";   cds[4]=30; casts[4]=1.5f; costs[4]=70;
            count = 5;
            if (level < 7) count = 4;
            if (level < 5) count = 3;
            if (level < 3) count = 2;
            if (level < 2) count = 1;
            break;

        default:
            printf("[ABILITY_BAR] Unknown class %u, no abilities assigned\n", player_class);
            break;
    }

    ability_bar_set_abilities(bar, ids, names, cds, casts, costs, count);
    printf("[ABILITY_BAR] Class %u level %d: %d abilities set up\n",
           player_class, level, count);
}

// ENTER / EXIT
static void playing_enter(GameState* game) {
    printf("[STATE] Entering gameplay\n");
    s_move_state_init = 0;
    s_ability_setup_attempted = 0;
    combat_init(&game->combat);
    
    ability_bar_init(&game->ability_bar,
                     game->camera.viewport_width,
                     game->camera.viewport_height);

    hud_init(&game->hud, 1920, 1080);

    // Initialize dialogue system with JSON data
    if (!dialogue_system_init("data/dialogues.json")) {
        printf("[WARNING] Failed to load dialogues.json\n");
    }

    extern GameState* g_current_game;
    g_current_game = game;
}

static void playing_exit(GameState* game) {
    (void)game;
    printf("[STATE] Exiting gameplay\n");
}

// UPDATE
static void playing_update(GameState* game, float delta_time) {
    double now = glfwGetTime();
     
    // Check for character data
    if (!game->player.info_loaded) {
        CharacterInfo info;
        if (network_get_character_data(&info)) {
            player_load_info(&game->player, &info);
            
            if (game->inventory) {
                inventory_load_from_server(game->inventory, info.inventory);
                inventory_add_item(game->inventory, 1, 5);
                inventory_add_item(game->inventory, 2, 3);
                inventory_add_item(game->inventory, 3, 1);
                inventory_add_item(game->inventory, 4, 1);
                printf("[GAME] Inventory loaded with test items\n");
            }
        }
    }
    
    // Set up ability bar if player is loaded but abilities aren't populated yet
    if (game->player.info_loaded && game->ability_bar.slot_count == 0) {
        if (!s_ability_setup_attempted) {
            s_ability_setup_attempted = 1;
            
            int raw_class = (int)game->player.info.player_class;
            uint8_t resolved_class = 0;
            
            if (raw_class >= 1 && raw_class <= 4) {
                resolved_class = (uint8_t)raw_class;
            }
            
            if (resolved_class == 0) {
                uint32_t converted = ntohl((uint32_t)raw_class);
                if (converted >= 1 && converted <= 4) {
                    resolved_class = (uint8_t)converted;
                }
            }
            
            if (resolved_class == 0) {
                int idx = game->char_select.selected_index;
                if (idx >= 0 && idx < (int)game->char_select.list.count) {
                    uint32_t list_class = ntohl(game->char_select.list.characters[idx].class_id);
                    if (list_class >= 1 && list_class <= 4) {
                        resolved_class = (uint8_t)list_class;
                    }
                }
            }
            
            if (resolved_class == 0 && game->char_select.selected_class >= 1 
                                    && game->char_select.selected_class <= 4) {
                resolved_class = (uint8_t)game->char_select.selected_class;
            }
            
            if (resolved_class >= 1 && resolved_class <= 4) {
                setup_ability_bar_for_class(&game->ability_bar,
                                            resolved_class,
                                            (int)game->player.info.level);
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
    //printf("[DEBUG] About to update chunks: player at %.0f, %.0f\n", game->player.x, game->player.y);
    world_update_chunks(&game->world, game->player.x, game->player.y); 
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

    // Update telegraph timers
    for (int i = 0; i < MAX_TELEGRAPHS; i++) {
        if (game->telegraphs[i].active) {
            game->telegraphs[i].elapsed += delta_time;
            if (game->telegraphs[i].elapsed >= game->telegraphs[i].cast_time) {
                game->telegraphs[i].active = 0;
            }
        }
    }

    // Update zone timers
    for (int i = 0; i < MAX_ZONES; i++) {
        if (game->zones[i].active) {
            game->zones[i].elapsed += delta_time;
            // Don't auto-expire - server sends REMOVE_ZONE
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
        network_send_ping();
        s_last_any_send = now;
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

static void render_ground_items(GameState* game) {
    for (int i = 0; i < MAX_GROUND_ITEMS; i++) {
        if (!game->ground_items[i].active) continue;
        GroundItem* item = &game->ground_items[i];
        // Gold/yellow square
        renderer_draw_rect(item->pos_x - 6, item->pos_y - 6,
                          12.0f, 12.0f, 0.9f, 0.8f, 0.1f, 1.0f);
        // Small sparkle border
        renderer_draw_rect(item->pos_x - 7, item->pos_y - 7,
                          14.0f, 14.0f, 1.0f, 0.9f, 0.3f, 0.4f);
    }
}

// ============================================================================
// SCREEN-SPACE RENDER HELPERS
// ============================================================================

static void render_chat(GameState* game) {
    ChatState* chat = &game->chat;
    float chat_x = 10.0f;
    float chat_y = (float)game->camera.viewport_height - 220.0f;
    float chat_w = 400.0f;
    float chat_h = 200.0f;

    // Chat background
    renderer_draw_rect(chat_x, chat_y, chat_w, chat_h, 0.0f, 0.0f, 0.0f, 0.4f);

    // Show last 8 messages
    int start = chat->line_count - 8;
    if (start < 0) start = 0;
    for (int i = start; i < chat->line_count; i++) {
        char line[320];
        const char* ch_prefix = "";
        switch (chat->lines[i].channel) {
            case 0: ch_prefix = "[L] "; break;
            case 1: ch_prefix = "[G] "; break;
            case 2: ch_prefix = "[W] "; break;
            case 3: ch_prefix = "[P] "; break;
        }
        snprintf(line, sizeof(line), "%s%s: %s",
                ch_prefix, chat->lines[i].sender, chat->lines[i].text);
        float line_y = chat_y + 10.0f + (float)(i - start) * 22.0f;
        renderer_draw_text(chat_x + 8.0f, line_y + 16.0f, line);
    }

    // Input line
    if (chat->is_typing) {
        float input_y = chat_y + chat_h;
        renderer_draw_rect(chat_x, input_y, chat_w, 28.0f, 0.1f, 0.1f, 0.1f, 0.7f);
        char input_display[270];
        snprintf(input_display, sizeof(input_display), "> %s_", chat->input_buf);
        renderer_draw_text(chat_x + 8.0f, input_y + 20.0f, input_display);
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

    // Dark overlay
    renderer_draw_rect(0, 0, (float)game->camera.viewport_width,
                      (float)game->camera.viewport_height,
                      0.0f, 0.0f, 0.0f, 0.5f);

    float cx = (float)game->camera.viewport_width / 2.0f;
    float cy = (float)game->camera.viewport_height / 2.0f;
    renderer_draw_text(cx - 50, cy, "YOU DIED");
    renderer_draw_text(cx - 80, cy + 30, "Press R to respawn");
}

static void render_level_up(GameState* game) {
    if (!game->show_level_up) return;

    float cx = (float)game->camera.viewport_width / 2.0f;
    char msg[64];
    snprintf(msg, sizeof(msg), "LEVEL UP! You are now level %d!", game->level_up_new_level);
    renderer_draw_rect(cx - 150, 100, 300, 40, 0.1f, 0.1f, 0.3f, 0.8f);
    renderer_draw_text(cx - 140, 126, msg);
}

// RENDER
static void playing_render(GameState* game) {
    renderer_begin_2d();
    camera_apply(&game->camera);

    world_render(&game->world, &game->camera, &game->textures);
    world_render_decorations(&game->world, &game->camera, &game->textures);

    // World-space entities
    render_zones(game);
    render_telegraphs(game);
    render_ground_items(game);
    npc_render_all(game->visible_npcs, game->visible_npc_count, game->world.tile_size);
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
    combat_render_cast_bar(&game->combat, 800.0f, 600.0f);

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

    char debug[128];
    snprintf(debug, sizeof(debug), "Pos: %.0f, %.0f  Mana: %d/%d",
             game->player.x, game->player.y,
             game->ability_bar.mana, game->ability_bar.max_mana);
    renderer_draw_text(10, 20, debug);
}

// INPUT
static void playing_input(GameState* game, GLFWwindow* window, float delta_time) {
    // Toggle inventory with 'I' key
    if (input_key_just_pressed(&game->input, GLFW_KEY_I)) {
        if (game->inventory) {
            inventory_toggle(game->inventory);
        }
    }
    
    // Toggle character screen with 'C' key
    if (input_key_just_pressed(&game->input, GLFW_KEY_C)) {
        if (game->character_screen) {
            character_screen_toggle(game->character_screen);
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

    // NPC interaction with right-click
    if (game->input.mouse_right_clicked && !dialogue_is_active()) {
        // Convert mouse position to world coordinates
        float world_x = game->input.mouse_x + game->camera.x
                        - game->camera.viewport_width / 2.0f;
        float world_y = game->input.mouse_y + game->camera.y
                        - game->camera.viewport_height / 2.0f;

        // Check if clicked on an NPC
        for (int i = 0; i < game->visible_npc_count; i++) {
            VisibleNPC* npc = &game->visible_npcs[i];
            if (!npc->is_alive) continue;
            if (!npc->is_interactable) continue;

            // Simple distance check (within 32 pixels / 1 tile)
            float dx = world_x - npc->pos_x;
            float dy = world_y - npc->pos_y;
            float dist_sq = dx * dx + dy * dy;

            if (dist_sq < 32.0f * 32.0f) {  // 32 pixel radius
                printf("[INPUT] Clicked on NPC %u at (%.1f, %.1f)\n",
                       npc->npc_id, npc->pos_x, npc->pos_y);
                network_send_npc_interact_request(npc->npc_id);
                return;  // Don't process other clicks
            }
        }
    }

    // Chat input - Enter key toggles typing mode
    if (input_key_just_pressed(&game->input, GLFW_KEY_ENTER)) {
        if (game->chat.is_typing) {
            // Send message
            if (game->chat.input_len > 0) {
                network_send_chat(game->chat.active_channel, game->chat.input_buf);
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
        // Backspace
        if (input_key_just_pressed(&game->input, GLFW_KEY_BACKSPACE)) {
            if (game->chat.input_len > 0) {
                game->chat.input_len--;
                game->chat.input_buf[game->chat.input_len] = '\0';
            }
        }
        // Tab to cycle channels
        if (input_key_just_pressed(&game->input, GLFW_KEY_TAB)) {
            game->chat.active_channel = (game->chat.active_channel + 1) % 4;
        }
        return; // Don't process other input while typing
    }

    // Respawn when dead
    if (game->is_dead) {
        if (input_key_just_pressed(&game->input, GLFW_KEY_R)) {
            // Request respawn (server handles via death timer)
            network_send_ping(); // Server auto-respawns; ping to keep connection alive
        }
        return; // Don't process other input while dead
    }

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

    // Ground item pickup with left click
    if (game->input.mouse_left_clicked) {
        float world_x = game->input.mouse_x + game->camera.x
                        - game->camera.viewport_width / 2.0f;
        float world_y = game->input.mouse_y + game->camera.y
                        - game->camera.viewport_height / 2.0f;

        for (int i = 0; i < MAX_GROUND_ITEMS; i++) {
            if (!game->ground_items[i].active) continue;
            GroundItem* item = &game->ground_items[i];
            float dx = world_x - item->pos_x;
            float dy = world_y - item->pos_y;
            if (dx*dx + dy*dy < 20.0f * 20.0f) {
                // Check player is close enough
                float pdx = game->player.x - item->pos_x;
                float pdy = game->player.y - item->pos_y;
                if (pdx*pdx + pdy*pdy < 80.0f * 80.0f) {
                    network_send_loot_pickup(item->ground_item_id);
                    break;
                }
            }
        }
    }

    // Leave party with P
    if (input_key_just_pressed(&game->input, GLFW_KEY_P) && game->party.has_party) {
        network_send_party_leave();
    }

    // Player movement (always allowed)
    player_update_movement(&game->player, &game->input, &game->world, delta_time);
    
    // Ability input (keys 1-5)
    uint16_t ability_to_cast = ability_bar_update(&game->ability_bar, delta_time, 
                                           game->input.keys_just_pressed, 
                                           game->player.info.player_class);

    if (ability_to_cast > 0) {
        float aim_x = game->input.mouse_x + game->camera.x
                      - game->camera.viewport_width / 2.0f;
        float aim_y = game->input.mouse_y + game->camera.y
                      - game->camera.viewport_height / 2.0f;

        network_send_ability_cast(ability_to_cast, aim_x, aim_y, 0);
    }

    // Legacy basic attack (space bar)
    if (input_key_just_pressed(&game->input, GLFW_KEY_SPACE)) {
        network_update_facing_direction(game->player.vel_x, game->player.vel_y);
        network_send_attack_intent(game->player.x, game->player.y);
    }
    
    // Cancel ability cast with right-click
    if (game->input.mouse_right_clicked && game->ability_bar.is_casting) {
        network_send_ability_cancel();
        ability_bar_on_cast_cancel(&game->ability_bar);
    }
    
    // ESC to close windows or quit
    if (input_key_just_pressed(&game->input, GLFW_KEY_ESCAPE)) {
        if (dialogue_is_active()) {
            dialogue_close();
        } else if (game->character_screen && game->character_screen->is_open) {
            character_screen_toggle(game->character_screen);
        } else if (game->inventory && game->inventory->is_open) {
            inventory_toggle(game->inventory);
        } else {
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }
    }
}

const StateHandler g_state_playing = {
    .enter = playing_enter,
    .exit = playing_exit,
    .update = playing_update,
    .render = playing_render,
    .handle_input = playing_input
};