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

// RENDER
static void playing_render(GameState* game) {
    renderer_begin_2d();
    camera_apply(&game->camera);
    
    world_render(&game->world, &game->camera, &game->textures);
    world_render_decorations(&game->world, &game->camera, &game->textures);
    npc_render_all(game->visible_npcs, game->visible_npc_count, game->world.tile_size);
    combat_render_indicator(&game->combat);
    combat_render_damage_numbers(&game->combat);
    player_render(&game->player, game->textures.player, game->world.tile_size);
    
    renderer_end_2d();

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
        if (game->character_screen && game->character_screen->is_open) {
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