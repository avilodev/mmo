/**
 * @file
 * Implement world-list retrieval, refresh timing, selection, and rendering.
 */

#include "state_handler.h"
#include "game.h"
#include "renderer.h"
#include "input/input.h"
#include "network/network.h"
#include <stdio.h>
#include <winsock2.h>
#include <GLFW/glfw3.h>
#include "core/client_log.h"

/** Refresh interval for a loaded world list, in seconds. */
#define SERVER_LIST_REFRESH_INTERVAL 10.0

static double s_last_refresh = 0.0;
static int    s_refreshing   = 0;   // 1 while a background re-fetch is in flight

static void server_list_enter(GameState* game) {
    CLOG_INFO("[STATE] Entering server list");
    game->server_list.loaded = 0;
    game->server_list.selected_index = -1;
    game->net_state = NET_STATE_IDLE;
    s_last_refresh = 0.0;
    s_refreshing   = 0;
}

static void server_list_exit(GameState* game) {
    (void)game;
    CLOG_INFO("[STATE] Exiting server list");
}

/**
 * Refresh the world list and bound stalled network requests.
 *
 * Background refresh failures wait a full interval before another attempt.
 *
 * @param delta_time  Elapsed frame time in seconds.
 */
static void server_list_update(GameState* game, float delta_time) {
    double now = glfwGetTime();

    if (game->net_state != NET_STATE_IDLE) {
        game->net_wait_seconds += delta_time;

        uint8_t  rejected_type = 0;
        uint16_t retry_ms      = 0;
        int rejected = network_get_rate_limit_notice(&rejected_type, &retry_ms);

        if (rejected || game->net_wait_seconds > NET_REQUEST_TIMEOUT_SECONDS) {
            CLOG_WARN("[SERVER_LIST] World list request abandoned (%s)",
                   rejected ? "rate limited" : "timed out");
            game->net_state        = NET_STATE_IDLE;
            game->net_wait_seconds = 0.0f;
            s_refreshing           = 0;
            s_last_refresh         = now;   // wait a full interval before retrying
        }
    }

    /* Due immediately on entry, then once per interval -- and after a failure
     * of any kind, once the interval has passed again.
     *
     * "Never loaded" used to force a request on its own, and only a successful
     * send moved s_last_refresh, so a request that could not be sent was
     * retried on the very next frame and every frame after it. s_last_refresh
     * of zero is what makes the first attempt immediate; every other path
     * through here sets it. */
    int needs_refresh = !s_refreshing &&
                        (s_last_refresh == 0.0 ||
                         (now - s_last_refresh) >= SERVER_LIST_REFRESH_INTERVAL);

    if (needs_refresh && game->net_state == NET_STATE_IDLE) {
        CLOG_INFO("[SERVER_LIST] Requesting world list...");
        if (network_request_world_list()) {
            game->net_state = NET_STATE_WAITING_FOR_WORLDS;
            game->net_wait_seconds = 0.0f;
            s_refreshing = 1;
        } else {
            s_last_refresh = now;   // wait a full interval before trying again
        }
    }

    if (game->net_state == NET_STATE_WAITING_FOR_WORLDS) {
        if (network_get_world_list(&game->server_list.list)) {
            game->server_list.loaded = 1;
            game->net_state = NET_STATE_IDLE;
            s_last_refresh = now;
            s_refreshing   = 0;
            CLOG_INFO("[SERVER_LIST] Received %d worlds", game->server_list.list.count);
        }
    }
}

/**
 * Render the world list and process pointer selection and navigation.
 */
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

    // Refresh indicator (top-right of title bar, only while fetching)
    if (s_refreshing) {
        renderer_draw_text(panel_x + panel_w - 110, panel_y + 40, "Refreshing...");
    }

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
                /* Status 2 is full, and only status 0 was refused here -- so a
                 * player could pick a world the list had just drawn as full,
                 * walk the whole character screen, and be turned away at the
                 * far end of a world entry. The realm refuses these now too;
                 * this is the half of it that does not cost a round trip. */
                if (world->status == 0) {
                    CLOG_DEBUG("[SERVER_LIST] Cannot select offline server");
                } else if (world->status == 2) {
                    CLOG_DEBUG("[SERVER_LIST] Cannot select a full server");
                } else {
                    game->server_list.selected_index = i;
                    game_change_state(game, GAME_MODE_CHARACTER_SELECT);
                    CLOG_INFO("[SERVER_LIST] Selected: %s", world->name);
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
        game_change_state(game, GAME_MODE_MAIN_MENU);
    }
}

static void server_list_input(GameState* game, GLFWwindow* window, float delta_time) {
    (void)window;
    (void)delta_time;

    /* The edge, not the level: a keypress spans many frames, and testing the
     * level here meant one press could carry through a state change and be
     * seen again by whatever screen it landed on. */
    if (input_key_just_pressed(&game->input, GLFW_KEY_ESCAPE)) {
        game_change_state(game, GAME_MODE_MAIN_MENU);
    }
}

/** State-handler table for GAME_MODE_SERVER_LIST. */
const StateHandler g_state_server_list = {
    .enter = server_list_enter,
    .exit = server_list_exit,
    .update = server_list_update,
    .render = server_list_render,
    .handle_input = server_list_input
};
