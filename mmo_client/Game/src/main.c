#include <GLFW/glfw3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"
#include "renderer.h"
#include "fps.h"
#include "network/network.h"
#include "game_types.h"

// ============================================================================
// CONSTANTS
// ============================================================================

#define TARGET_FPS       60
#define GAME_VIEW_WIDTH  1920
#define GAME_VIEW_HEIGHT 1080

// ============================================================================
// GLOBALS
// ============================================================================

GameState* g_current_game = NULL;

// ============================================================================
// CALLBACKS
// ============================================================================

static void scroll_callback(GLFWwindow* window, double xoffset, double yoffset) {
    (void)xoffset;
    GameState* game = (GameState*)glfwGetWindowUserPointer(window);

    game->camera.zoom *= (1.0f + (float)yoffset * 0.1f);

    // Clamp zoom between min and max (don't wrap around)
    if (game->camera.zoom < 1.0f) game->camera.zoom = 1.0f;
    if (game->camera.zoom > 2.0f) game->camera.zoom = 2.0f;
}

static void char_callback(GLFWwindow* window, unsigned int codepoint) {
    GameState* game = (GameState*)glfwGetWindowUserPointer(window);
    if (!game || !game->chat.is_typing) return;

    // Only handle printable ASCII
    if (codepoint >= 32 && codepoint < 127) {
        if (game->chat.input_len < MAX_CHAT_INPUT_LEN - 1) {
            game->chat.input_buf[game->chat.input_len] = (char)codepoint;
            game->chat.input_len++;
            game->chat.input_buf[game->chat.input_len] = '\0';
        }
    }
}

// ============================================================================
// ARGUMENT PARSING
// ============================================================================

static void parse_arguments(int argc, char* argv[], GameState* game) {
    memset(game->session_key, 0, sizeof(game->session_key));
    memset(game->username, 0, sizeof(game->username));
    memset(game->realm_ip, 0, sizeof(game->realm_ip));
    game->account_id = 0;
    game->realm_port = 7777;
    game->network_connected = 0;
    snprintf(game->network_status, sizeof(game->network_status), "Not connected");
    
    printf("[ARGS] Parsing %d arguments\n", argc);
    
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--session=", 10) == 0) {
            strncpy(game->session_key, argv[i] + 10, 64);
            game->session_key[64] = '\0';
        }
        else if (strncmp(argv[i], "--playerid=", 11) == 0) {
            char* end = NULL;
            long id = strtol(argv[i] + 11, &end, 10);
            game->account_id = (end != argv[i] + 11 && id > 0) ? (uint32_t)id : 0;
        }
        else if (strncmp(argv[i], "--username=", 11) == 0) {
            const char* start = argv[i] + 11;
            if (start[0] == '"') {
                start++;
                size_t len = strlen(start);
                if (len > 0 && start[len - 1] == '"') len--;
                strncpy(game->username, start, len);
                game->username[len] = '\0';
            } else {
                strncpy(game->username, start, 31);
                game->username[31] = '\0';
            }
        }
        else if (strncmp(argv[i], "--server=", 9) == 0) {
            char server[32];
            strncpy(server, argv[i] + 9, 31);
            server[31] = '\0';
            
            char* colon = strchr(server, ':');
            if (colon) {
                *colon = '\0';
                strncpy(game->realm_ip, server, 15);
                game->realm_ip[15] = '\0';
                char* end2 = NULL;
                long port = strtol(colon + 1, &end2, 10);
                game->realm_port = (end2 != colon + 1 && port >= 1 && port <= 65535) ? (int)port : 7777;
            } else {
                strncpy(game->realm_ip, server, 15);
                game->realm_ip[15] = '\0';
            }
        }
    }
    
    printf("[ARGS] Session: %s\n", game->session_key[0] ? game->session_key : "(none)");
    printf("[ARGS] Account: %u\n", game->account_id);
    printf("[ARGS] Username: %s\n", game->username[0] ? game->username : "(none)");
    printf("[ARGS] Server: %s:%d\n", game->realm_ip[0] ? game->realm_ip : "(none)", game->realm_port);
}

static void hex_to_binary(const char* hex, char* bin, size_t bin_size) {
    size_t hex_len = strlen(hex);
    size_t bin_len = hex_len / 2;
    if (bin_len > bin_size) bin_len = bin_size;
    
    for (size_t i = 0; i < bin_len; i++) {
        char byte[3] = {hex[i * 2], hex[i * 2 + 1], '\0'};
        bin[i] = (char)strtol(byte, NULL, 16);
    }
}

// ============================================================================
// NETWORK CONNECTION
// ============================================================================

static void connect_to_realm(GameState* game) {
    if (game->session_key[0] == '\0' || game->account_id == 0 || game->realm_ip[0] == '\0') {
        printf("[NETWORK] Missing connection parameters\n");
        snprintf(game->network_status, sizeof(game->network_status), "Missing parameters");
        return;
    }
    
    char binary_key[32] = {0};
    hex_to_binary(game->session_key, binary_key, 32);
    
    if (network_connect_to_realm(game->realm_ip, (uint16_t)game->realm_port, 
                                 binary_key, game->account_id)) {
        game->network_connected = 1;
        snprintf(game->network_status, sizeof(game->network_status),
                "Connected to %s:%d", game->realm_ip, game->realm_port);
    } else {
        game->network_connected = 0;
        snprintf(game->network_status, sizeof(game->network_status), "Connection failed");
    }
}

// ============================================================================
// MAIN
// ============================================================================

int main(int argc, char* argv[]) {
    printf("=== MMO Client Starting ===\n");
    
    // Initialize GLFW
    if (!glfwInit()) {
        fprintf(stderr, "Failed to initialize GLFW\n");
        return -1;
    }
    
    // Get monitor info
    GLFWmonitor* monitor = glfwGetPrimaryMonitor();
    const GLFWvidmode* mode = glfwGetVideoMode(monitor);

    printf("Monitor: %dx%d @ %dHz\n", mode->width, mode->height, mode->refreshRate);

    // Create window hints
    glfwWindowHint(GLFW_RED_BITS, mode->redBits);
    glfwWindowHint(GLFW_GREEN_BITS, mode->greenBits);
    glfwWindowHint(GLFW_BLUE_BITS, mode->blueBits);
    glfwWindowHint(GLFW_REFRESH_RATE, mode->refreshRate);

    // Calculate window size (90% of monitor size)
    int window_width = (int)(mode->width * 0.9f);
    int window_height = (int)(mode->height * 0.9f);

    printf("Window size: %dx%d\n", window_width, window_height);

    GLFWwindow* window = glfwCreateWindow(window_width, window_height, "MMO Game", NULL, NULL);
    if (!window) {
        fprintf(stderr, "Failed to create window\n");
        glfwTerminate();
        return -1;
    }

    // Center the window on the monitor
    int monitor_x, monitor_y;
    glfwGetMonitorPos(monitor, &monitor_x, &monitor_y);
    glfwSetWindowPos(window, 
                    monitor_x + (mode->width - window_width) / 2,
                    monitor_y + (mode->height - window_height) / 2);

    glfwMakeContextCurrent(window);
    glfwSwapInterval(0);  // Disable VSync
    g_window = window;
    
    // Initialize renderer with CONSTANT viewport (1920x1080)
    renderer_init(GAME_VIEW_WIDTH, GAME_VIEW_HEIGHT);
    renderer_font_init("Game/Sprites/Fonts/ARIAL.TTF", 22.0f);
    
    // Initialize FPS counter
    FPSCounter fps;
    fps_init(&fps, TARGET_FPS);
    
    // Initialize game state with CONSTANT viewport (1920x1080)
    GameState game;
    game_init(&game, GAME_VIEW_WIDTH, GAME_VIEW_HEIGHT);
    g_current_game = &game;
    
    // Parse command line
    parse_arguments(argc, argv, &game);
    
    // Initialize network
    printf("[NETWORK] Initializing...\n");
    if (!network_init(game.account_id)) {
        fprintf(stderr, "[NETWORK] Init failed\n");
        snprintf(game.network_status, sizeof(game.network_status), "Network init failed");
    } else {
        connect_to_realm(&game);
    }
    
    // Set callbacks
    glfwSetWindowUserPointer(window, &game);
    glfwSetScrollCallback(window, scroll_callback);
    glfwSetCharCallback(window, char_callback);
    
    printf("\n=== Entering Main Loop ===\n");
    printf("Target FPS: %d\n", TARGET_FPS);
    printf("Game viewport: %dx%d\n", window_width, window_height);
    printf("Network: %s\n\n", game.network_status);
    
    // ========================================================================
    // MAIN LOOP
    // ========================================================================
    
    while (!glfwWindowShouldClose(window) && game.is_running) {
        // Get framebuffer size
        int fb_width, fb_height;
        glfwGetFramebufferSize(window, &fb_width, &fb_height);
        glViewport(0, 0, fb_width, fb_height);
        
        // Update FPS
        fps_update(&fps);
        float delta_time = (float)fps_get_delta_time(&fps);
        
        // Update network
        if (game.network_connected) {
            network_update_with_ping(game.mode);
            
            if (!network_is_connected()) {
                game.network_connected = 0;
                snprintf(game.network_status, sizeof(game.network_status), "Disconnected");
                printf("[NETWORK] Lost connection\n");
            }
        } else if (game.mode == GAME_MODE_MAIN_MENU || 
                   game.mode == GAME_MODE_SERVER_LIST ||
                   game.mode == GAME_MODE_CHARACTER_SELECT) {
            // Try to reconnect on menu screens
            static double last_reconnect = 0.0;
            double now = glfwGetTime();
            if (now - last_reconnect > 5.0) {
                connect_to_realm(&game);
                last_reconnect = now;
            }
        }
        
        // FPS display
        static double last_fps_print = 0.0;
        if (glfwGetTime() - last_fps_print >= 1.0) {
            printf("FPS: %.1f | %s\n", fps_get_current(&fps),
                   game.network_connected ? "Online" : "Offline");
            last_fps_print = glfwGetTime();
        }
        
        // F11 — toggle fullscreen (global shortcut, any state)
        static int prev_f11 = 0;
        int cur_f11 = glfwGetKey(window, GLFW_KEY_F11) == GLFW_PRESS;
        if (cur_f11 && !prev_f11) {
            game.settings.fullscreen = !game.settings.fullscreen;
            game_settings_apply(&game.settings);
            game_settings_save(&game.settings, SETTINGS_PATH);
        }
        prev_f11 = cur_f11;

        // === GAME LOOP ===
        game_handle_input(&game, window, delta_time);
        game_update(&game, delta_time);
        game_render(&game);
        
        // Update window title
        char title[128];
        snprintf(title, sizeof(title), "Multiverse MMO - %s",
                game.network_connected ? "Online" : "Offline");
        glfwSetWindowTitle(window, title);
        
        glfwSwapBuffers(window);
        glfwPollEvents();
    }
    
    // ========================================================================
    // CLEANUP
    // ========================================================================
    
    printf("\n=== Shutting Down ===\n");
    
    if (game.network_connected) {
        network_disconnect();
    }
    network_cleanup();
    
    game_cleanup(&game);
    renderer_cleanup();
    glfwTerminate();
    
    g_current_game = NULL;
    printf("Goodbye!\n");
    
    return 0;
}