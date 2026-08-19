/**
 * @file
 * Implement character listing, creation, deletion, and world-entry transitions.
 */

#include "state_handler.h"
#include "game.h"
#include "renderer.h"
#include "input/input.h"
#include "network/network.h"
#include <stdio.h>
#include <string.h>
#include <winsock2.h>

static void char_select_enter(GameState* game) {
    printf("[STATE] Entering character select\n");
    game->char_select.loaded = 0;
    game->char_select.selected_index = -1;
    game->char_select.show_creation = 0;
    game->char_select.selected_class = 1;
    game->char_select.selected_race = 1;
    game->char_select.pending_delete = 0;
    game->char_select.pending_delete_index = -1;
    memset(game->char_select.new_name, 0, sizeof(game->char_select.new_name));
    memset(game->char_select.error_message, 0, sizeof(game->char_select.error_message));
    game->net_state = NET_STATE_IDLE;
}

static void char_select_exit(GameState* game) {
    (void)game;
    printf("[STATE] Exiting character select\n");
}

/**
 * Release a pending character request after rejection or timeout.
 *
 * @param delta_time  Elapsed frame time in seconds.
 * @param error_out  Destination for a user-facing failure message.
 * @param error_size  Capacity of error_out in bytes.
 */
static void resolve_stalled_request(GameState* game, float delta_time,
                                    char* error_out, size_t error_size) {
    if (game->net_state == NET_STATE_IDLE) return;

    game->net_wait_seconds += delta_time;

    uint8_t  rejected_type = 0;
    uint16_t retry_ms      = 0;

    if (network_get_rate_limit_notice(&rejected_type, &retry_ms)) {
        game->net_state        = NET_STATE_IDLE;
        game->net_wait_seconds = 0.0f;
        snprintf(error_out, error_size,
                 "Too many requests - try again in %.1fs", retry_ms / 1000.0f);
        return;
    }

    if (game->net_wait_seconds > NET_REQUEST_TIMEOUT_SECONDS) {
        game->net_state        = NET_STATE_IDLE;
        game->net_wait_seconds = 0.0f;
        snprintf(error_out, error_size, "Server did not respond - please try again");
    }
}

/**
 * Drive deferred character operations and their network response state machine.
 *
 * @param delta_time  Elapsed frame time in seconds.
 */
static void char_select_update(GameState* game, float delta_time) {
    resolve_stalled_request(game, delta_time,
                            game->char_select.error_message,
                            sizeof(game->char_select.error_message));
    
    // Handle deferred character creation
    if (game->char_select.pending_create) {
        game->char_select.pending_create = 0;

        if (strlen(game->char_select.new_name) < 3) {
            snprintf(game->char_select.error_message, sizeof(game->char_select.error_message), "Name must be at least 3 characters");
        } else if (game->net_state == NET_STATE_IDLE) {
            uint32_t world_id = ntohl(game->server_list.list.worlds[game->server_list.selected_index].world_id);

            if (network_create_character(world_id, game->char_select.new_name,
                                         game->char_select.selected_class,
                                         game->char_select.selected_race)) {
                game->net_state = NET_STATE_CREATING_CHARACTER;
                game->net_wait_seconds = 0.0f;
                memset(game->char_select.error_message, 0, sizeof(game->char_select.error_message));
            }
        }
    }

    // Handle deferred character deletion
    if (game->char_select.pending_delete) {
        game->char_select.pending_delete = 0;
        int idx = game->char_select.pending_delete_index;
        if (idx >= 0 && idx < game->char_select.list.count && game->net_state == NET_STATE_IDLE) {
            uint32_t world_id = ntohl(game->char_select.list.world_id);
            uint32_t char_id  = ntohl(game->char_select.list.characters[idx].character_id);
            if (network_delete_character(world_id, char_id)) {
                game->net_state = NET_STATE_DELETING_CHARACTER;
                game->net_wait_seconds = 0.0f;
                memset(game->char_select.error_message, 0, sizeof(game->char_select.error_message));
            }
        }
        game->char_select.pending_delete_index = -1;
    }
    
    // State machine
    switch (game->net_state) {
        case NET_STATE_IDLE:
            if (!game->char_select.loaded && game->server_list.selected_index >= 0) {
                uint32_t world_id = ntohl(game->server_list.list.worlds[game->server_list.selected_index].world_id);
                if (network_request_character_list(world_id)) {
                    game->net_state = NET_STATE_WAITING_FOR_CHARACTERS;
                    game->net_wait_seconds = 0.0f;
                }
            }
            break;
            
        case NET_STATE_WAITING_FOR_CHARACTERS:
            if (network_get_character_list(&game->char_select.list)) {
                game->char_select.loaded = 1;
                game->net_state = NET_STATE_IDLE;
                printf("[CHAR_SELECT] Received %d characters\n", game->char_select.list.count);
            }
            break;
            
        case NET_STATE_CREATING_CHARACTER:
            {
                CharacterCreateResponsePacket response;
                if (network_get_character_create_response(&response)) {
                    if (response.success) {
                        game->char_select.show_creation = 0;
                        memset(game->char_select.new_name, 0, sizeof(game->char_select.new_name));

                        // The list is about to change, and the old highlight
                        // refers to a position in the OLD list. Drop it rather
                        // than let it point at a different character.
                        game->char_select.selected_index = -1;

                        // The server pushes a refreshed list immediately after
                        // this response, so wait for THAT instead of asking for
                        // another one. Going through NET_STATE_IDLE would have
                        // issued a second request whose first act is to discard
                        // the pushed list -- a wasted round-trip, and a window in
                        // which the screen showed the pre-create list. Waiting
                        // here consumes the push, usually already buffered from
                        // the same read, so the new character appears in the
                        // same frame the response is handled.
                        game->char_select.loaded = 0;
                        game->net_state = NET_STATE_WAITING_FOR_CHARACTERS;
                        game->net_wait_seconds = 0.0f;
                    } else {
                        game->net_state = NET_STATE_IDLE;
                        strncpy(game->char_select.error_message, response.message, 127);
                        game->char_select.error_message[127] = '\0';
                    }
                }
            }
            break;

        case NET_STATE_DELETING_CHARACTER:
            {
                CharacterDeleteResponsePacket response;
                if (network_get_character_delete_response(&response)) {
                    if (response.success) {
                        // Same as create: the server pushes the refreshed list,
                        // so consume that rather than issuing a request that
                        // would only discard it.
                        game->char_select.loaded = 0;
                        game->char_select.selected_index = -1;
                        game->net_state = NET_STATE_WAITING_FOR_CHARACTERS;
                        game->net_wait_seconds = 0.0f;
                    } else {
                        game->net_state = NET_STATE_IDLE;
                        strncpy(game->char_select.error_message, response.message, 127);
                        game->char_select.error_message[127] = '\0';
                    }
                }
            }
            break;
            
        case NET_STATE_WAITING_FOR_ENTER_WORLD:
            {
                EnterWorldResponsePacket response;
                if (network_get_enter_world_response(&response)) {
                    game->net_state = NET_STATE_IDLE;
                    
                    if (response.success) {
                        char world_ip[16];
                        strncpy(world_ip, response.world_ip, 15);
                        world_ip[15] = '\0';
                        uint16_t world_port = ntohs(response.world_port);
                        
                        uint32_t char_id = ntohl(
                            game->char_select.list.characters[game->char_select.selected_index].character_id
                        );
                        
                        if (network_connect_to_world(world_ip, world_port,
                                                    response.game_ticket, char_id)) {
                            game->network_connected = 1;
                            game->player.info_loaded = 0;
                            game_change_state(game, GAME_MODE_PLAYING);

                            uint32_t world_id = ntohl(game->server_list.list.worlds[game->server_list.selected_index].world_id);
                            network_request_character_data(char_id, world_id);
                            network_set_character_id(char_id);
                        }
                    } else {
                        printf("[CHAR_SELECT] Enter world denied: %s\n", response.message);
                    }
                }
            }
            break;
            
        default:
            break;
    }
}

/**
 * Render and operate the character-creation overlay.
 */
static void char_select_render_creation(GameState* game) {
    int vw = game->camera.viewport_width;
    int vh = game->camera.viewport_height;
    
    float panel_w = 500;
    float panel_h = 500;
    float panel_x = (vw - panel_w) / 2;
    float panel_y = (vh - panel_h) / 2;
    
    renderer_draw_rect(panel_x, panel_y, panel_w, panel_h, 0.15f, 0.15f, 0.2f, 0.95f);
    renderer_draw_rect(panel_x, panel_y, panel_w, 60, 0.2f, 0.3f, 0.4f, 1.0f);
    renderer_draw_text(panel_x + 20, panel_y + 40, "CREATE CHARACTER");
    
    // Name input
    renderer_draw_rect(panel_x + 50, panel_y + 80, panel_w - 100, 40, 0.1f, 0.1f, 0.15f, 1.0f);
    renderer_draw_text(panel_x + 60, panel_y + 105, "Name:");
    renderer_draw_text(panel_x + 130, panel_y + 105, game->char_select.new_name);
    
    // Class selection
    renderer_draw_text(panel_x + 50, panel_y + 140, "Class:");
    const char* classes[] = {"Gladiator", "Ninja", "Landweaver", "Spirit"};
    
    for (int i = 0; i < 4; i++) {
        float y = panel_y + 160 + i * 45;
        int selected = (game->char_select.selected_class == i + 1);
        int hovered = input_mouse_in_rect(&game->input, panel_x + 50, y, 200, 35);
        
        float c = selected ? 0.4f : (hovered ? 0.3f : 0.2f);
        renderer_draw_rect(panel_x + 50, y, 200, 35, c, c, c + 0.1f, 1.0f);
        renderer_draw_text(panel_x + 60, y + 25, classes[i]);
        
        if (hovered && game->input.mouse_left_clicked) {
            game->char_select.selected_class = i + 1;
        }
    }
    
    // Race selection
    renderer_draw_text(panel_x + 270, panel_y + 140, "Race:");
    const char* races[] = {"Human", "Pyseck", "Infor"};
    
    for (int i = 0; i < 3; i++) {
        float y = panel_y + 160 + i * 45;
        int selected = (game->char_select.selected_race == i + 1);
        int hovered = input_mouse_in_rect(&game->input, panel_x + 270, y, 180, 35);
        
        float c = selected ? 0.4f : (hovered ? 0.3f : 0.2f);
        renderer_draw_rect(panel_x + 270, y, 180, 35, c, c, c + 0.1f, 1.0f);
        renderer_draw_text(panel_x + 280, y + 25, races[i]);
        
        if (hovered && game->input.mouse_left_clicked) {
            game->char_select.selected_race = i + 1;
        }
    }
    
    // Create button
    float btn_y = panel_y + panel_h - 100;
    int create_hovered = input_mouse_in_rect(&game->input, panel_x + 150, btn_y, 200, 45);
    int creating = (game->net_state == NET_STATE_CREATING_CHARACTER);
    
    renderer_draw_rect(panel_x + 150, btn_y, 200, 45,
                      creating ? 0.15f : (create_hovered ? 0.3f : 0.2f),
                      creating ? 0.4f : (create_hovered ? 0.7f : 0.5f),
                      0.2f, 1.0f);
    renderer_draw_text(panel_x + 220, btn_y + 30, "CREATE");
    
    if (create_hovered && game->input.mouse_left_clicked && !creating) {
        game->char_select.pending_create = 1;
    }
    
    // Error message
    if (game->char_select.error_message[0] != '\0') {
        renderer_draw_rect(panel_x + 50, panel_y + panel_h - 50, panel_w - 100, 30,
                          0.8f, 0.2f, 0.2f, 1.0f);
        renderer_draw_text(panel_x + 60, panel_y + panel_h - 30, game->char_select.error_message);
    }
    
    // Cancel button
    int cancel_hovered = input_mouse_in_rect(&game->input, panel_x + 50, btn_y, 80, 45);
    renderer_draw_rect(panel_x + 50, btn_y, 80, 45,
                      cancel_hovered ? 0.7f : 0.5f, 0.2f, 0.2f, 1.0f);
    renderer_draw_text(panel_x + 55, btn_y + 30, "CANCEL");
    
    if (cancel_hovered && game->input.mouse_left_clicked) {
        game->char_select.show_creation = 0;
        memset(game->char_select.new_name, 0, sizeof(game->char_select.new_name));
        memset(game->char_select.error_message, 0, sizeof(game->char_select.error_message));
    }
}

/**
 * Render the character list and process selection, deletion, and navigation.
 */
static void char_select_render(GameState* game) {
    int vw = game->camera.viewport_width;
    int vh = game->camera.viewport_height;
    
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, vw, vh, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    
    // Background
    if (game->textures.background != 0) {
        renderer_draw_sprite(0, 0, vw, vh, game->textures.background);
    } else {
        renderer_draw_rect(0, 0, vw, vh, 0.1f, 0.1f, 0.2f, 1.0f);
    }
    
    // Character creation overlay
    if (game->char_select.show_creation) {
        char_select_render_creation(game);
        return;
    }
    
    // Character list panel
    float panel_w = 600;
    float panel_h = 500;
    float panel_x = (vw - panel_w) / 2;
    float panel_y = (vh - panel_h) / 2;
    
    renderer_draw_rect(panel_x, panel_y, panel_w, panel_h, 0.15f, 0.15f, 0.2f, 0.95f);
    renderer_draw_rect(panel_x, panel_y, panel_w, 4, 0.4f, 0.6f, 0.8f, 1.0f);
    renderer_draw_rect(panel_x, panel_y + panel_h - 4, panel_w, 4, 0.4f, 0.6f, 0.8f, 1.0f);
    renderer_draw_rect(panel_x, panel_y, 4, panel_h, 0.4f, 0.6f, 0.8f, 1.0f);
    renderer_draw_rect(panel_x + panel_w - 4, panel_y, 4, panel_h, 0.4f, 0.6f, 0.8f, 1.0f);
    
    renderer_draw_rect(panel_x, panel_y, panel_w, 60, 0.2f, 0.3f, 0.4f, 1.0f);
    renderer_draw_text(panel_x + 20, panel_y + 40, "SELECT CHARACTER");
    
    if (!game->char_select.loaded) {
        renderer_draw_text(panel_x + 250, panel_y + 200, "Loading...");
    } else {
        float item_y = panel_y + 80;
        float item_h = 60;
        float item_spacing = 10;
        
        const char* class_names[] = {"", "Gladiator", "Ninja", "Landweaver", "Spirit"};
        const char* race_names[] = {"", "Human", "Pyseck", "Infor"};
        
        int is_busy = (game->net_state != NET_STATE_IDLE);

        for (int i = 0; i < game->char_select.list.count && i < 10; i++) {
            float y = item_y + i * (item_h + item_spacing);

            // Main row (excluding delete button area)
            float row_w = panel_w - 100;  // leave 80px on right for DEL
            int row_hovered = input_mouse_in_rect(&game->input, panel_x + 20, y, row_w, item_h);
            float c = row_hovered ? 0.3f : 0.2f;
            renderer_draw_rect(panel_x + 20, y, row_w, item_h, c, c, c + 0.1f, 1.0f);

            char info[128];
            uint32_t class_id = ntohl(game->char_select.list.characters[i].class_id);
            uint32_t race_id  = ntohl(game->char_select.list.characters[i].race_id);

            snprintf(info, sizeof(info), "%s - Lv.%lu %s %s",
                    game->char_select.list.characters[i].name,
                    (unsigned long)ntohl(game->char_select.list.characters[i].level),
                    class_id <= 4 ? class_names[class_id] : "Unknown",
                    race_id  <= 3 ? race_names[race_id]  : "Unknown");

            renderer_draw_text(panel_x + 40, y + 40, info);

            if (row_hovered && game->input.mouse_left_clicked && !is_busy) {
                game->char_select.selected_index = i;

                uint32_t char_id = ntohl(game->char_select.list.characters[i].character_id);
                uint32_t world_id = ntohl(game->char_select.list.world_id);

                if (network_request_enter_world(char_id, world_id)) {
                    game->net_state = NET_STATE_WAITING_FOR_ENTER_WORLD;
                    game->net_wait_seconds = 0.0f;
                    is_busy = 1;
                }
            }

            // Delete button
            float del_x = panel_x + 20 + row_w + 5;
            float del_w = 55;
            int del_hovered = input_mouse_in_rect(&game->input, del_x, y + 10, del_w, item_h - 20);
            renderer_draw_rect(del_x, y + 10, del_w, item_h - 20,
                              del_hovered ? 0.9f : 0.6f, 0.2f, 0.2f, 1.0f);
            renderer_draw_text(del_x + 8, y + 35, "DEL");

            if (del_hovered && game->input.mouse_left_clicked && !is_busy) {
                game->char_select.pending_delete = 1;
                game->char_select.pending_delete_index = i;
                is_busy = 1;
            }
        }
        
        // New character button
        float btn_y = panel_y + panel_h - 70;
        int btn_hovered = input_mouse_in_rect(&game->input, panel_x + 200, btn_y, 200, 45);
        
        renderer_draw_rect(panel_x + 200, btn_y, 200, 45,
                          btn_hovered ? 0.3f : 0.2f, btn_hovered ? 0.7f : 0.5f, 0.2f, 1.0f);
        renderer_draw_text(panel_x + 215, btn_y + 30, "NEW CHARACTER");
        
        if (btn_hovered && game->input.mouse_left_clicked) {
            game->char_select.show_creation = 1;
        }
    }
    
    // Loading indicator
    if (game->net_state == NET_STATE_WAITING_FOR_ENTER_WORLD) {
        renderer_draw_rect(panel_x + 200, panel_y + 200, 200, 50, 0.2f, 0.6f, 0.8f, 0.9f);
        renderer_draw_text(panel_x + 220, panel_y + 230, "Entering world...");
    } else if (game->net_state == NET_STATE_DELETING_CHARACTER) {
        renderer_draw_rect(panel_x + 200, panel_y + 200, 200, 50, 0.7f, 0.2f, 0.2f, 0.9f);
        renderer_draw_text(panel_x + 215, panel_y + 230, "Deleting...");
    }
    
    // Back button
    float back_x = panel_x - 120;
    float back_y = panel_y + panel_h / 2 - 25;
    int back_hovered = input_mouse_in_rect(&game->input, back_x, back_y, 100, 50);
    
    renderer_draw_rect(back_x, back_y, 100, 50,
                      back_hovered ? 0.8f : 0.6f, 0.3f, 0.3f, 1.0f);
    renderer_draw_text(back_x + 25, back_y + 35, "Back");
    
    if (back_hovered && game->input.mouse_left_clicked) {
        game_change_state(game, GAME_MODE_SERVER_LIST);
    }
}

/**
 * Handle character-select escape and creation-name keyboard input.
 */
static void char_select_input(GameState* game, GLFWwindow* window, float delta_time) {
    (void)delta_time;
    
    if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
        if (game->char_select.show_creation) {
            game->char_select.show_creation = 0;
        } else {
            game_change_state(game, GAME_MODE_SERVER_LIST);
        }
    }
    
    // Handle text input for character creation
    if (game->char_select.show_creation) {
        int shift_held = glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
                         glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;
        for (int key = GLFW_KEY_A; key <= GLFW_KEY_Z; key++) {
            if (input_key_just_pressed(&game->input, key)) {
                size_t len = strlen(game->char_select.new_name);
                if (len < 31) {
                    int capitalize = shift_held || (len == 0);
                    char c = capitalize ? ('A' + (key - GLFW_KEY_A)) : ('a' + (key - GLFW_KEY_A));
                    game->char_select.new_name[len] = c;
                }
            }
        }
        
        if (input_key_just_pressed(&game->input, GLFW_KEY_BACKSPACE)) {
            size_t len = strlen(game->char_select.new_name);
            if (len > 0) {
                game->char_select.new_name[len - 1] = '\0';
            }
        }
    }
}

/** State-handler table for GAME_MODE_CHARACTER_SELECT. */
const StateHandler g_state_character_select = {
    .enter = char_select_enter,
    .exit = char_select_exit,
    .update = char_select_update,
    .render = char_select_render,
    .handle_input = char_select_input
};
