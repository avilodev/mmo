/**
 * @file
 * Implement the main-menu settings state and its shared settings panel.
 */
#include "state_handler.h"
#include "game.h"
#include "renderer.h"
#include "ui/settings_panel.h"
#include "input/input.h"

#include <stdio.h>
#include "core/client_log.h"

static void settings_enter(GameState* game) {
    (void)game;
    CLOG_INFO("[STATE] Entering settings");
}

static void settings_exit(GameState* game) {
    (void)game;
    CLOG_INFO("[STATE] Exiting settings");
}

static void settings_update(GameState* game, float delta_time) {
    (void)game; (void)delta_time;
}

/** Render the scaled settings panel over the menu background. */
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

    float scale = game->settings.ui_scale;
    float pw = SP_PW * scale;
    float ph = SP_PH * scale;
    float px = ((float)vw - pw) * 0.5f;
    float py = ((float)vh - ph) * 0.5f;

    // Apply UI scale transform centered on panel origin
    glTranslatef(px, py, 0.0f);
    glScalef(scale, scale, 1.0f);

    // Panel (drawn at 0,0 in scaled space)
    renderer_draw_rect(0, 0, SP_PW, SP_PH, 0.10f, 0.10f, 0.16f, 0.97f);
    renderer_draw_rect(0,          0,          SP_PW, 2.0f,  0.35f,0.50f,0.70f,1.0f);
    renderer_draw_rect(0,          SP_PH-2,    SP_PW, 2.0f,  0.35f,0.50f,0.70f,1.0f);
    renderer_draw_rect(0,          0,          2.0f,  SP_PH, 0.35f,0.50f,0.70f,1.0f);
    renderer_draw_rect(SP_PW-2,    0,          2.0f,  SP_PH, 0.35f,0.50f,0.70f,1.0f);

    sp_draw_content(0, 0, &game->settings);

    // Back button (in scaled space)
    float btn_w = 140.0f, btn_h = 36.0f;
    float btn_x = (SP_PW - btn_w) * 0.5f;
    float btn_y = SP_PH - 54.0f;
    // Inverse-transform mouse for hover detection
    float rel_mx = (game->input.mouse_x - px) / scale;
    float rel_my = (game->input.mouse_y - py) / scale;
    int hov = (rel_mx >= btn_x && rel_mx <= btn_x + btn_w &&
               rel_my >= btn_y && rel_my <= btn_y + btn_h);
    float bc = hov ? 0.30f : 0.18f;
    renderer_draw_rect(btn_x, btn_y, btn_w, btn_h, bc, bc, bc+0.12f, 0.95f);
    renderer_draw_rect(btn_x,            btn_y,             btn_w, 1.5f, 0.35f,0.50f,0.70f,0.8f);
    renderer_draw_rect(btn_x,            btn_y+btn_h-1.5f,  btn_w, 1.5f, 0.35f,0.50f,0.70f,0.8f);
    renderer_draw_rect(btn_x,            btn_y,             1.5f, btn_h, 0.35f,0.50f,0.70f,0.8f);
    renderer_draw_rect(btn_x+btn_w-1.5f, btn_y,             1.5f, btn_h, 0.35f,0.50f,0.70f,0.8f);
    renderer_draw_text(btn_x + btn_w*0.5f - 20.0f, btn_y+btn_h-10.0f, "Back");

    glLoadIdentity(); // Reset modelview after panel drawing
}

/** Handle settings controls, persistence, and return-to-menu input. */
static void settings_input(GameState* game, GLFWwindow* window, float delta_time) {
    (void)window; (void)delta_time;

    if (input_key_just_pressed(&game->input, GLFW_KEY_ESCAPE)) {
        game_change_state(game, GAME_MODE_MAIN_MENU);
        return;
    }

    float vw = (float)game->camera.viewport_width;
    float vh = (float)game->camera.viewport_height;
    float scale = game->settings.ui_scale;
    float pw = SP_PW * scale;
    float ph = SP_PH * scale;
    float px = (vw - pw) * 0.5f;
    float py = (vh - ph) * 0.5f;
    // Inverse-transform mouse into panel's unscaled coordinate space
    float rel_mx = (game->input.mouse_x - px) / scale;
    float rel_my = (game->input.mouse_y - py) / scale;
    float btn_y = SP_PH - 54.0f;

    int back = sp_handle_mouse(0, 0,
                                rel_mx, rel_my,
                                game->input.mouse_left_clicked,
                                game->input.mouse_left_down,
                                &game->settings, btn_y);
    if (back) {
        game_settings_save(&game->settings, SETTINGS_PATH);
        game_settings_apply(&game->settings);
        game_change_state(game, GAME_MODE_MAIN_MENU);
    }
}

/** Dispatch lifecycle callbacks for the main-menu settings state. */
const StateHandler g_state_settings = {
    .enter        = settings_enter,
    .exit         = settings_exit,
    .update       = settings_update,
    .render       = settings_render,
    .handle_input = settings_input
};
