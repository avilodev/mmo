/**
 * @file
 * Maintain client player movement and server position state. The body is
 * drawn by the playing state (state_playing_render.c).
 */

#include "player.h"
#include "world/world.h"
#include "input.h"
#include "core/keybinds.h"
#include "camera/camera.h"
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "core/client_log.h"

/** Bound the samples one replay may take.
 *
 * A tail longer than this is not a latency spike, it is a client that has been
 * predicting for seconds against a server that stopped agreeing -- and walking
 * it step by step would cost more than accepting a jump. Matches the server's
 * own COLLISION_PATH_MAX_STEPS in world_collision.c.
 */
#define PLAYER_REPLAY_MAX_STEPS 256

/**
 * Initialize an empty player state with the default movement speed.
 */
void player_init(PlayerState* player) {
    memset(player, 0, sizeof(PlayerState));
    player->speed = 200.0f;  // Default; overridden by server PACKET_PLAYER_STATS
    player->move_mode = LOCO_MODE_RUN;
}

/** Forget every unanswered proposal.
 *
 * Called wherever the position moves by something other than walking. The ring
 * exists to measure how far the player has walked since a given proposal, and
 * across a teleport that measurement is meaningless: a correction naming a
 * pre-jump sequence would replay a tail that includes the jump itself and land
 * the player somewhere neither end asked for. Dropping the history makes such
 * a correction unreconcilable, which is the honest answer -- it is taken as-is.
 */
static void forget_sent_moves(PlayerState* player) {
    memset(player->move_history, 0, sizeof(player->move_history));
    player->move_history_head = 0;
}

/**
 * Reset the player position and clear pending movement state.
 */
void player_reset_position(PlayerState* player, float x, float y) {
    player->x = x;
    player->y = y;
    player->vel_x = 0.0f;
    player->vel_y = 0.0f;
    locomotion_halt(&player->loco);
    player->needs_position_reset = 0;
    forget_sent_moves(player);
    CLOG_DEBUG("[PLAYER] Position reset to (%.1f, %.1f)", x, y);
}

/**
 * Move the body from the bound keys, GTA-style (player/locomotion.h), and
 * resolve collision per axis.
 *
 * The pace is the movement mode: run by default, Ctrl toggles walk. The keys
 * give a direction relative to the camera (D22); the body turns toward it and
 * moves the way it faces.
 *
 * @param world  Loaded world used for tile size and collision queries.
 * @param delta_time  Elapsed frame time in seconds.
 * @return      Nonzero when the body moved this frame; otherwise zero.
 */
int player_update_movement(PlayerState* player, const InputState* input,
                           const WorldState* world, float delta_time, float input_yaw) {
    // Screen-relative key direction: +x right, +y down
    float key_x = 0.0f, key_y = 0.0f;
    if (input_key_pressed(input, g_keybinds.move_up))    key_y -= 1.0f;
    if (input_key_pressed(input, g_keybinds.move_down))  key_y += 1.0f;
    if (input_key_pressed(input, g_keybinds.move_left))  key_x -= 1.0f;
    if (input_key_pressed(input, g_keybinds.move_right)) key_x += 1.0f;
    if (input_key_just_pressed(input, g_keybinds.toggle_walk))
        player->move_mode = (player->move_mode == LOCO_MODE_WALK) ? LOCO_MODE_RUN
                                                                  : LOCO_MODE_WALK;

    /* The keys are screen directions; turn them into the world direction that
     * looks the same way from where the camera now stands (D22). */
    float want_x = 0.0f, want_y = 0.0f;
    if (key_x != 0.0f || key_y != 0.0f)
        camera_math_rotate_input(input_yaw, key_x, key_y, &want_x, &want_y);

    float move_x, move_y;
    locomotion_step(&player->loco, want_x, want_y, player->move_mode, player->speed, delta_time,
                    &move_x, &move_y);

    // The body faces where it is heading, moving or not (D23)
    player->facing_x = sinf(player->loco.heading);
    player->facing_y = cosf(player->loco.heading);

    if (move_x == 0.0f && move_y == 0.0f) {
        player->vel_x = 0.0f;
        player->vel_y = 0.0f;
        return 0;
    }

    // Player is 2 tiles wide
    float half_size = world->tile_size * 1.0f;

    // Try X movement
    float new_x = player->x + move_x;
    if (!world_check_box_collision(world, new_x, player->y, half_size)) {
        player->x = new_x;
    } else {
        move_x = 0.0f;
    }

    // Try Y movement
    float new_y = player->y + move_y;
    if (!world_check_box_collision(world, player->x, new_y, half_size)) {
        player->y = new_y;
    } else {
        move_y = 0.0f;
    }

    /* Walking into a wall head-on stops the body, so it does not run on the
     * spot; brushing along one keeps the speed and slides. */
    if (move_x == 0.0f && move_y == 0.0f) locomotion_halt(&player->loco);

    // Calculate velocity for network sync
    if (delta_time > 0.0f) {
        player->vel_x = move_x / delta_time;
        player->vel_y = move_y / delta_time;
    }

    return (move_x != 0.0f || move_y != 0.0f);
}

/**
 * Record a position proposal, so a correction naming it can be reconciled.
 */
void player_record_sent_move(PlayerState* player, uint32_t sequence,
                             float x, float y) {
    if (!player || sequence == 0) return;

    PlayerMoveRecord* slot = &player->move_history[player->move_history_head];
    slot->sequence = sequence;
    slot->x        = x;
    slot->y        = y;

    player->move_history_head =
        (player->move_history_head + 1) % PLAYER_MOVE_HISTORY;
}

/** Find a recorded proposal, or NULL when it has aged out or never existed. */
static const PlayerMoveRecord* find_sent_move(const PlayerState* player,
                                              uint32_t sequence) {
    if (sequence == 0) return NULL;
    for (int i = 0; i < PLAYER_MOVE_HISTORY; i++) {
        const PlayerMoveRecord* rec = &player->move_history[i];
        if (rec->sequence == sequence) return rec;
    }
    return NULL;
}

/** Walk a displacement against the world, one axis at a time.
 *
 * The same shape player_update_movement() uses -- try X, keep it if the box
 * fits, then try Y -- so a replayed step lands where the original steps would
 * have, rather than by a different rule that could disagree with them.
 *
 * Sampled rather than applied in one jump: the tail is several frames of
 * movement, and a single step of that size can cross a wall entirely. Half a
 * tile is the same interval the server samples its own path checks at.
 */
static void walk_against_world(const WorldState* world, float half_size,
                               float* x, float* y, float dx, float dy) {
    float distance = sqrtf(dx * dx + dy * dy);
    if (distance <= 0.0f) return;

    float interval = (world->tile_size > 0) ? (float)world->tile_size * 0.5f : 8.0f;
    int steps = (int)(distance / interval) + 1;
    if (steps > PLAYER_REPLAY_MAX_STEPS) steps = PLAYER_REPLAY_MAX_STEPS;

    float step_x = dx / (float)steps;
    float step_y = dy / (float)steps;

    for (int i = 0; i < steps; i++) {
        float try_x = *x + step_x;
        if (!world_check_box_collision(world, try_x, *y, half_size)) *x = try_x;

        float try_y = *y + step_y;
        if (!world_check_box_collision(world, *x, try_y, half_size)) *y = try_y;
    }
}

int player_apply_correction(PlayerState* player, const WorldState* world,
                            uint32_t sequence, float x, float y) {
    if (!player) return 0;

    float before_x = player->x;
    float before_y = player->y;

    const PlayerMoveRecord* rec = find_sent_move(player, sequence);

    if (!rec || !world) {
        /* Nothing to reconcile against: a correction older than the ring, one
         * naming a sequence from a previous session, or a caller with no world
         * to check a replay against. Taking the server's word for it is the
         * behaviour this function had for every correction, and it is still
         * the right fallback -- the server is authoritative either way. */
        player->x = x;
        player->y = y;
        CLOG_DEBUG("[PLAYER] Correction for move %u applied without replay: "
                   "(%.1f, %.1f) -> (%.1f, %.1f)",
                   sequence, before_x, before_y, x, y);
        return 1;
    }

    /* Everything sent after the refused proposal, expressed as one
     * displacement. The refused move's own step is excluded by construction:
     * rec->{x,y} is the position that move claimed, so measuring from it
     * drops exactly the step the server would not accept and keeps every step
     * taken since. */
    float tail_dx = player->x - rec->x;
    float tail_dy = player->y - rec->y;

    float replay_x = x;
    float replay_y = y;
    walk_against_world(world, player_get_half_size(player, world->tile_size),
                       &replay_x, &replay_y, tail_dx, tail_dy);

    player->x = replay_x;
    player->y = replay_y;

    /* Sub-pixel is not a jerk. A correction the client would have agreed with
     * lands the replay within rounding of where the player already was, and
     * saying so is what distinguishes "the server disagreed" from "the server
     * confirmed us late" in a log. */
    float moved_x = player->x - before_x;
    float moved_y = player->y - before_y;
    int visible = (moved_x * moved_x + moved_y * moved_y) > (0.5f * 0.5f);

    if (visible) {
        CLOG_DEBUG("[PLAYER] Correction for move %u: server (%.1f, %.1f) + "
                   "%.1f,%.1f of replay -> (%.1f, %.1f), moved %.1fpx",
                   sequence, x, y, tail_dx, tail_dy, player->x, player->y,
                   sqrtf(moved_x * moved_x + moved_y * moved_y));
    } else {
        CLOG_TRACE("[PLAYER] Correction for move %u reconciled invisibly", sequence);
    }

    return visible;
}


/**
 * Copy server character data into the local player state.
 *
 * @param info  Complete character record received from the server.
 */
void player_load_info(PlayerState* player, const CharacterInfo* info) {
    memcpy(&player->info, info, sizeof(CharacterInfo));
    player->info_loaded = 1;
    
    // Set position from server data
    player->x = info->pos_x;
    player->y = info->pos_y;
    player->needs_position_reset = 1;
    forget_sent_moves(player);
    
    CLOG_INFO("[PLAYER] Info loaded: %s Lv.%u at (%.1f, %.1f)",
           info->name, info->level, info->pos_x, info->pos_y);
}

/**
 * Return the collision half-size used for a player.
 *
 * @param tile_size  World tile size in pixels.
 * @return      Collision half-size in world pixels.
 */
float player_get_half_size(const PlayerState* player, int tile_size) {
    (void)player;
    return tile_size * 1.0f;
}
