#ifndef STATE_PLAYING_INTERNAL_H
#define STATE_PLAYING_INTERNAL_H

/**
 * @file
 * The seams between the gameplay state's five translation units.
 *
 * state_playing.c was 1,835 lines and owned the whole of playing: the
 * simulation step, the world draw, ten screen-space overlays, and a
 * six-hundred-line input router. Nothing in it was wrong, but nothing in it
 * could be found either -- a change to the map overlay meant scrolling past
 * movement prediction and chat parsing to reach it, and the two nearest things
 * to whatever you were editing were usually unrelated to it.
 *
 * It is now split by what changes together rather than by what happens to be
 * called from the same place:
 *
 *   state_playing.c         the state handler table, enter and exit
 *   state_playing_update.c  the simulation step and movement sync
 *   state_playing_render.c  the world-space pass and the frame's running order
 *   state_playing_hud.c     the screen-space overlays
 *   state_playing_input.c   the input router
 *
 * This header is what they share, and it is deliberately small: five entry
 * points, the chat box's geometry, and the movement-sync constants. Everything
 * else each unit needs, it owns. Nothing outside these five files includes it.
 *
 * The per-session state these used to keep in file-scope statics -- movement
 * sync and the hovered buff icon -- is in PlayingState now, which is what made
 * the split possible without either duplicating it or exporting it.
 */

#include "game_types.h"

#include <GLFW/glfw3.h>

/* --- The state handler's five entry points -------------------------------
 *
 * Declared here rather than made static, because they no longer live in the
 * same file as the table that names them (state_playing.c).
 */

void playing_enter(GameState* game);
void playing_exit(GameState* game);
void playing_update(GameState* game, float delta_time);
void playing_render(GameState* game);
void playing_input(GameState* game, GLFWwindow* window, float delta_time);

/* --- The world-space pass (state_playing_render.c) ----------------------- */

void playing_render_projectiles(GameState* game);
void playing_render_telegraphs(GameState* game);
void playing_render_zones(GameState* game);
void playing_render_heal_vfxs(GameState* game);
void playing_render_ground_items(GameState* game);

/* --- The screen-space overlays (state_playing_hud.c) ---------------------
 *
 * Called by playing_render() in the order the player sees them, which is the
 * order they are listed in.
 */

void playing_render_big_map(GameState* game);
void playing_render_chat(GameState* game);
void playing_render_party_frames(GameState* game);
void playing_render_party_invite(GameState* game);
void playing_render_death_screen(GameState* game);
void playing_render_level_up(GameState* game);
void playing_render_reward_notifications(GameState* game);
void playing_render_buff_tooltip(const GameState* game);
void playing_render_pause_overlay(GameState* game);
void playing_render_settings_overlay(GameState* game);

/* --- Movement sync -------------------------------------------------------
 *
 * Read by the update step, which is the only thing that sends a position.
 */

/** Seconds between position packets, at most. */
#define MOVE_INTERVAL       0.05f

/** World pixels of movement below which no packet is sent at all. */
#define MOVE_THRESHOLD      0.5f

/** Seconds of total silence after which something is sent regardless. */
#define HEARTBEAT_INTERVAL  15.0f

/* --- The chat box --------------------------------------------------------
 *
 * Shared because the overlay draws it and the input router hit-tests it, and
 * the two must agree about where it is. They were two copies of these numbers
 * in one file, which is one file away from being two copies in two files.
 */

#define CHAT_X       14.0f
#define CHAT_W       420.0f
#define CHAT_LINE_H  20.0f
#define CHAT_LINES   8
#define CHAT_PAD     8.0f
#define CHAT_INBOX_H 30.0f

#endif // STATE_PLAYING_INTERNAL_H
