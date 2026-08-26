/**
 * @file
 * Maintain and render the client's quest log, and decide what is tracked.
 */

#include "ui/quest_log.h"
#include "renderer.h"

#include <winsock2.h>   /* ntohl for the counters the accept packet carries */

#include <string.h>
#include <stdio.h>

#define QL_PW        480.0f   // Panel width
#define QL_PH        420.0f   // Panel height
#define QL_ROW_H     36.0f    // Height per collapsed quest row
#define QL_OBJ_H     20.0f    // Height per objective line when expanded
#define QL_BTN_H     24.0f    // Height of the abandon button on an expanded row
#define QL_BTN_W     92.0f    // Width of the same

/**
 * Initialize an empty, closed quest log.
 */
void quest_log_init(QuestLogState* ql) {
    memset(ql, 0, sizeof(*ql));
    ql->selected = -1;
}

/**
 * Copy one objective's definition, leaving its progress counter alone.
 *
 * Progress arrives in its own packet and is authoritative there, so a refresh
 * of the definition must never reset it -- that is what would make a reconnect
 * look like a rolled-back quest.
 */
static void adopt_objective(QuestObjEntry* out, const QuestObjectiveInfo* in) {
    memcpy(out->description, in->description, sizeof(out->description) - 1);
    out->description[sizeof(out->description) - 1] = '\0';

    out->required       = (int32_t)ntohl((uint32_t)in->required);
    out->objective_type = in->objective_type;
    out->has_marker     = in->has_marker;
    out->target_id      = ntohl(in->target_id);
    out->marker_x       = in->marker_x;
    out->marker_y       = in->marker_y;
}

/**
 * Add a quest or refresh the definition of an existing entry.
 */
void quest_log_add(QuestLogState* ql, uint32_t id, const char* title,
                   uint8_t obj_count, const QuestObjectiveInfo* objectives) {
    if (obj_count > MAX_QUEST_OBJ) obj_count = MAX_QUEST_OBJ;

    QuestEntry* e = NULL;
    for (int i = 0; i < ql->count; i++) {
        if (ql->entries[i].id == id) { e = &ql->entries[i]; break; }
    }

    if (!e) {
        if (ql->count >= MAX_QUESTS) return;
        e = &ql->entries[ql->count++];
        memset(e, 0, sizeof(*e));
        e->id = id;
    }

    snprintf(e->title, sizeof(e->title), "%s", title);
    e->obj_count = obj_count;
    e->active    = 1;
    e->completed = 0;

    for (int j = 0; j < obj_count; j++)
        adopt_objective(&e->objectives[j], &objectives[j]);
}

/**
 * Replace progress for one objective of an active quest.
 */
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

/**
 * Mark a quest completed and inactive.
 */
void quest_log_complete(QuestLogState* ql, uint32_t id) {
    for (int i = 0; i < ql->count; i++) {
        if (ql->entries[i].id == id) {
            ql->entries[i].completed = 1;
            ql->entries[i].active    = 0;
            return;
        }
    }
}

/**
 * Remove a quest and compact the entry array.
 */
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

/**
 * Find the entry the player is currently being pointed at.
 *
 * The expanded entry wins. With nothing expanded, the first active quest is
 * used, so a player who never opens the log is still shown a next step.
 *
 * @return The entry, or NULL when there is no active quest.
 */
static const QuestEntry* tracked_entry(const QuestLogState* ql) {
    if (ql->selected >= 0 && ql->selected < ql->count &&
        ql->entries[ql->selected].active)
        return &ql->entries[ql->selected];

    for (int i = 0; i < ql->count; i++)
        if (ql->entries[i].active) return &ql->entries[i];

    return NULL;
}

/**
 * Report the outstanding objective of the tracked quest.
 */
QuestTrackedStep quest_log_tracked_step(const QuestLogState* ql) {
    QuestTrackedStep step;
    memset(&step, 0, sizeof(step));

    const QuestEntry* e = tracked_entry(ql);
    if (!e) return step;

    for (int j = 0; j < e->obj_count; j++) {
        const QuestObjEntry* obj = &e->objectives[j];
        if (obj->current >= obj->required) continue;

        step.valid          = 1;
        step.quest_id       = e->id;
        step.quest_title    = e->title;
        step.description    = obj->description;
        step.current        = obj->current;
        step.required       = obj->required;
        step.objective_type = obj->objective_type;
        step.target_id      = obj->target_id;
        step.has_marker     = obj->has_marker;
        step.marker_x       = obj->marker_x;
        step.marker_y       = obj->marker_y;
        return step;
    }

    /* Every objective met: the step is now "go and hand it in", which has no
     * separate objective record. Point at the quest itself so the tracker can
     * still say something useful. */
    step.valid       = 1;
    step.quest_id    = e->id;
    step.quest_title = e->title;
    step.description = "Ready to turn in";
    step.current     = 1;
    step.required    = 1;
    return step;
}

/**
 * Report whether an NPC type is the target of the tracked step.
 */
int quest_log_npc_is_tracked(const QuestLogState* ql, uint8_t npc_type_id) {
    QuestTrackedStep step = quest_log_tracked_step(ql);
    if (!step.valid || step.target_id == 0) return 0;
    if (step.objective_type != QUEST_OBJECTIVE_TALK &&
        step.objective_type != QUEST_OBJECTIVE_KILL) return 0;
    return step.target_id == (uint32_t)npc_type_id;
}

static void draw_panel_border(float px, float py, float pw, float ph) {
    renderer_draw_rect(px,        py,         pw, 2.0f, 0.45f, 0.55f, 0.75f, 1.0f);
    renderer_draw_rect(px,        py+ph-2.0f, pw, 2.0f, 0.45f, 0.55f, 0.75f, 1.0f);
    renderer_draw_rect(px,        py,         2.0f, ph, 0.45f, 0.55f, 0.75f, 1.0f);
    renderer_draw_rect(px+pw-2.0f,py,         2.0f, ph, 0.45f, 0.55f, 0.75f, 1.0f);
}

// Returns the expanded height for a selected entry (objective lines, the
// abandon button, and padding)
static float expanded_height(const QuestEntry* e) {
    int obj = e->obj_count > 0 ? e->obj_count : 1;
    return 12.0f + obj * QL_OBJ_H + QL_BTN_H + 12.0f;
}

/** Place the abandon button inside an expanded row. */
static void abandon_button_rect(float px, float row_top, float exp_h,
                                float* out_x, float* out_y) {
    *out_x = px + QL_PW - QL_BTN_W - 20.0f;
    *out_y = row_top + QL_ROW_H + exp_h - QL_BTN_H - 6.0f;
}

uint32_t quest_log_take_abandon_request(QuestLogState* ql) {
    uint32_t quest_id = ql->pending_abandon;
    ql->pending_abandon = 0;
    return quest_id;
}

/**
 * Render the open quest log centered in a logical viewport.
 */
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

    /* Selecting a quest is what tracks it, so the row says so rather than
     * leaving the player to infer it from the panel in the corner. */
    const QuestEntry* tracked = tracked_entry(ql);

    float list_y = py + 56.0f;

    if (ql->count == 0) {
        renderer_draw_text(px + QL_PW*0.5f - 92.0f, list_y + 30.0f,
                           "No quests yet. Speak to the Courtyard Warden.");
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

            // Title, marked when this is the quest being tracked on screen
            renderer_draw_text(px + 28.0f, list_y + QL_ROW_H - 10.0f, e->title);
            if (e == tracked) {
                renderer_draw_text(px + QL_PW - 118.0f, list_y + QL_ROW_H - 10.0f,
                                   "tracking");
            }

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

                /* Only on the expanded row, so a quest cannot be dropped by a
                 * stray click on a collapsed one. */
                float bx, by;
                abandon_button_rect(px, list_y, this_exp_h, &bx, &by);
                renderer_draw_rect(bx, by, QL_BTN_W, QL_BTN_H,
                                   0.34f, 0.12f, 0.12f, 0.9f);
                renderer_draw_rect(bx, by, QL_BTN_W, 1.0f, 0.70f, 0.30f, 0.30f, 0.8f);
                renderer_draw_text(bx + 12.0f, by + QL_BTN_H - 7.0f, "Abandon");
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

/**
 * Toggle, close, or select entries in the quest log.
 *
 * @param clicked  Nonzero on a new pointer click.
 * @param key_j  Nonzero on a new quest-log binding press.
 * @param key_esc  Nonzero on a new escape-key press.
 */
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
                /* The button first: it sits inside the row it belongs to, so
                 * the row's own toggle must not swallow the click. */
                if (is_sel) {
                    float bx, by;
                    abandon_button_rect(px, list_y, this_exp_h, &bx, &by);
                    if (mx >= bx && mx <= bx + QL_BTN_W &&
                        my >= by && my <= by + QL_BTN_H) {
                        ql->pending_abandon = ql->entries[i].id;
                        return;
                    }
                }
                ql->selected = is_sel ? -1 : i;
                return;
            }
            list_y += row_h;
        }
    }
}
