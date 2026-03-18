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
#define QL_OBJ_H     20.0f    // Height per objective line when expanded

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
                   const char* title,
                   uint8_t obj_count,
                   const char descriptions[][64],
                   const int32_t required[]) {
    if (obj_count > MAX_QUEST_OBJ) obj_count = MAX_QUEST_OBJ;

    // Refresh existing entry
    for (int i = 0; i < ql->count; i++) {
        if (ql->entries[i].id == id) {
            strncpy(ql->entries[i].title, title, 47);
            ql->entries[i].title[47] = '\0';
            ql->entries[i].obj_count = obj_count;
            for (int j = 0; j < obj_count; j++) {
                strncpy(ql->entries[i].objectives[j].description, descriptions[j], 63);
                ql->entries[i].objectives[j].description[63] = '\0';
                ql->entries[i].objectives[j].required = required[j];
                // Preserve current progress — don't reset it
            }
            ql->entries[i].active = 1;
            return;
        }
    }

    if (ql->count >= MAX_QUESTS) return;
    QuestEntry* e = &ql->entries[ql->count++];
    e->id = id;
    strncpy(e->title, title, 47); e->title[47] = '\0';
    e->obj_count  = obj_count;
    e->active     = 1;
    e->completed  = 0;
    for (int j = 0; j < obj_count; j++) {
        strncpy(e->objectives[j].description, descriptions[j], 63);
        e->objectives[j].description[63] = '\0';
        e->objectives[j].current  = 0;
        e->objectives[j].required = required[j];
    }
}

void quest_log_update_progress(QuestLogState* ql, uint32_t quest_id,
                               uint8_t obj_index, int32_t current, int32_t required) {
    for (int i = 0; i < ql->count; i++) {
        if (ql->entries[i].id == quest_id && ql->entries[i].active) {
            if (obj_index < MAX_QUEST_OBJ) {
                ql->entries[i].objectives[obj_index].current  = current;
                ql->entries[i].objectives[obj_index].required = required;
                if (obj_index >= ql->entries[i].obj_count)
                    ql->entries[i].obj_count = obj_index + 1;
            }
            return;
        }
    }
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

// Returns the expanded height for a selected entry (objective lines + padding)
static float expanded_height(const QuestEntry* e) {
    int obj = e->obj_count > 0 ? e->obj_count : 1;
    return 12.0f + obj * QL_OBJ_H + 8.0f;
}

void quest_log_render(const QuestLogState* ql, int vw, int vh) {
    if (!ql->is_open) return;

    // Calculate total panel height
    float exp_h = 0.0f;
    if (ql->selected >= 0 && ql->selected < ql->count && ql->entries[ql->selected].active)
        exp_h = expanded_height(&ql->entries[ql->selected]);

    float ph = QL_PH + exp_h;
    float px = ((float)vw - QL_PW) * 0.5f;
    float py = ((float)vh - ph)    * 0.5f;

    // Panel background
    renderer_draw_rect(px, py, QL_PW, ph, 0.08f, 0.08f, 0.14f, 0.97f);
    draw_panel_border(px, py, QL_PW, ph);

    // Title bar
    renderer_draw_rect(px, py, QL_PW, 44.0f, 0.12f, 0.18f, 0.28f, 1.0f);
    renderer_draw_rect(px, py+44.0f, QL_PW, 1.5f, 0.35f,0.50f,0.70f,0.8f);
    renderer_draw_text(px + QL_PW*0.5f - 42.0f, py + 28.0f, "QUEST LOG");
    renderer_draw_text(px + QL_PW - 52.0f, py + 28.0f, "[J]");

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
            float this_exp_h = is_sel ? expanded_height(e) : 0.0f;
            float row_h = QL_ROW_H + this_exp_h;

            // Row background
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
                float oy = list_y + QL_ROW_H + 8.0f;
                renderer_draw_rect(px+8.0f, list_y+QL_ROW_H, QL_PW-16.0f, this_exp_h,
                                   0.10f, 0.10f, 0.16f, 0.7f);

                if (e->obj_count == 0) {
                    renderer_draw_text(px+16.0f, oy, "No objectives.");
                } else {
                    for (int j = 0; j < e->obj_count; j++) {
                        const QuestObjEntry* obj = &e->objectives[j];
                        char buf[128];
                        if (obj->required > 1) {
                            snprintf(buf, sizeof(buf), "%s: %d/%d",
                                     obj->description, obj->current, obj->required);
                        } else {
                            snprintf(buf, sizeof(buf), "%s: %s",
                                     obj->description,
                                     obj->current >= obj->required ? "Done" : "Pending");
                        }
                        // Colour: green if done, white if not
                        // (renderer_draw_text doesn't support per-call colour here
                        //  so we rely on the default white text for now)
                        renderer_draw_text(px+16.0f, oy + j * QL_OBJ_H, buf);
                    }
                }
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

            renderer_draw_rect(px, list_y, QL_PW, QL_ROW_H, 0.0f, 0.0f, 0.0f, 0.2f);

            // Green dot
            renderer_draw_rect(px+12.0f, list_y+13.0f, 8.0f, 8.0f,
                               0.25f, 0.80f, 0.25f, 1.0f);

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

    float exp_h = 0.0f;
    if (ql->selected >= 0 && ql->selected < ql->count && ql->entries[ql->selected].active)
        exp_h = expanded_height(&ql->entries[ql->selected]);

    float ph = QL_PH + exp_h;
    float px = ((float)vw - QL_PW) * 0.5f;
    float py = ((float)vh - ph)    * 0.5f;

    if (mx < px || mx > px+QL_PW || my < py || my > py+ph) {
        ql->is_open = 0;
        return;
    }

    float list_y = py + 56.0f;

    int active_count = 0;
    for (int i = 0; i < ql->count; i++)
        if (ql->entries[i].active) active_count++;

    if (active_count > 0) {
        list_y += QL_ROW_H; // section header
        for (int i = 0; i < ql->count; i++) {
            if (!ql->entries[i].active) continue;
            int is_sel = (ql->selected == i);
            float this_exp_h = is_sel ? expanded_height(&ql->entries[i]) : 0.0f;
            float row_h = QL_ROW_H + this_exp_h;
            if (my >= list_y && my < list_y + row_h) {
                ql->selected = is_sel ? -1 : i;
                return;
            }
            list_y += row_h;
        }
    }
}
