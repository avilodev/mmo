#include "ui/quest_log.h"
#include "renderer.h"
#include <string.h>
#include <stdio.h>

// ============================================================================
// LAYOUT CONSTANTS
// ============================================================================

#define QL_PW        480.0f   // Panel width
#define QL_PH        420.0f   // Panel height
#define QL_ROW_H     36.0f    // Height per collapsed quest row
#define QL_EXP_H     80.0f    // Extra height when a quest is expanded

// ============================================================================
// INIT
// ============================================================================

void quest_log_init(QuestLogState* ql) {
    memset(ql, 0, sizeof(*ql));
    ql->selected = -1;
}

// ============================================================================
// DATA MANAGEMENT
// ============================================================================

void quest_log_add(QuestLogState* ql, uint32_t id,
                   const char* title, const char* objective) {
    // Refresh existing
    for (int i = 0; i < ql->count; i++) {
        if (ql->entries[i].id == id) {
            strncpy(ql->entries[i].title,     title,     47);
            strncpy(ql->entries[i].objective, objective, 127);
            ql->entries[i].title[47]     = '\0';
            ql->entries[i].objective[127] = '\0';
            ql->entries[i].active = 1;
            return;
        }
    }
    if (ql->count >= MAX_QUESTS) return; // Full
    QuestEntry* e = &ql->entries[ql->count++];
    e->id = id;
    strncpy(e->title,     title,     47);  e->title[47]     = '\0';
    strncpy(e->objective, objective, 127); e->objective[127] = '\0';
    e->active    = 1;
    e->completed = 0;
}

void quest_log_complete(QuestLogState* ql, uint32_t id) {
    for (int i = 0; i < ql->count; i++) {
        if (ql->entries[i].id == id) {
            ql->entries[i].completed = 1;
            ql->entries[i].active    = 0;
            return;
        }
    }
}

void quest_log_remove(QuestLogState* ql, uint32_t id) {
    for (int i = 0; i < ql->count; i++) {
        if (ql->entries[i].id == id) {
            // Shift remaining entries down
            for (int j = i; j < ql->count - 1; j++)
                ql->entries[j] = ql->entries[j + 1];
            ql->count--;
            if (ql->selected >= ql->count) ql->selected = -1;
            return;
        }
    }
}

// ============================================================================
// RENDER
// ============================================================================

static void draw_panel_border(float px, float py, float pw, float ph) {
    renderer_draw_rect(px,        py,         pw, 2.0f, 0.45f, 0.55f, 0.75f, 1.0f);
    renderer_draw_rect(px,        py+ph-2.0f, pw, 2.0f, 0.45f, 0.55f, 0.75f, 1.0f);
    renderer_draw_rect(px,        py,         2.0f, ph, 0.45f, 0.55f, 0.75f, 1.0f);
    renderer_draw_rect(px+pw-2.0f,py,         2.0f, ph, 0.45f, 0.55f, 0.75f, 1.0f);
}

void quest_log_render(const QuestLogState* ql, int vw, int vh) {
    if (!ql->is_open) return;

    // Calculate total panel height (expanded entry adds extra height)
    float ph = QL_PH;
    if (ql->selected >= 0 && ql->selected < ql->count)
        ph += QL_EXP_H;

    float px = ((float)vw - QL_PW) * 0.5f;
    float py = ((float)vh - ph)    * 0.5f;

    // Panel background
    renderer_draw_rect(px, py, QL_PW, ph, 0.08f, 0.08f, 0.14f, 0.97f);
    draw_panel_border(px, py, QL_PW, ph);

    // Title bar
    renderer_draw_rect(px, py, QL_PW, 44.0f, 0.12f, 0.18f, 0.28f, 1.0f);
    renderer_draw_rect(px, py+44.0f, QL_PW, 1.5f, 0.35f,0.50f,0.70f,0.8f);
    renderer_draw_text(px + QL_PW*0.5f - 42.0f, py + 28.0f, "QUEST LOG");

    // Close hint
    renderer_draw_text(px + QL_PW - 52.0f, py + 28.0f, "[J]");

    // Counts
    int active_count = 0, done_count = 0;
    for (int i = 0; i < ql->count; i++) {
        if (ql->entries[i].active)    active_count++;
        if (ql->entries[i].completed) done_count++;
    }

    float list_y = py + 56.0f;

    if (ql->count == 0) {
        renderer_draw_text(px + QL_PW*0.5f - 70.0f, list_y + 30.0f,
                           "No quests yet. Talk to an NPC!");
        return;
    }

    // --- ACTIVE quests ---
    if (active_count > 0) {
        renderer_draw_text(px + 14.0f, list_y + 14.0f, "ACTIVE");
        renderer_draw_rect(px + 70.0f, list_y+8.0f, QL_PW-90.0f, 1.0f,
                           0.25f,0.40f,0.55f,0.6f);
        list_y += QL_ROW_H;

        for (int i = 0; i < ql->count; i++) {
            const QuestEntry* e = &ql->entries[i];
            if (!e->active) continue;

            int is_sel = (ql->selected == i);
            float row_h = is_sel ? QL_ROW_H + QL_EXP_H : QL_ROW_H;

            // Row background (slightly highlighted if selected)
            float bg = is_sel ? 0.14f : 0.0f;
            renderer_draw_rect(px, list_y, QL_PW, row_h, bg, bg, bg+0.06f, 0.5f);

            // Yellow indicator dot
            renderer_draw_rect(px+12.0f, list_y+13.0f, 8.0f, 8.0f,
                               0.90f, 0.75f, 0.10f, 1.0f);

            // Title
            renderer_draw_text(px + 28.0f, list_y + QL_ROW_H - 10.0f, e->title);

            // Expand arrow
            renderer_draw_text(px + QL_PW - 22.0f, list_y + QL_ROW_H - 10.0f,
                               is_sel ? "v" : ">");

            if (is_sel) {
                // Objective text (word-wrapped roughly by renderer)
                renderer_draw_rect(px+8.0f, list_y+QL_ROW_H, QL_PW-16.0f, QL_EXP_H,
                                   0.10f, 0.10f, 0.16f, 0.7f);
                renderer_draw_text(px+16.0f, list_y+QL_ROW_H+18.0f, "Objective:");
                renderer_draw_text(px+16.0f, list_y+QL_ROW_H+36.0f, e->objective);
            }

            // Row separator
            renderer_draw_rect(px+8.0f, list_y+row_h-1.0f, QL_PW-16.0f, 1.0f,
                               0.20f,0.25f,0.35f,0.5f);
            list_y += row_h;
        }
    }

    // --- COMPLETED quests ---
    if (done_count > 0) {
        list_y += 6.0f;
        renderer_draw_text(px + 14.0f, list_y + 14.0f, "COMPLETED");
        renderer_draw_rect(px + 100.0f, list_y+8.0f, QL_PW-120.0f, 1.0f,
                           0.20f,0.45f,0.20f,0.6f);
        list_y += QL_ROW_H;

        for (int i = 0; i < ql->count; i++) {
            const QuestEntry* e = &ql->entries[i];
            if (!e->completed) continue;

            // Row background
            renderer_draw_rect(px, list_y, QL_PW, QL_ROW_H, 0.0f, 0.0f, 0.0f, 0.2f);

            // Green checkmark dot
            renderer_draw_rect(px+12.0f, list_y+13.0f, 8.0f, 8.0f,
                               0.25f, 0.80f, 0.25f, 1.0f);

            // Title (grey-out completed)
            renderer_draw_text(px + 28.0f, list_y + QL_ROW_H - 10.0f, e->title);

            renderer_draw_rect(px+8.0f, list_y+QL_ROW_H-1.0f, QL_PW-16.0f, 1.0f,
                               0.15f,0.20f,0.25f,0.4f);
            list_y += QL_ROW_H;
        }
    }
}

// ============================================================================
// INPUT
// ============================================================================

void quest_log_handle_input_full(QuestLogState* ql,
                                  float mx, float my, int clicked,
                                  int key_j, int key_esc,
                                  int vw, int vh) {
    if (key_j) { ql->is_open = !ql->is_open; return; }
    if (!ql->is_open) return;
    if (key_esc) { ql->is_open = 0; return; }
    if (!clicked) return;

    float ph = QL_PH;
    if (ql->selected >= 0 && ql->selected < ql->count) ph += QL_EXP_H;

    float px = ((float)vw - QL_PW) * 0.5f;
    float py = ((float)vh - ph)    * 0.5f;

    // Click outside panel = close
    if (mx < px || mx > px+QL_PW || my < py || my > py+ph) {
        ql->is_open = 0;
        return;
    }

    float list_y = py + 56.0f;

    // Count active and section header
    int active_count = 0;
    for (int i = 0; i < ql->count; i++)
        if (ql->entries[i].active) active_count++;

    if (active_count > 0) {
        list_y += QL_ROW_H; // section header row
        for (int i = 0; i < ql->count; i++) {
            if (!ql->entries[i].active) continue;
            int is_sel = (ql->selected == i);
            float row_h = is_sel ? QL_ROW_H + QL_EXP_H : QL_ROW_H;
            if (my >= list_y && my < list_y + row_h) {
                ql->selected = is_sel ? -1 : i;
                return;
            }
            list_y += row_h;
        }
    }
}
