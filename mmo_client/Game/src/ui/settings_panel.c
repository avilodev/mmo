/**
 * @file
 * Render shared settings controls and update their values from pointer input.
 */
#include "ui/settings_panel.h"
#include "renderer.h"
#include <stdio.h>

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

    float ry = py + 56.0f;
    renderer_draw_text(px + 20.0f, ry + 14.0f, "AUDIO");
    renderer_draw_rect(px + 75.0f, ry + 8.0f, pw - 95.0f, 1.0f, 0.25f, 0.35f, 0.50f, 0.6f);
    ry += SP_ROW;
    sp_draw_slider(px, ry, "Master Volume", s->master_volume, 0.35f, 0.60f, 0.85f); ry += SP_ROW;
    sp_draw_slider(px, ry, "Music Volume",  s->music_volume,  0.50f, 0.35f, 0.80f); ry += SP_ROW;
    sp_draw_slider(px, ry, "SFX Volume",    s->sfx_volume,    0.85f, 0.60f, 0.25f); ry += SP_ROW + 8.0f;

    renderer_draw_rect(px, ry, pw, 1.0f, 0.25f, 0.35f, 0.50f, 0.6f); ry += 10.0f;
    renderer_draw_text(px + 20.0f, ry + 14.0f, "DISPLAY");
    renderer_draw_rect(px + 95.0f, ry + 8.0f, pw - 115.0f, 1.0f, 0.25f, 0.35f, 0.50f, 0.6f);
    ry += SP_ROW;
    sp_draw_checkbox(px, ry, "Show FPS",   s->show_fps);    ry += SP_ROW;
    sp_draw_checkbox(px, ry, "Fullscreen", s->fullscreen);  ry += SP_ROW;
    sp_draw_slider(px, ry, "UI Scale", (s->ui_scale - 0.75f) / 0.75f,
                   0.60f, 0.75f, 0.40f);
    // Show the actual scale value next to the percentage label
    char scl[16];
    snprintf(scl, sizeof(scl), " (%.2fx)", s->ui_scale);
    renderer_draw_text(px + SP_SX + SP_SW + 46.0f, ry + SP_ROW - 10.0f, scl);
}

/**
 * Apply pointer interaction to settings controls and the close button.
 *
 * @return      Nonzero when the close button is clicked, otherwise zero.
 */
int sp_handle_mouse(float px, float py, float mx, float my,
                    int clicked, int held, GameSettings* s, float btn_y) {
    float sx = px + SP_SX;
    float audio_start = py + 56.0f + SP_ROW;
    float sry[3] = { audio_start, audio_start + SP_ROW, audio_start + SP_ROW * 2.0f };
    float* vols[3] = { &s->master_volume, &s->music_volume, &s->sfx_volume };

    if (held) {
        for (int i = 0; i < 3; i++) {
            float sy = sry[i] + (SP_ROW - SP_SH) * 0.5f;
            if (mx >= sx && mx <= sx + SP_SW && my >= sy && my <= sy + SP_SH) {
                float v = (mx - sx) / SP_SW;
                if (v < 0.0f) v = 0.0f;
                if (v > 1.0f) v = 1.0f;
                *vols[i] = v;
            }
        }
    }

    // Display section top row (Show FPS)
    float disp_row = audio_start + SP_ROW * 3.0f + 18.0f + SP_ROW;

    if (held) {
        // UI Scale slider (disp_row + SP_ROW * 2)
        float uiry = disp_row + SP_ROW * 2.0f;
        float sy = uiry + (SP_ROW - SP_SH) * 0.5f;
        if (mx >= sx && mx <= sx + SP_SW && my >= sy && my <= sy + SP_SH) {
            float v = (mx - sx) / SP_SW;
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            s->ui_scale = 0.75f + v * 0.75f;
        }
    }

    if (clicked) {
        // Show FPS checkbox
        float bx = px + SP_SX, by = disp_row + (SP_ROW - 18.0f) * 0.5f;
        if (mx >= bx && mx <= bx + 18.0f && my >= by && my <= by + 18.0f)
            s->show_fps = !s->show_fps;

        // Fullscreen checkbox (disp_row + SP_ROW)
        float fry = disp_row + SP_ROW;
        float fbx = px + SP_SX, fby = fry + (SP_ROW - 18.0f) * 0.5f;
        if (mx >= fbx && mx <= fbx + 18.0f && my >= fby && my <= fby + 18.0f)
            s->fullscreen = !s->fullscreen;

        // Close/Back button
        float bw = 140.0f, bh = 36.0f;
        float bx2 = px + (SP_PW - bw) * 0.5f;
        if (mx >= bx2 && mx <= bx2 + bw && my >= btn_y && my <= btn_y + bh)
            return 1;
    }
    return 0;
}
