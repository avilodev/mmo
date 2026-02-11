#include "state_handler.h"
#include "renderer.h"
#include "input/input.h"
#include "network/network.h"
#include <stdio.h>
#include <winsock2.h>

// ============================================================================
// SERVER LIST STATE
// ============================================================================

static void server_list_enter(GameState* game) {
    printf("[STATE] Entering server list\n");
    game->server_list.loaded = 0;
    game->server_list.selected_index = -1;
    game->net_state = NET_STATE_IDLE;
}

static void server_list_exit(GameState* game) {
    (void)game;
    printf("[STATE] Exiting server list\n");
}

static void server_list_update(GameState* game, float delta_time) {
    (void)delta_time;
    
    if (game->server_list.loaded) return;
    
    switch (game->net_state) {
        case NET_STATE_IDLE:
            printf("[SERVER_LIST] Requesting world list...\n");
            if (network_request_world_list()) {
                game->net_state = NET_STATE_WAITING_FOR_WORLDS;
            }
            break;
            
        case NET_STATE_WAITING_FOR_WORLDS:
            if (network_get_world_list(&game->server_list.list)) {
                game->server_list.loaded = 1;
                game->net_state = NET_STATE_IDLE;
                printf("[SERVER_LIST] Received %d worlds\n", game->server_list.list.count);
            }
            break;
            
        default:
            break;
    }
}

static void server_list_render(GameState* game) {
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
    
    // Panel
    float panel_w = 600;
    float panel_h = 500;
    float panel_x = (vw - panel_w) / 2;
    float panel_y = (vh - panel_h) / 2;
    
    renderer_draw_rect(panel_x, panel_y, panel_w, panel_h, 0.15f, 0.15f, 0.2f, 0.95f);
    
    // Border
    renderer_draw_rect(panel_x, panel_y, panel_w, 4, 0.4f, 0.6f, 0.8f, 1.0f);
    renderer_draw_rect(panel_x, panel_y + panel_h - 4, panel_w, 4, 0.4f, 0.6f, 0.8f, 1.0f);
    renderer_draw_rect(panel_x, panel_y, 4, panel_h, 0.4f, 0.6f, 0.8f, 1.0f);
    renderer_draw_rect(panel_x + panel_w - 4, panel_y, 4, panel_h, 0.4f, 0.6f, 0.8f, 1.0f);
    
    // Title
    renderer_draw_rect(panel_x, panel_y, panel_w, 60, 0.2f, 0.3f, 0.4f, 1.0f);
    renderer_draw_text(panel_x + 20, panel_y + 40, "Select World");
    
    if (!game->server_list.loaded) {
        renderer_draw_text(panel_x + 250, panel_y + 200, "Loading...");
    } else {
        // World list
        float item_y = panel_y + 80;
        float item_h = 45;
        float item_spacing = 5;
        
        for (int i = 0; i < game->server_list.list.count && i < MAX_WORLDS; i++) {
            float y = item_y + i * (item_h + item_spacing);
            
            int hovered = input_mouse_in_rect(&game->input, 
                panel_x + 20, y, panel_w - 40, item_h);
            
            float color = hovered ? 0.3f : 0.2f;
            renderer_draw_rect(panel_x + 20, y, panel_w - 40, item_h,
                              color, color, color + 0.1f, 1.0f);
            
            WorldInfo* world = &game->server_list.list.worlds[i];
            
            // Status indicator
            float sr = 0.5f, sg = 0.5f, sb = 0.5f;
            if (world->status == 1) { sr = 0.2f; sg = 0.8f; sb = 0.2f; }
            else if (world->status == 2) { sr = 0.8f; sg = 0.6f; sb = 0.2f; }
            else { sr = 0.8f; sg = 0.2f; sb = 0.2f; }
            
            renderer_draw_rect(panel_x + 30, y + 8, 12, 30, sr, sg, sb, 1.0f);
            
            // World name
            renderer_draw_text(panel_x + 55, y + 30, world->name);
            
            // Population
            char pop_text[32];
            snprintf(pop_text, sizeof(pop_text), "%d/%d", 
                    ntohs(world->population), ntohs(world->capacity));
            renderer_draw_text(panel_x + panel_w - 100, y + 30, pop_text);
            
            // Handle click
            if (hovered && game->input.mouse_left_clicked) {
                if (world->status == 0) {
                    printf("[SERVER_LIST] Cannot select offline server\n");
                } else {
                    game->server_list.selected_index = i;
                    game->mode = GAME_MODE_CHARACTER_SELECT;
                    printf("[SERVER_LIST] Selected: %s\n", world->name);
                }
            }
        }
    }
    
    // Back button
    float back_x = panel_x - 120;
    float back_y = panel_y + panel_h / 2 - 25;
    int back_hovered = input_mouse_in_rect(&game->input, back_x, back_y, 100, 50);
    
    renderer_draw_rect(back_x, back_y, 100, 50, 
                      back_hovered ? 0.8f : 0.6f, 0.3f, 0.3f, 1.0f);
    renderer_draw_text(back_x + 25, back_y + 35, "Back");
    
    if (back_hovered && game->input.mouse_left_clicked) {
        game->mode = GAME_MODE_MAIN_MENU;
    }
}

static void server_list_input(GameState* game, GLFWwindow* window, float delta_time) {
    (void)delta_time;
    
    if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
        game->mode = GAME_MODE_MAIN_MENU;
    }
}

const StateHandler g_state_server_list = {
    .enter = server_list_enter,
    .exit = server_list_exit,
    .update = server_list_update,
    .render = server_list_render,
    .handle_input = server_list_input
};