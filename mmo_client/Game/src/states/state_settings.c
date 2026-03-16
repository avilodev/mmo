#include "state_handler.h"
#include "game.h"
#include "renderer.h"
#include "ui/settings_panel.h"
#include "input/input.h"

#include <stdio.h>

// ============================================================================
// SETTINGS STATE (entered from main menu)
// ============================================================================

static void settings_enter(GameState* game) {
    (void)game;
    printf("[STATE] Entering settings\n");
}

static void settings_exit(GameState* game) {
    (void)game;
    printf("[STATE] Exiting settings\n");
}

static void settings_update(GameState* game, float delta_time) {
    (void)game; (void)delta_time;
}

static void settings_render(GameState* game) {
    int vw = game->camera.viewport_width;
    int vh = game->camera.viewport_height;

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, vw, vh, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    if (game->textures.background != 0) {
        renderer_draw_sprite(0, 0, vw, vh, game->textures.background);
    } else {
        renderer_draw_rect(0, 0, (float)vw, (float)vh, 0.08f, 0.08f, 0.14f, 1.0f);
    }
    // Slight dim over background
    renderer_draw_rect(0, 0, (float)vw, (float)vh, 0.0f, 0.0f, 0.0f, 0.45f);

    float px = ((float)vw - SP_PW) * 0.5f;
    float py = ((float)vh - SP_PH) * 0.5f;

    // Panel
    renderer_draw_rect(px, py, SP_PW, SP_PH, 0.10f, 0.10f, 0.16f, 0.97f);
    renderer_draw_rect(px,         py,          SP_PW, 2.0f,  0.35f,0.50f,0.70f,1.0f);
    renderer_draw_rect(px,         py+SP_PH-2,  SP_PW, 2.0f,  0.35f,0.50f,0.70f,1.0f);
    renderer_draw_rect(px,         py,          2.0f,  SP_PH, 0.35f,0.50f,0.70f,1.0f);
    renderer_draw_rect(px+SP_PW-2, py,          2.0f,  SP_PH, 0.35f,0.50f,0.70f,1.0f);

    sp_draw_content(px, py, &game->settings);

    // Back button
    float btn_w = 140.0f, btn_h = 36.0f;
    float btn_x = px + (SP_PW - btn_w) * 0.5f;
    float btn_y = py + SP_PH - 54.0f;
    int hov = input_mouse_in_rect(&game->input, btn_x, btn_y, btn_w, btn_h);
    float bc = hov ? 0.30f : 0.18f;
    renderer_draw_rect(btn_x, btn_y, btn_w, btn_h, bc, bc, bc+0.12f, 0.95f);
    renderer_draw_rect(btn_x,          btn_y,           btn_w, 1.5f, 0.35f,0.50f,0.70f,0.8f);
    renderer_draw_rect(btn_x,          btn_y+btn_h-1.5f, btn_w, 1.5f, 0.35f,0.50f,0.70f,0.8f);
    renderer_draw_rect(btn_x,          btn_y,           1.5f, btn_h, 0.35f,0.50f,0.70f,0.8f);
    renderer_draw_rect(btn_x+btn_w-1.5f, btn_y,         1.5f, btn_h, 0.35f,0.50f,0.70f,0.8f);
    renderer_draw_text(btn_x + btn_w*0.5f - 20.0f, btn_y+btn_h-10.0f, "Back");
}

static void settings_input(GameState* game, GLFWwindow* window, float delta_time) {
    (void)window; (void)delta_time;

    if (input_key_just_pressed(&game->input, GLFW_KEY_ESCAPE)) {
        game_change_state(game, GAME_MODE_MAIN_MENU);
        return;
    }

    float vw = (float)game->camera.viewport_width;
    float vh = (float)game->camera.viewport_height;
    float px = (vw - SP_PW) * 0.5f;
    float py = (vh - SP_PH) * 0.5f;
    float btn_y = py + SP_PH - 54.0f;

    int back = sp_handle_mouse(px, py,
                                game->input.mouse_x, game->input.mouse_y,
                                game->input.mouse_left_clicked,
                                game->input.mouse_left_down,
                                &game->settings, btn_y);
    if (back) {
        game_settings_save(&game->settings, "Game/data/settings.cfg");
        game_settings_apply(&game->settings);
        game_change_state(game, GAME_MODE_MAIN_MENU);
    }
}

const StateHandler g_state_settings = {
    .enter        = settings_enter,
    .exit         = settings_exit,
    .update       = settings_update,
    .render       = settings_render,
    .handle_input = settings_input
};
