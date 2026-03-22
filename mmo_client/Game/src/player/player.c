#include "player.h"
#include "world/world.h"
#include "input.h"
#include "renderer.h"
#include "core/keybinds.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

void player_init(PlayerState* player) {
    memset(player, 0, sizeof(PlayerState));
    player->speed = 200.0f;  // Default; overridden by server PACKET_PLAYER_STATS
}

void player_reset_position(PlayerState* player, float x, float y) {
    player->x = x;
    player->y = y;
    player->vel_x = 0.0f;
    player->vel_y = 0.0f;
    player->needs_position_reset = 0;
    printf("[PLAYER] Position reset to (%.1f, %.1f)\n", x, y);
}

int player_update_movement(PlayerState* player, const InputState* input,
                           const WorldState* world, float delta_time) {
    float move_x = 0.0f;
    float move_y = 0.0f;
    float speed = player->speed * delta_time;
    
    // Read movement input
    if (input_key_pressed(input, g_keybinds.move_up))    move_y -= speed;
    if (input_key_pressed(input, g_keybinds.move_down))  move_y += speed;
    if (input_key_pressed(input, g_keybinds.move_left))  move_x -= speed;
    if (input_key_pressed(input, g_keybinds.move_right)) move_x += speed;
    
    // No movement
    if (move_x == 0.0f && move_y == 0.0f) {
        player->vel_x = 0.0f;
        player->vel_y = 0.0f;
        return 0;
    }

    // Normalize diagonal movement so it's not faster than cardinal
    if (move_x != 0.0f && move_y != 0.0f) {
        float inv_sqrt2 = 0.70710678f; // 1/sqrt(2)
        move_x *= inv_sqrt2;
        move_y *= inv_sqrt2;
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
    
    // Calculate velocity for network sync
    if (delta_time > 0.0f) {
        player->vel_x = move_x / delta_time;
        player->vel_y = move_y / delta_time;
    }
    
    return (move_x != 0.0f || move_y != 0.0f);
}

void player_apply_correction(PlayerState* player, float x, float y) {
    player->x = x;
    player->y = y;
    printf("[PLAYER] Server correction applied: (%.1f, %.1f)\n", x, y);
}

void player_load_info(PlayerState* player, const CharacterInfo* info) {
    memcpy(&player->info, info, sizeof(CharacterInfo));
    player->info_loaded = 1;
    
    // Set position from server data
    player->x = info->pos_x;
    player->y = info->pos_y;
    player->needs_position_reset = 1;
    
    printf("[PLAYER] Info loaded: %s Lv.%u at (%.1f, %.1f)\n",
           info->name, info->level, info->pos_x, info->pos_y);
}

void player_render(const PlayerState* player, unsigned int texture, int tile_size) {
    int size = tile_size * 2;

    if (texture != 0) {
        renderer_draw_sprite(
            player->x - size / 2,
            player->y - size / 2,
            size, size,
            texture
        );
    } else {
        // Fallback colored rectangle
        renderer_draw_rect(
            player->x - size / 2,
            player->y - size / 2,
            size, size,
            0.2f, 0.6f, 1.0f, 1.0f
        );
    }

    // Name + level label above the sprite
    if (player->info_loaded && player->info.name[0] != '\0') {
        char label[48];
        snprintf(label, sizeof(label), "%u - %s",
                 (unsigned)player->info.level, player->info.name);
        float label_w = 120.0f;
        float label_x = player->x - label_w / 2.0f;
        float label_y = player->y - size / 2.0f - 20.0f;
        renderer_draw_text_centered(label_x, label_y, label_w, 0.0f, label);
    }
}

float player_get_half_size(const PlayerState* player, int tile_size) {
    (void)player;
    return tile_size * 1.0f;
}