/**
 * @file
 * Start the game client and coordinate its window, network session, and frame loop.
 */

#include <GLFW/glfw3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "game.h"
#include "renderer.h"
#include "render/gl_loader.h"
#include "render/ground_renderer.h"
#include "render/sky_renderer.h"
#include "render/precip_renderer.h"
#include "render/weather_map.h"
#include "render/shadow_map.h"
#include "render/lamp_shadow.h"
#include "render/character_renderer.h"
#include "render/city_renderer.h"
#include "world/city_heights.h"
#include "ui/map_view.h"
#include "ui/weather_window.h"
#include "ui/settings_menu.h"
#include "world/footing.h"
#include "world/world_clock.h"
#include "world/weather_state.h"
#include "world_seats.h"
#include "fps.h"
#include "network/network.h"
#include "network/net_connect.h"
#include "network/net_reconnect.h"
#include "ui/lost_connection.h"
#include "network/name_cache.h"
#include "game_types.h"
#include "core/client_log.h"
#include "states/chat_input.h"
#include "ui/settings_fields.h"
#include "states/chat_layout.h"
#include "ui/emote_hub.h"

/** Window the FPS counter averages over. Not a limit; see GameSettings. */
#define TARGET_FPS       60
#define GAME_VIEW_WIDTH  1920
#define GAME_VIEW_HEIGHT 1080

GameState* g_current_game = NULL;

// Scrolls the chat if the cursor is over it; returns 1 if the scroll was used.
static int scroll_chat(GameState* game, double yoffset) {
	chat_box box;

	if(!game->playing || game->playing->show_map || yoffset == 0.0)
		return 0;

	box = chat_box_at(&game->playing->chat.win, (float)game->camera.viewport_width,
			(float)game->camera.viewport_height);
	if(game->input.mouse_x < box.x || game->input.mouse_x > box.x + box.w ||
	   game->input.mouse_y < box.y || game->input.mouse_y > box.input_y)
		return 0;

	chat_scroll_by(&game->playing->chat, yoffset > 0.0 ? 2 : -2);

	return 1;
}

/**
 * Adjust the active map or camera zoom from a scroll event.
 *
 * @param window  Window whose user pointer must reference the active GameState.
 * @param yoffset  Signed scroll displacement reported by GLFW.
 */
static void scroll_callback(GLFWwindow* window, double xoffset, double yoffset) {
    (void)xoffset;
    GameState* game = (GameState*)glfwGetWindowUserPointer(window);

	if((game->mode == GAME_MODE_SETTINGS || game->show_settings) && sm_scroll((float)yoffset))
		return;

	if(game->playing && emote_hub_scroll((float)yoffset))
		return;

	if(game->playing && weather_window_scroll(game, (float)yoffset))
		return;

	if(scroll_chat(game, yoffset))
		return;

    if (game->playing && game->playing->show_map) {
        // Scroll zooms the full map (faster rate than camera)
        game->playing->map_zoom *= (1.0f + (float)yoffset * 0.15f);
        // Lower bound must reach continent scale: at base_radius 1050 world px,
        // 0.008 shows a ~131,000 px half-extent, enough to frame the whole
        // 15,400 x 7,700 tile world (246,400 x 123,200 px) at once.
        if (game->playing->map_zoom < MAP_ZOOM_MIN) game->playing->map_zoom = MAP_ZOOM_MIN;
        if (game->playing->map_zoom > MAP_ZOOM_MAX) game->playing->map_zoom = MAP_ZOOM_MAX;
    } else {
        camera_zoom_by(&game->camera, (float)yoffset);
    }
}

static void char_callback(GLFWwindow* window, unsigned int codepoint) {
	GameState* game = (GameState*)glfwGetWindowUserPointer(window);

	if(!game || !game->playing || !game->playing->chat.is_typing)
		return;

	chat_type(&game->playing->chat, codepoint);
}

/**
 * Populate connection settings from the environment and command line.
 *
 * @param argv  Argument vector containing optional player, user, and server values.
 * @param game  Game state to initialize with parsed connection settings.
 */
static void parse_arguments(int argc, char* argv[], GameState* game) {
    memset(game->session_key, 0, sizeof(game->session_key));
    memset(game->username, 0, sizeof(game->username));
    memset(game->realm_ip, 0, sizeof(game->realm_ip));
    game->account_id = 0;
    game->realm_port = 7777;
    game->network_connected = 0;
    snprintf(game->network_status, sizeof(game->network_status), "Not connected");

    // Session key comes from the environment variable set by the Launcher,
    // not from argv — keeps it out of the process command line.
    const char* env_session = getenv("MMO_SESSION");
    if (env_session) {
        strncpy(game->session_key, env_session, 64);
        game->session_key[64] = '\0';
    }

    CLOG_INFO("[ARGS] Parsing %d arguments", argc);

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--playerid=", 11) == 0) {
            char* end = NULL;
            long id = strtol(argv[i] + 11, &end, 10);
            game->account_id = (end != argv[i] + 11 && id > 0) ? (uint32_t)id : 0;
        }
        else if (strncmp(argv[i], "--username=", 11) == 0) {
            const char* start = argv[i] + 11;
            size_t len = strlen(start);

            /* Strip a surrounding pair of quotes, then bound the copy.
             *
             * The quoted branch used to copy `len` bytes into a 32-byte field
             * and write a terminator at index len, with nothing capping len at
             * all -- so --username="<33 or more characters>" wrote over
             * realm_ip and realm_port, which sit immediately after it in
             * GameState. The unquoted branch was capped; this one was not. */
            if (len >= 2 && start[0] == '"' && start[len - 1] == '"') {
                start++;
                len -= 2;
            }
            if (len > sizeof(game->username) - 1) len = sizeof(game->username) - 1;
            memcpy(game->username, start, len);
            game->username[len] = '\0';
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
    
    CLOG_INFO("[ARGS] Session: %s", game->session_key[0] ? "provided" : "(none)");
    CLOG_INFO("[ARGS] Account: %u", game->account_id);
    CLOG_INFO("[ARGS] Username: %s", game->username[0] ? game->username : "(none)");
    CLOG_INFO("[ARGS] Server: %s:%d", game->realm_ip[0] ? game->realm_ip : "(none)", game->realm_port);
}

/**
 * Decode hexadecimal byte pairs into a bounded binary buffer.
 *
 * @param hex  NUL-terminated hexadecimal input with two characters per byte.
 * @param bin  Destination buffer for decoded bytes.
 * @param bin_size  Capacity of bin in bytes.
 */
static void hex_to_binary(const char* hex, char* bin, size_t bin_size) {
    size_t hex_len = strlen(hex);
    size_t bin_len = hex_len / 2;
    if (bin_len > bin_size) bin_len = bin_size;
    
    for (size_t i = 0; i < bin_len; i++) {
        char byte[3] = {hex[i * 2], hex[i * 2 + 1], '\0'};
        bin[i] = (char)strtol(byte, NULL, 16);
    }
}

/**
 * Connect the client to its configured realm server.
 *
 * @param game  Game state containing the session key, account, and realm endpoint.
 */
/** Start connecting to the realm. Returns immediately; the frame loop finishes it.
 *
 * This used to be the blocking form: a blocking connect() followed by up to ten
 * seconds of Sleep(10) polling, called from inside the frame loop on a five
 * second timer. With the realm down, the window stopped answering the operating
 * system for the whole of that, every five seconds.
 *
 * @return 1 when an attempt is in flight, otherwise 0.
 */
static int connect_to_realm(GameState* game) {
    if (game->session_key[0] == '\0' || game->account_id == 0 || game->realm_ip[0] == '\0') {
        CLOG_WARN("[NETWORK] Missing connection parameters");
        snprintf(game->network_status, sizeof(game->network_status), "Missing parameters");
        return 0;
    }

    char binary_key[32] = {0};
    hex_to_binary(game->session_key, binary_key, 32);

    if (!network_begin_realm_connect(game->realm_ip, (uint16_t)game->realm_port,
                                     binary_key, game->account_id)) {
        game->network_connected = 0;
        snprintf(game->network_status, sizeof(game->network_status),
                 "Connection failed: %s", network_connect_message());
        return 0;
    }

    snprintf(game->network_status, sizeof(game->network_status),
             "Connecting to %s:%d...", game->realm_ip, game->realm_port);
    return 1;
}

/** Send the player out of the world after a connection that could not be recovered.
 *
 * They land on the world list, where the menu retry below reconnects to the
 * realm on its own with the session the launcher handed over, so there is
 * nothing to log in to again. Only when the login itself has ended is that not
 * so, and the dialog says it.
 */
static void leave_world_after_lost_connection(GameState* game) {
	int login_ended = net_reconnect_failure() == RECONNECT_FAIL_LOGIN;

	CLOG_WARN("[NETWORK] Could not get back into the world (%s)",
	          login_ended ? "the login has ended" : "out of time");

	// Idle again, or the menu's realm connect would never be polled
	net_reconnect_forget();
	game->network_connected = 0;
	snprintf(game->network_status, sizeof(game->network_status), "Disconnected");

	game_change_state(game, GAME_MODE_SERVER_LIST);
	lost_connection_show(login_ended);
}

/** Finish whatever connection attempt is in flight, without waiting on it. */
static void poll_realm_connect(GameState* game) {
    switch (network_connect_poll()) {
        case NET_CONNECT_SUCCEEDED:
            game->network_connected = 1;
            snprintf(game->network_status, sizeof(game->network_status),
                     "Connected to %s:%d", game->realm_ip, game->realm_port);
            break;
        case NET_CONNECT_FAILED:
            game->network_connected = 0;
            snprintf(game->network_status, sizeof(game->network_status),
                     "%s", network_connect_message());
            break;
        default:
            break;
    }
}

// Hand the time of day the server sent to the world clock
static void take_world_time(void) {
	uint32_t time_of_day_ms;
	uint32_t day_length_ms;

	if(!network_get_world_time(&time_of_day_ms, &day_length_ms))
		return;
	if(world_clock_sync(time_of_day_ms, day_length_ms, glfwGetTime()))
		CLOG_INFO("[CLOCK] Time of day %u of %u ms", time_of_day_ms, day_length_ms);
	else
		CLOG_WARN("[CLOCK] Refused time of day %u of %u ms", time_of_day_ms, day_length_ms);
}

// Hand the forecast the server sent to the weather state
static void take_weather(void) {
	WeatherForecast forecast;

	if(!network_get_weather(&forecast))
		return;
	if(weather_state_apply(&forecast, glfwGetTime())) {
		CLOG_INFO("[WEATHER] Forecast for day %u, %d regions", (unsigned)forecast.day_index,
		          forecast.region_count);
	} else {
		CLOG_WARN("[WEATHER] Refused a forecast with %d regions", forecast.region_count);
	}
}

/**
 * Run the game client until its window closes or the game requests shutdown.
 *
 * @param argv  Argument vector containing optional connection parameters.
 * @return      Zero after normal shutdown, or -1 when GLFW or window creation fails.
 */
int main(int argc, char* argv[]) {
    /* Before anything that might have something to say. A released client has
     * no console, so until this existed a player's disconnect left nothing at
     * all to diagnose. */
    client_log_init();

    CLOG_INFO("=== MMO Client starting ===");
    
    // Initialize GLFW
    if (!glfwInit()) {
        CLOG_ERROR("Failed to initialize GLFW");
        return -1;
    }
    
    // Get monitor info
    GLFWmonitor* monitor = glfwGetPrimaryMonitor();
    const GLFWvidmode* mode = glfwGetVideoMode(monitor);

    CLOG_INFO("Monitor: %dx%d @ %dHz", mode->width, mode->height, mode->refreshRate);
	sf_set_screen_size(mode->width, mode->height);

    // Create window hints
    glfwWindowHint(GLFW_RED_BITS, mode->redBits);
    glfwWindowHint(GLFW_GREEN_BITS, mode->greenBits);
    glfwWindowHint(GLFW_BLUE_BITS, mode->blueBits);
    glfwWindowHint(GLFW_REFRESH_RATE, mode->refreshRate);
    gl_loader_window_hints();

	// Borderless at the monitor size; a saved resolution applies after settings load
	glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
	int window_width = mode->width;
	int window_height = mode->height;

    CLOG_INFO("Window size: %dx%d", window_width, window_height);

    GLFWwindow* window = glfwCreateWindow(window_width, window_height, "MMO Game", NULL, NULL);
    if (!window) {
        /* The usual cause is a driver without an OpenGL 3.3 compatibility
         * context, which gl_loader_window_hints() asked for. */
        CLOG_ERROR("Failed to create window (OpenGL 3.3 compatibility context unavailable?)");
        glfwTerminate();
        return -1;
    }

	// Cover the monitor
	int monitor_x, monitor_y;
	glfwGetMonitorPos(monitor, &monitor_x, &monitor_y);
	glfwSetWindowPos(window, monitor_x, monitor_y);

    glfwMakeContextCurrent(window);
    g_window = window;

    /* The ground draws through a shader, so OpenGL 3.3 is the floor. Say so
     * plainly rather than opening a window that draws nothing. */
    if (!gl_loader_init() || !ground_renderer_init()) {
        CLOG_ERROR("This computer's graphics driver does not provide OpenGL 3.3, "
                   "which the game needs. Updating the graphics driver usually fixes this.");
        glfwDestroyWindow(window);
        glfwTerminate();
        return -1;
    }
	// The sky is scenery: without it the world keeps its flat colour
	weather_map_init();
	sky_renderer_init();
	precip_renderer_init();
	// Shadows are optional too: without them the world is lit but casts none
	shadow_map_init();
	lamp_shadow_init();

    /* Players and NPCs are 3D models. A missing or broken model file is not
     * fatal: characters then draw as flat placeholder cards. */
    if (!character_renderer_init())
        CLOG_ERROR("[CHARACTER] Characters will draw as placeholders");

	// The city scene, its baked data and the tile world load when a character enters the game

    /* VSync and the frame cap are settings now, applied by
     * game_settings_apply() below once the loaded settings exist. This used to
     * be an unconditional glfwSwapInterval(0) with nothing limiting the loop
     * afterwards. */
    
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
    CLOG_INFO("[NETWORK] Initializing...");
    if (!network_init(game.account_id)) {
        CLOG_ERROR("[NETWORK] Init failed");
        snprintf(game.network_status, sizeof(game.network_status), "Network init failed");
    } else {
        connect_to_realm(&game);
    }
    
    // Set callbacks
    glfwSetWindowUserPointer(window, &game);
    glfwSetScrollCallback(window, scroll_callback);
    glfwSetCharCallback(window, char_callback);
    
    CLOG_INFO("=== Entering Main Loop ===");
    if (game.settings.vsync) {
        CLOG_INFO("Frame pacing: VSync (display refresh)");
    } else if (game.settings.fps_limit > 0) {
        CLOG_INFO("Frame pacing: capped at %d FPS", game.settings.fps_limit);
    } else {
        CLOG_INFO("Frame pacing: uncapped");
    }
	CLOG_INFO("Game viewport: %dx%d in a %dx%d window", GAME_VIEW_WIDTH, GAME_VIEW_HEIGHT,
	          window_width, window_height);
    CLOG_INFO("Network: %s\n", game.network_status);
    
    while (!glfwWindowShouldClose(window) && game.is_running) {
        /* Taken before any work, so the cap below measures a whole frame
         * rather than only the part after the last swap. */
        const double frame_start = glfwGetTime();

        // Get framebuffer size
        int fb_width, fb_height;
        glfwGetFramebufferSize(window, &fb_width, &fb_height);

		// The game always draws 16:9; any other window shape gets bars
		letterbox_rect view_box = letterbox_fit(fb_width, fb_height,
		                                        GAME_VIEW_WIDTH, GAME_VIEW_HEIGHT);
		glViewport(view_box.x, view_box.y, view_box.w, view_box.h);
        
        // Update FPS
        fps_update(&fps);
        float delta_time = (float)fps_get_delta_time(&fps);
        world_clock_tick(glfwGetTime());
        weather_state_tick(glfwGetTime());
        
        /* Network: never more than a frame's worth of work.
         *
         * Every branch here returns immediately. A connection attempt in
         * flight is advanced by one non-blocking step; a dropped in-world
         * session is handed to the reconnect supervisor, which walks the
         * realm -> ticket -> world sequence across frames with its own
         * backoff. Nothing in this loop waits on the network. */
        double frame_now = glfwGetTime();

        /* Only the realm handshake is the frame loop's to finish.
         *
         * network_connect_poll() reports SUCCEEDED and FAILED exactly once, so
         * whoever started an attempt has to be the one that polls it. The test
         * here was network_connect_in_flight(), which is equally true of a
         * world handshake character select had started -- and this block runs
         * before game_update(), so it took that result every time: the frame
         * loop marked the client "Connected to <realm>" while character select,
         * polling second, saw NET_CONNECT_IDLE and sat in "Entering world..."
         * until its own timeout gave up. World entry could not complete at all.
         *
         * The outer test stays network_connect_in_flight(): while ANY attempt
         * is in flight there is no live session to read or ping through. */
        if (network_connect_in_flight()) {
            if (network_realm_connect_in_flight() && net_reconnect_phase() == RECONNECT_IDLE)
                poll_realm_connect(&game);
        } else if (game.network_connected) {
            network_update_with_ping(game.mode);
            take_world_time();
            take_weather();

            if (!network_is_connected()) {
                game.network_connected = 0;
                snprintf(game.network_status, sizeof(game.network_status), "Disconnected");
                CLOG_WARN("[NETWORK] Lost connection");

                /* In world, a drop is recoverable: the supervisor goes back to
                 * the realm for a fresh ticket and rejoins. It used to end the
                 * session outright, and the player's only recourse was to
                 * restart the client. */
                if (game.mode == GAME_MODE_PLAYING) net_reconnect_begin(frame_now);
            }
        }

        if (game.mode == GAME_MODE_PLAYING) {
            if (net_reconnect_phase() != RECONNECT_IDLE) {
                net_reconnect_update(frame_now);
                snprintf(game.network_status, sizeof(game.network_status),
                         "%s", net_reconnect_status());
                game.network_connected = network_is_connected();
            }

			// Recovery ran out of road: leave the world and tell the player
			if(net_reconnect_phase() == RECONNECT_GIVEN_UP)
				leave_world_after_lost_connection(&game);
        } else if (!game.network_connected && !network_connect_in_flight() &&
                   !lost_connection_login_ended() &&
                   (game.mode == GAME_MODE_MAIN_MENU ||
                    game.mode == GAME_MODE_SERVER_LIST ||
                    game.mode == GAME_MODE_CHARACTER_SELECT)) {
            /* Retry on the menu screens, backing off rather than retrying on a
             * flat five-second timer against a realm that is down. */
            static double next_retry = 0.0;
            static double retry_backoff = 0.0;

            if (frame_now >= next_retry) {
                if (retry_backoff <= 0.0) retry_backoff = 1.0;
                else                      retry_backoff *= 2.0;
                if (retry_backoff > 30.0) retry_backoff = 30.0;

                next_retry = frame_now + retry_backoff;
                if (connect_to_realm(&game)) retry_backoff = 0.0;
            }
        }
        
        // FPS display
        static double last_fps_print = 0.0;
        if (glfwGetTime() - last_fps_print >= 1.0) {
            CLOG_DEBUG("FPS: %.1f | %s", fps_get_current(&fps),
                   game.network_connected ? "Online" : "Offline");
            last_fps_print = glfwGetTime();
        }
        
        // F11 — toggle fullscreen (global shortcut, any state)
        static int prev_f11 = 0;
        int cur_f11 = glfwGetKey(window, GLFW_KEY_F11) == GLFW_PRESS;
        if (cur_f11 && !prev_f11) {
			game_toggle_fullscreen(&game.settings);
            game_settings_apply(&game.settings);
            game_settings_save(&game.settings, SETTINGS_PATH);
        }
        prev_f11 = cur_f11;

        /* Send any names the render pass asked about, as one packet.
         *
         * Batched here rather than sent from the lookup: walking into a crowd
         * of thirty strangers asks thirty questions in one frame, and thirty
         * packets to answer them would be worse than the field in the
         * broadcast this replaces. */
        if (game.network_connected) name_cache_flush_requests();

        game_handle_input(&game, window, delta_time);
        game_update(&game, delta_time);
        character_renderer_set_animal_features(game.settings.show_animal_features);
        game_render(&game);
        
        /* Update the window title, on change only.
         *
         * This used to rebuild the string with snprintf and call
         * glfwSetWindowTitle every frame -- a window-manager round trip per
         * frame -- for a value with exactly two states that changes a handful
         * of times in a session. The initial -1 makes the first pass write it
         * whichever state it starts in. */
        static int shown_connected = -1;
        if (game.network_connected != shown_connected) {
            shown_connected = game.network_connected;
            glfwSetWindowTitle(window, shown_connected ? "Multiverse MMO - Online"
                                                       : "Multiverse MMO - Offline");
        }
        
		renderer_clear_bars(fb_width, fb_height, &view_box);
        glfwSwapBuffers(window);
        glfwPollEvents();

        /* The frame limiter, for when VSync is off.
         *
         * With VSync on, glfwSwapBuffers() has already blocked until the
         * display was ready and there is nothing to do here. With it off, and
         * with no limit configured, the loop runs as fast as the GPU allows --
         * which on a menu screen is several hundred redraws a second of the
         * same image, for nothing but heat.
         *
         * Sleeping for the whole remainder would overshoot: a sleep is a
         * minimum, not an exact duration. So it sleeps for all but the last
         * millisecond and spins out the rest, which holds the cap within a
         * frame without burning a core waiting for it. */
        if (!game.settings.vsync && game.settings.fps_limit > 0) {
            const double frame_seconds = 1.0 / (double)game.settings.fps_limit;
            double deadline = frame_start + frame_seconds;
            double remaining = deadline - glfwGetTime();

            if (remaining > 0.001) {
                struct timespec nap = {
                    .tv_sec  = 0,
                    .tv_nsec = (long)((remaining - 0.001) * 1e9),
                };
                nanosleep(&nap, NULL);
            }
            while (glfwGetTime() < deadline) { /* the last fraction of a ms */ }
        }
    }
    
    CLOG_INFO("=== Shutting down ===");

    // Drain pending window messages before teardown.
    //
    // Nothing pumps the message queue once the loop exits, and the window stops
    // responding the moment that happens -- so any teardown step that waits on a
    // message shows up as a frozen window rather than a slow exit. Draining here
    // lets queued close/focus messages settle first.
    for (int i = 0; i < 3; i++) glfwPollEvents();

    // Each step announces itself and flushes, so a hang names the step it is in
    // instead of leaving a silent frozen window.
    CLOG_INFO("[SHUTDOWN] network...");
    if (game.network_connected) {
        network_disconnect();
    }
    network_cleanup();

    CLOG_INFO("[SHUTDOWN] game...");
    game_cleanup(&game);

    CLOG_INFO("[SHUTDOWN] renderer...");
    character_renderer_shutdown();
    map_view_shutdown();
    city_renderer_shutdown();
    city_heights_free();
    footing_free();
    lamp_shadow_shutdown();
    shadow_map_shutdown();
    precip_renderer_shutdown();
    weather_map_shutdown();
    sky_renderer_shutdown();
    ground_renderer_shutdown();
    renderer_cleanup();

    CLOG_INFO("[SHUTDOWN] glfw...");
    glfwTerminate();

    g_current_game = NULL;
    CLOG_INFO("Goodbye!");

    /* Last, so anything the teardown above had to say is in the file. */
    client_log_close();

    return 0;
}
