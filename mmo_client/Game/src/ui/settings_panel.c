/**
 * @file
 * Render shared settings controls and update their values from pointer input.
 */
#include "ui/settings_panel.h"
#include "renderer.h"
#include <stdio.h>

const int SP_FPS_LIMITS[SP_FPS_LIMIT_CHOICES] = { 0, 30, 60, 120, 144, 240 };

/** Vertical gap between the audio block and the display block. */
#define SP_SECTION_GAP 18.0f

/** Where the first audio row starts, relative to the panel origin. */
#define SP_AUDIO_TOP   (56.0f + SP_ROW)

/**
 * Y position of one row.
 *
 * One function, used by both the draw pass and the hit test, so a control
 * cannot draw in one place and respond in another.
 */
float sp_row_y(float py, SettingsRow row) {
    float y = py + SP_AUDIO_TOP;

    /* Audio rows run consecutively from the top. */
    if (row <= SP_ROW_SFX_VOLUME) return y + SP_ROW * (float)row;

    /* Then the section divider and the DISPLAY heading. */
    y += SP_ROW * 3.0f + SP_SECTION_GAP + SP_ROW;
    return y + SP_ROW * (float)(row - SP_ROW_SHOW_FPS);
}

/** Draw a labeled option row that cycles through named choices. */
static void sp_draw_choice(float px, float py, const char* label, const char* value) {
    float bx = px + SP_SX, by = py + (SP_ROW - 18.0f) * 0.5f;
    renderer_draw_text(px + 20.0f, py + SP_ROW - 10.0f, label);
    renderer_draw_rect(bx, by, 90.0f, 18.0f, 0.12f, 0.12f, 0.18f, 1.0f);
    renderer_draw_rect(bx,       by,       90.0f, 1.0f,  0.4f, 0.4f, 0.55f, 1.0f);
    renderer_draw_rect(bx,       by+17.0f, 90.0f, 1.0f,  0.4f, 0.4f, 0.55f, 1.0f);
    renderer_draw_rect(bx,       by,       1.0f,  18.0f, 0.4f, 0.4f, 0.55f, 1.0f);
    renderer_draw_rect(bx+89.0f, by,       1.0f,  18.0f, 0.4f, 0.4f, 0.55f, 1.0f);
    renderer_draw_text(bx + 8.0f, py + SP_ROW - 10.0f, value);
}

/** Draw a labeled normalized-value slider row. */
void sp_draw_slider(float px, float py, const char* label, float val,
                    float sr, float sg, float sb) {
    float sy = py + (SP_ROW - SP_SH) * 0.5f;
    float sx = px + SP_SX;
    renderer_draw_text(px + 20.0f, py + SP_ROW - 10.0f, label);
    renderer_draw_rect(sx, sy, SP_SW, SP_SH, 0.12f, 0.12f, 0.18f, 1.0f);
    if (val > 0.0f)
        renderer_draw_rect(sx, sy, SP_SW * val, SP_SH, sr, sg, sb, 0.85f);
    renderer_draw_rect(sx,            sy,            SP_SW, 1.0f, 0.3f, 0.3f, 0.45f, 1.0f);
    renderer_draw_rect(sx,            sy+SP_SH-1.0f, SP_SW, 1.0f, 0.3f, 0.3f, 0.45f, 1.0f);
    renderer_draw_rect(sx,            sy,            1.0f,  SP_SH, 0.3f, 0.3f, 0.45f, 1.0f);
    renderer_draw_rect(sx+SP_SW-1.0f, sy,            1.0f,  SP_SH, 0.3f, 0.3f, 0.45f, 1.0f);
    char pct[8];
    snprintf(pct, sizeof(pct), "%d%%", (int)(val * 100.0f + 0.5f));
    renderer_draw_text(sx + SP_SW + 6.0f, py + SP_ROW - 10.0f, pct);
}

/** Draw a labeled Boolean checkbox row. */
void sp_draw_checkbox(float px, float py, const char* label, int on) {
    float bx = px + SP_SX, by = py + (SP_ROW - 18.0f) * 0.5f;
    renderer_draw_text(px + 20.0f, py + SP_ROW - 10.0f, label);
    renderer_draw_rect(bx, by, 18.0f, 18.0f, 0.12f, 0.12f, 0.18f, 1.0f);
    if (on) renderer_draw_rect(bx + 3.0f, by + 3.0f, 12.0f, 12.0f, 0.35f, 0.75f, 0.35f, 1.0f);
    renderer_draw_rect(bx,       by,       18.0f, 1.0f,  0.4f, 0.4f, 0.55f, 1.0f);
    renderer_draw_rect(bx,       by+17.0f, 18.0f, 1.0f,  0.4f, 0.4f, 0.55f, 1.0f);
    renderer_draw_rect(bx,       by,       1.0f,  18.0f, 0.4f, 0.4f, 0.55f, 1.0f);
    renderer_draw_rect(bx+17.0f, by,       1.0f,  18.0f, 0.4f, 0.4f, 0.55f, 1.0f);
    renderer_draw_text(bx + 26.0f, py + SP_ROW - 10.0f, on ? "On" : "Off");
}

/** Render the audio and display settings panel content. */
void sp_draw_content(float px, float py, const GameSettings* s) {
    float pw = SP_PW;
    renderer_draw_rect(px, py, pw, 46.0f, 0.14f, 0.20f, 0.30f, 1.0f);
    renderer_draw_text(px + pw * 0.5f - 36.0f, py + 30.0f, "SETTINGS");
    renderer_draw_rect(px, py + 44.0f, pw, 1.5f, 0.35f, 0.50f, 0.70f, 0.8f);

    renderer_draw_text(px + 20.0f, py + 56.0f + 14.0f, "AUDIO");
    renderer_draw_rect(px + 75.0f, py + 56.0f + 8.0f, pw - 95.0f, 1.0f,
                       0.25f, 0.35f, 0.50f, 0.6f);

    sp_draw_slider(px, sp_row_y(py, SP_ROW_MASTER_VOLUME), "Master Volume",
                   s->master_volume, 0.35f, 0.60f, 0.85f);
    sp_draw_slider(px, sp_row_y(py, SP_ROW_MUSIC_VOLUME), "Music Volume",
                   s->music_volume, 0.50f, 0.35f, 0.80f);
    sp_draw_slider(px, sp_row_y(py, SP_ROW_SFX_VOLUME), "SFX Volume",
                   s->sfx_volume, 0.85f, 0.60f, 0.25f);

    /* The DISPLAY heading sits in the gap sp_row_y() accounts for. */
    float heading_y = sp_row_y(py, SP_ROW_SHOW_FPS) - SP_ROW;
    renderer_draw_rect(px, heading_y - 10.0f, pw, 1.0f, 0.25f, 0.35f, 0.50f, 0.6f);
    renderer_draw_text(px + 20.0f, heading_y + 14.0f, "DISPLAY");
    renderer_draw_rect(px + 95.0f, heading_y + 8.0f, pw - 115.0f, 1.0f,
                       0.25f, 0.35f, 0.50f, 0.6f);

    sp_draw_checkbox(px, sp_row_y(py, SP_ROW_SHOW_FPS),   "Show FPS",   s->show_fps);
    sp_draw_checkbox(px, sp_row_y(py, SP_ROW_FULLSCREEN), "Fullscreen", s->fullscreen);

    /* VSync and the frame cap. Before these existed the client disabled VSync
     * unconditionally and limited nothing, so it redrew as fast as the GPU
     * would allow -- on a menu screen, hundreds of identical frames a second. */
    sp_draw_checkbox(px, sp_row_y(py, SP_ROW_VSYNC), "VSync", s->vsync);

    char cap[16];
    if (s->vsync)              snprintf(cap, sizeof(cap), "Display");
    else if (s->fps_limit > 0) snprintf(cap, sizeof(cap), "%d FPS", s->fps_limit);
    else                       snprintf(cap, sizeof(cap), "Unlimited");
    sp_draw_choice(px, sp_row_y(py, SP_ROW_FPS_LIMIT), "Frame Cap", cap);

    float ui_y = sp_row_y(py, SP_ROW_UI_SCALE);
    sp_draw_slider(px, ui_y, "UI Scale", (s->ui_scale - 0.75f) / 0.75f,
                   0.60f, 0.75f, 0.40f);
    // Show the actual scale value next to the percentage label
    char scl[16];
    snprintf(scl, sizeof(scl), " (%.2fx)", s->ui_scale);
    renderer_draw_text(px + SP_SX + SP_SW + 46.0f, ui_y + SP_ROW - 10.0f, scl);
}

/**
 * Apply pointer interaction to settings controls and the close button.
 *
 * @return      Nonzero when the close button is clicked, otherwise zero.
 */
int sp_handle_mouse(float px, float py, float mx, float my,
                    int clicked, int held, GameSettings* s, float btn_y) {
    float sx = px + SP_SX;

    /* Every hit test below indexes sp_row_y() with the same enum value the
     * draw pass used, so a row cannot move for one and not the other. */
    if (held) {
        const SettingsRow slider_rows[3] = {
            SP_ROW_MASTER_VOLUME, SP_ROW_MUSIC_VOLUME, SP_ROW_SFX_VOLUME
        };
        float* vols[3] = { &s->master_volume, &s->music_volume, &s->sfx_volume };

        for (int i = 0; i < 3; i++) {
            float sy = sp_row_y(py, slider_rows[i]) + (SP_ROW - SP_SH) * 0.5f;
            if (mx >= sx && mx <= sx + SP_SW && my >= sy && my <= sy + SP_SH) {
                float v = (mx - sx) / SP_SW;
                if (v < 0.0f) v = 0.0f;
                if (v > 1.0f) v = 1.0f;
                *vols[i] = v;
            }
        }

        float sy = sp_row_y(py, SP_ROW_UI_SCALE) + (SP_ROW - SP_SH) * 0.5f;
        if (mx >= sx && mx <= sx + SP_SW && my >= sy && my <= sy + SP_SH) {
            float v = (mx - sx) / SP_SW;
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            s->ui_scale = 0.75f + v * 0.75f;
        }
    }

    if (clicked) {
        float bx = px + SP_SX;

        float fps_y = sp_row_y(py, SP_ROW_SHOW_FPS) + (SP_ROW - 18.0f) * 0.5f;
        if (mx >= bx && mx <= bx + 18.0f && my >= fps_y && my <= fps_y + 18.0f)
            s->show_fps = !s->show_fps;

        float full_y = sp_row_y(py, SP_ROW_FULLSCREEN) + (SP_ROW - 18.0f) * 0.5f;
        if (mx >= bx && mx <= bx + 18.0f && my >= full_y && my <= full_y + 18.0f)
            s->fullscreen = !s->fullscreen;

        float vs_y = sp_row_y(py, SP_ROW_VSYNC) + (SP_ROW - 18.0f) * 0.5f;
        if (mx >= bx && mx <= bx + 18.0f && my >= vs_y && my <= vs_y + 18.0f)
            s->vsync = !s->vsync;

        /* The frame cap cycles rather than sliding: the useful values are a
         * short list of display refresh rates, and a slider would let a player
         * land on 37. Clicking it while VSync is on still changes the stored
         * value, which is what takes effect the moment VSync is turned off. */
        float cap_y = sp_row_y(py, SP_ROW_FPS_LIMIT) + (SP_ROW - 18.0f) * 0.5f;
        if (mx >= bx && mx <= bx + 90.0f && my >= cap_y && my <= cap_y + 18.0f) {
            int index = 0;
            for (int i = 0; i < SP_FPS_LIMIT_CHOICES; i++) {
                if (SP_FPS_LIMITS[i] == s->fps_limit) { index = i; break; }
            }
            s->fps_limit = SP_FPS_LIMITS[(index + 1) % SP_FPS_LIMIT_CHOICES];
        }

        // Close/Back button
        float bw = 140.0f, bh = 36.0f;
        float bx2 = px + (SP_PW - bw) * 0.5f;
        if (mx >= bx2 && mx <= bx2 + bw && my >= btn_y && my <= btn_y + bh)
            return 1;
    }
    return 0;
}
