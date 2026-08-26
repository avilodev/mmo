/**
 * @file
 * Render the tracked quest step: a panel, a badge over the target, a map marker.
 */

#include "ui/quest_tracker.h"
#include "renderer.h"

#include <stdio.h>
#include <math.h>

/** Panel geometry, in logical screen pixels. */
#define QT_WIDTH        300.0f
#define QT_MARGIN        14.0f
#define QT_LINE_H        22.0f
/** Sit below everything else the top-right corner already holds.
 *
 * The minimap runs to y=220 (hud_init: y 20, size 200) and the local-coin badge
 * to roughly y=256 (currency_y = minimap bottom + 30). Starting here clears both. */
#define QT_TOP_OFFSET   300.0f

/** The one colour that means "this is your objective", used on all three surfaces. */
#define QT_R 0.95f
#define QT_G 0.80f
#define QT_B 0.15f

void quest_tracker_render_panel(const QuestLogState* ql, int vw, int vh) {
    (void)vh;

    QuestTrackedStep step = quest_log_tracked_step(ql);
    if (!step.valid) return;

    float px = (float)vw - QT_WIDTH - QT_MARGIN;
    float py = QT_TOP_OFFSET;

    int lines = renderer_measure_text_wrapped(QT_WIDTH - QT_MARGIN * 2.0f, step.description);
    if (lines < 1) lines = 1;

    float ph = QT_LINE_H * 2.0f + lines * QT_LINE_H + QT_MARGIN;

    renderer_draw_rect(px, py, QT_WIDTH, ph, 0.05f, 0.06f, 0.10f, 0.72f);
    renderer_draw_rect(px, py, 3.0f, ph, QT_R, QT_G, QT_B, 0.9f);

    renderer_draw_text(px + QT_MARGIN, py + QT_LINE_H, step.quest_title);

    float text_y = py + QT_LINE_H * 2.0f;
    renderer_draw_rect(px + QT_MARGIN - 8.0f, text_y - 8.0f, 5.0f, 5.0f,
                       QT_R, QT_G, QT_B, 1.0f);
    renderer_draw_text_wrapped(px + QT_MARGIN, text_y,
                               QT_WIDTH - QT_MARGIN * 2.0f, QT_LINE_H,
                               step.description);

    if (step.required > 1) {
        char counter[32];
        snprintf(counter, sizeof(counter), "%d / %d", step.current, step.required);
        renderer_draw_text(px + QT_WIDTH - QT_MARGIN - renderer_text_width(counter),
                           py + QT_LINE_H, counter);
    }
}

void quest_tracker_render_world_badge(float x, float top_y) {
    /* A filled marker with a dark outline, so it reads against both the grass
     * and the paving of the courtyard. */
    float w = 18.0f, h = 22.0f;
    float bx = x - w * 0.5f;
    float by = top_y - h;

    renderer_draw_rect(bx - 1.5f, by - 1.5f, w + 3.0f, h + 3.0f, 0.0f, 0.0f, 0.0f, 0.75f);
    renderer_draw_rect(bx, by, w, h, QT_R, QT_G, QT_B, 1.0f);

    /* Exclamation, drawn as blocks: the baked font is laid out for screen space
     * and would come out the wrong size in world space. */
    renderer_draw_rect(x - 2.0f, by + 4.0f,  4.0f, 9.0f, 0.12f, 0.10f, 0.02f, 1.0f);
    renderer_draw_rect(x - 2.0f, by + 15.0f, 4.0f, 4.0f, 0.12f, 0.10f, 0.02f, 1.0f);

    /* Downward point, so the badge reads as attached to the NPC below it. */
    renderer_draw_rect(x - 4.0f, by + h,        8.0f, 3.0f, QT_R, QT_G, QT_B, 1.0f);
    renderer_draw_rect(x - 2.0f, by + h + 3.0f, 4.0f, 3.0f, QT_R, QT_G, QT_B, 1.0f);
}

void quest_tracker_render_map_marker(const QuestLogState* ql,
                                     float map_x, float map_y, float map_w, float map_h,
                                     float centre_x, float centre_y,
                                     float player_wx, float player_wy, float scale) {
    QuestTrackedStep step = quest_log_tracked_step(ql);
    if (!step.valid || !step.has_marker) return;

    float sx = centre_x + (step.marker_x - player_wx) * scale;
    float sy = centre_y + (step.marker_y - player_wy) * scale;

    /* Clamped rather than culled: a target off the edge is exactly when the
     * player most needs to be told which way it lies. */
    float inset = 10.0f;
    float cx = sx, cy = sy;
    if (cx < map_x + inset)          cx = map_x + inset;
    if (cx > map_x + map_w - inset)  cx = map_x + map_w - inset;
    if (cy < map_y + inset)          cy = map_y + inset;
    if (cy > map_y + map_h - inset)  cy = map_y + map_h - inset;

    int clamped = (fabsf(cx - sx) > 0.5f || fabsf(cy - sy) > 0.5f);

    renderer_draw_rect(cx - 6.0f, cy - 6.0f, 12.0f, 12.0f, 0.0f, 0.0f, 0.0f, 0.8f);
    renderer_draw_rect(cx - 4.5f, cy - 4.5f, 9.0f, 9.0f, QT_R, QT_G, QT_B, 1.0f);

    if (clamped) {
        /* A tail pointing back along the line to the real position. */
        float dx = sx - cx, dy = sy - cy;
        float len = sqrtf(dx * dx + dy * dy);
        if (len > 0.001f) {
            dx /= len; dy /= len;
            for (int i = 1; i <= 3; i++) {
                float t = 5.0f + i * 4.0f;
                float size = 5.0f - i;
                renderer_draw_rect(cx + dx * t - size * 0.5f, cy + dy * t - size * 0.5f,
                                   size, size, QT_R, QT_G, QT_B, 1.0f - i * 0.22f);
            }
        }
    } else {
        renderer_draw_text(cx + 10.0f, cy + 5.0f, step.description);
    }
}
