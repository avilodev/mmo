/**
 * @file
 * Maintain and render the client's quest log, and decide what is tracked.
 */

#include "ui/quest_log.h"
#include "renderer.h"
#include "ui/quest_distance.h"
#include "ui/ui_kit.h"
#include "ui/window_pos.h"
#include "player/item_db.h"
#include "world/npc_types.h"
#include "world_regions.h"

#include <winsock2.h>   /* ntohl for the counters the accept packet carries */

#include <string.h>
#include <stdio.h>

#define QL_LIST_W      300.0f   // Width of the quest list
#define QL_DETAIL_W    380.0f   // Width of the details window
#define QL_TITLE_H      40.0f   // Height of a window's title strip
#define QL_ROW_H        26.0f   // Height of a quest row
#define QL_HEAD_H       22.0f   // Height of a section header
#define QL_PAD          14.0f   // Inner padding
#define QL_CLOSE        22.0f   // Side of the close button
#define QL_BTN_W        92.0f   // Width of the abandon button
#define QL_BTN_H        26.0f   // Height of the abandon button
#define QL_LINE_H       19.0f   // Height of a line of details
#define QL_TEXT         14.0f   // Size of the text
#define QL_SMALL        13.0f   // Size of the dim text
#define QL_TITLE        19.0f   // Size of a window's title
#define QL_MAX_ROWS     18      // Rows the list shows before it says "more"
#define QL_GAP          12.0f   // Space between the list and its details

/**
 * Initialize an empty, closed quest log.
 */
void quest_log_init(QuestLogState* ql) {
    memset(ql, 0, sizeof(*ql));
    ql->selected = -1;
	ql->available_selected = -1;
	window_pos_reset(&ql->list_win);
	window_pos_reset(&ql->detail_win);
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
        if (!quest_log_make_room(ql)) return;
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
		step.step           = e->step;
		step.step_count     = e->step_count;
		step.step_title     = e->step_title;
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
	step.step        = e->step;
	step.step_count  = e->step_count;
	step.step_title  = e->step_title;
    step.description = "Ready to turn in";
    step.current     = 1;
    step.required    = 1;
    return step;
}

// Name the NPC type that gives a quest, for the row and the hint.
static const char* giver_name(uint8_t npc_type) {
	const char* name = npc_type_get_name(npc_type);

	return name && name[0] ? name : "someone";
}

// Write "Reward: 40 XP, 25 Ennara coin, Item x1" for an available quest
void quest_log_available_reward_text(const available_quest_entry* e, char* out, size_t size) {
	size_t used = (size_t)snprintf(out, size, "Reward:");
	int any = 0;

	if(e->xp_reward > 0 && used < size) {
		used += (size_t)snprintf(out + used, size - used, "%s %u XP", any ? "," : "", e->xp_reward);
		any = 1;
	}
	if(e->currency_reward > 0 && used < size) {
		used += (size_t)snprintf(out + used, size - used, "%s %u %s", any ? "," : "",
		                         e->currency_reward, world_currency_name(e->currency_id));
		any = 1;
	}
	for(int i = 0; i < e->item_count && used < size; i++) {
		const char* name = item_db_get_name(e->items[i].item_id);

		used += (size_t)snprintf(out + used, size - used, "%s %s x%u", any ? "," : "",
		                         name ? name : "item", e->items[i].quantity);
		any = 1;
	}
	if(!any && used < size)
		snprintf(out + used, size - used, " none");
}

uint32_t quest_log_take_abandon_request(QuestLogState* ql) {
    uint32_t quest_id = ql->pending_abandon;
    ql->pending_abandon = 0;
    return quest_id;
}

// --- The list ---------------------------------------------------------------

typedef enum {
	ROW_HEAD,
	ROW_AVAILABLE,
	ROW_ACTIVE,
	ROW_DONE,
	ROW_MORE
} row_kind;

// One line of the list: what it is, which quest, and a label for headers
typedef struct {
	row_kind kind;
	int index;
	const char* label;
} list_row;

static float row_height(row_kind kind) {
	return kind == ROW_HEAD || kind == ROW_MORE ? QL_HEAD_H : QL_ROW_H;
}

// Lay out the list top to bottom; returns how many rows there are
static int build_rows(const QuestLogState* ql, list_row* rows, int max) {
	int count = 0;
	int active = 0;
	int done = 0;
	int wanted = 0;

	for(int i = 0; i < ql->count; i++) {
		active += ql->entries[i].active ? 1 : 0;
		done += ql->entries[i].completed ? 1 : 0;
	}
	wanted = (ql->available_count ? ql->available_count + 1 : 0) + (active ? active + 1 : 0) +
	         (done ? done + 1 : 0);

	if(ql->available_count > 0 && count < max) {
		rows[count++] = (list_row){ ROW_HEAD, 0, "Available" };
		for(int i = 0; i < ql->available_count && count < max - 1; i++)
			rows[count++] = (list_row){ ROW_AVAILABLE, i, NULL };
	}
	if(active > 0 && count < max - 1) {
		rows[count++] = (list_row){ ROW_HEAD, 0, "Active" };
		for(int i = 0; i < ql->count && count < max - 1; i++)
			if(ql->entries[i].active)
				rows[count++] = (list_row){ ROW_ACTIVE, i, NULL };
	}
	if(done > 0 && count < max - 1) {
		rows[count++] = (list_row){ ROW_HEAD, 0, "Completed" };
		for(int i = 0; i < ql->count && count < max - 1; i++)
			if(ql->entries[i].completed)
				rows[count++] = (list_row){ ROW_DONE, i, NULL };
	}
	if(wanted > count)
		rows[count++] = (list_row){ ROW_MORE, wanted - count, NULL };

	return count;
}

static float list_height(const list_row* rows, int count) {
	float h = QL_TITLE_H + QL_PAD;

	for(int i = 0; i < count; i++)
		h += row_height(rows[i].kind);
	if(count == 0)
		h += QL_ROW_H;

	return h;
}

// The text cut short with dots until it fits a width
static void fit_text(const char* text, float width, char* out, size_t out_size) {
	size_t n;

	snprintf(out, out_size, "%s", text);
	n = strlen(out);
	while(n > 3 && ui_kit_px_text_width(out, QL_TEXT) > width) {
		out[n - 1] = '\0';
		n--;
		if(n > 3)
			memcpy(out + n - 3, "...", 3);
	}
}

// Whether each kind of selection points at a real quest
static int active_selection_valid(const QuestLogState* ql) {
	return ql->selected >= 0 && ql->selected < ql->count && ql->entries[ql->selected].active;
}

static int available_selection_valid(const QuestLogState* ql) {
	return ql->available_selected >= 0 && ql->available_selected < ql->available_count;
}

static int detail_open(const QuestLogState* ql) {
	return active_selection_valid(ql) || available_selection_valid(ql);
}

// Pick a row's quest, or put it down if it was already picked
static void toggle_row(QuestLogState* ql, const list_row* row) {
	if(row->kind == ROW_ACTIVE) {
		ql->selected = ql->selected == row->index ? -1 : row->index;
		ql->available_selected = -1;
	} else if(row->kind == ROW_AVAILABLE) {
		ql->available_selected = ql->available_selected == row->index ? -1 : row->index;
		ql->selected = -1;
	}
}

// --- The details ------------------------------------------------------------

#define WRAP_LINES 6

// A line of wrapped text: where it starts in the source and how long it is
typedef struct {
	int start;
	int len;
} wrap_line;

// Break text into lines no wider than width, at spaces; returns how many
static int wrap_text(const char* text, float width, wrap_line* lines, int max) {
	int n = (int)strlen(text);
	int start = 0;
	int count = 0;

	while(start < n && count < max) {
		int best = -1;

		// The longest run of whole words that fits, and always at least one word
		for(int i = start; i <= n; i++) {
			char buf[128];
			int len = i - start;

			if(i < n && text[i] != ' ')
				continue;
			if(len >= (int)sizeof(buf))
				len = (int)sizeof(buf) - 1;
			memcpy(buf, text + start, (size_t)len);
			buf[len] = '\0';
			if(best >= 0 && ui_kit_px_text_width(buf, QL_TEXT) > width)
				break;
			best = i;
		}

		lines[count].start = start;
		lines[count].len = best - start;
		count++;
		start = best + 1;
	}

	return count;
}

// Draw wrapped text from x, y; returns the height it took
static float draw_wrapped(const char* text, float x, float y, float width, kit_color color,
                          int draw) {
	wrap_line lines[WRAP_LINES];
	int count = wrap_text(text, width, lines, WRAP_LINES);
	char buf[128];

	if(count == 0)
		count = 1;
	for(int i = 0; i < count && draw; i++) {
		int len = lines[i].len < (int)sizeof(buf) - 1 ? lines[i].len : (int)sizeof(buf) - 1;

		memcpy(buf, text + lines[i].start, (size_t)len);
		buf[len] = '\0';
		ui_kit_px_text(x, y + (float)i * QL_LINE_H, buf, QL_TEXT, color, KIT_LEFT);
	}

	return (float)count * QL_LINE_H;
}

typedef struct {
	float height;
	float btn_x;
	float btn_y;
	int has_btn;
} detail_layout;

static const kit_color DONE_GREEN = { 0.50f, 0.88f, 0.55f };

// Lay out, and when asked draw, what the details window says
static detail_layout detail_pass(const QuestLogState* ql, float x, float y, int draw) {
	detail_layout out = { 0.0f, 0.0f, 0.0f, 0, };
	float cy = y + QL_TITLE_H + QL_PAD + QL_LINE_H * 0.5f;
	float text_w = QL_DETAIL_W - QL_PAD * 2.0f - 52.0f;
	char text[160];

	if(active_selection_valid(ql)) {
		const QuestEntry* e = &ql->entries[ql->selected];

		if(quest_step_label(e, text, sizeof(text))) {
			if(draw)
				ui_kit_px_text(x + QL_PAD, cy, text, QL_SMALL, KIT_DIM, KIT_LEFT);
			cy += QL_LINE_H + 4.0f;
		}
		for(int j = 0; j < e->obj_count; j++) {
			const QuestObjEntry* obj = &e->objectives[j];
			int finished = obj->current >= obj->required;

			if(obj->required > 1)
				snprintf(text, sizeof(text), "%d/%d", obj->current, obj->required);
			else
				snprintf(text, sizeof(text), "%s", finished ? "Done" : "");
			if(draw)
				ui_kit_px_text(x + QL_DETAIL_W - QL_PAD, cy, text, QL_TEXT,
				               finished ? DONE_GREEN : KIT_DIM, KIT_RIGHT);
			cy += draw_wrapped(obj->description, x + QL_PAD, cy, text_w,
			                   finished ? KIT_DIM : KIT_WHITE, draw) + 6.0f;
		}
		if(e->obj_count == 0) {
			if(draw)
				ui_kit_px_text(x + QL_PAD, cy, "No objectives.", QL_TEXT, KIT_DIM, KIT_LEFT);
			cy += QL_LINE_H;
		}

		// Only here, so a quest cannot be dropped by a stray click on the list
		out.has_btn = 1;
		out.btn_x = x + QL_DETAIL_W - QL_PAD - QL_BTN_W;
		out.btn_y = cy - QL_LINE_H * 0.5f + 4.0f;
		cy = out.btn_y + QL_BTN_H + QL_LINE_H * 0.5f;
	} else if(available_selection_valid(ql)) {
		const available_quest_entry* e = &ql->available[ql->available_selected];

		for(int j = 0; j < e->obj_count; j++)
			cy += draw_wrapped(e->objectives[j].description, x + QL_PAD, cy,
			                   QL_DETAIL_W - QL_PAD * 2.0f, KIT_WHITE, draw) + 6.0f;
		if(e->obj_count == 0) {
			if(draw)
				ui_kit_px_text(x + QL_PAD, cy, "No objectives.", QL_TEXT, KIT_DIM, KIT_LEFT);
			cy += QL_LINE_H;
		}
		quest_log_available_reward_text(e, text, sizeof(text));
		cy += draw_wrapped(text, x + QL_PAD, cy, QL_DETAIL_W - QL_PAD * 2.0f, KIT_GOLD, draw) + 6.0f;
		snprintf(text, sizeof(text), "Speak to %s to accept.", giver_name(e->giver_npc_type));
		cy += draw_wrapped(text, x + QL_PAD, cy, QL_DETAIL_W - QL_PAD * 2.0f, KIT_DIM, draw);
	}

	out.height = cy - y + QL_PAD - QL_LINE_H * 0.5f;
	return out;
}

// The title of the picked quest
static const char* detail_title(const QuestLogState* ql) {
	if(active_selection_valid(ql))
		return ql->entries[ql->selected].title;

	return ql->available[ql->available_selected].title;
}

// --- Where the windows are --------------------------------------------------

typedef struct {
	float x;
	float y;
	float w;
	float h;
} rect;

static int rect_has(const rect* r, float mx, float my) {
	return mx >= r->x && mx < r->x + r->w && my >= r->y && my < r->y + r->h;
}

static rect list_rect(const QuestLogState* ql, int vw, int vh, const list_row* rows, int count) {
	rect r = { 0.0f, 0.0f, QL_LIST_W, list_height(rows, count) };

	window_pos_origin(&ql->list_win, (float)vw, (float)vh, r.w, r.h, &r.x, &r.y);

	return r;
}

// The details window: where it was moved to, else beside the list
static rect detail_rect(const QuestLogState* ql, int vw, int vh, const rect* list) {
	rect r = { 0.0f, 0.0f, QL_DETAIL_W, detail_pass(ql, 0.0f, 0.0f, 0).height };

	if(ql->detail_win.placed) {
		window_pos_origin(&ql->detail_win, (float)vw, (float)vh, r.w, r.h, &r.x, &r.y);
		return r;
	}

	r.x = list->x + list->w + QL_GAP;
	r.y = list->y;
	if(r.x + r.w > (float)vw)
		r.x = list->x - QL_GAP - r.w;
	if(r.x < 0.0f)
		r.x = 0.0f;
	if(r.y + r.h > (float)vh)
		r.y = (float)vh - r.h;
	if(r.y < 0.0f)
		r.y = 0.0f;

	return r;
}

static float close_x(const rect* r) {
	return r->x + r->w - QL_CLOSE - 10.0f;
}

static float close_y(const rect* r) {
	return r->y + (QL_TITLE_H - QL_CLOSE) * 0.5f;
}

static int close_hit(const rect* r, float mx, float my) {
	return mx >= close_x(r) && mx < close_x(r) + QL_CLOSE && my >= close_y(r) &&
	       my < close_y(r) + QL_CLOSE;
}

// --- Drawing ----------------------------------------------------------------

// The frame every window here has: panel, title and close button
static void draw_frame(const rect* r, const char* title) {
	char fitted[64];

	ui_kit_px_rect(r->x, r->y, r->w, r->h, 0.05f, 0.05f, 0.09f, 0.97f);
	ui_kit_px_panel(r->x, r->y, r->w, r->h);
	fit_text(title, r->w - 80.0f, fitted, sizeof(fitted));
	ui_kit_px_text(r->x + QL_PAD, r->y + QL_TITLE_H * 0.5f, fitted, QL_TITLE, KIT_GOLD, KIT_LEFT);
	ui_kit_px_close_button(close_x(r), close_y(r), QL_CLOSE, 0);
}

static const kit_color DOT_COLORS[ROW_MORE + 1] = {
	{ 0.0f, 0.0f, 0.0f }, { 0.40f, 0.65f, 0.95f }, { 0.90f, 0.75f, 0.10f },
	{ 0.25f, 0.80f, 0.25f }, { 0.0f, 0.0f, 0.0f }
};

// One quest row: its dot, name, and how far away it is
static void draw_quest_row(const QuestLogState* ql, const list_row* row, const rect* r, float y,
                           float player_x, float player_y) {
	float cy = y + QL_ROW_H * 0.5f;
	const char* title;
	float target_x = 0.0f;
	float target_y = 0.0f;
	int has_target = 0;
	int picked;
	char distance[16] = "";
	char fitted[64];

	if(row->kind == ROW_AVAILABLE) {
		const available_quest_entry* e = &ql->available[row->index];

		title = e->title;
		has_target = quest_available_target(e, &target_x, &target_y);
		picked = ql->available_selected == row->index;
	} else {
		const QuestEntry* e = &ql->entries[row->index];

		title = e->title;
		has_target = row->kind == ROW_ACTIVE && quest_active_target(e, &target_x, &target_y);
		picked = row->kind == ROW_ACTIVE && ql->selected == row->index;
	}

	if(picked)
		ui_kit_px_rect(r->x + 6.0f, y, r->w - 12.0f, QL_ROW_H - 2.0f, 1.0f, 1.0f, 1.0f, 0.10f);

	ui_kit_px_rect(r->x + QL_PAD, cy - 4.0f, 8.0f, 8.0f, DOT_COLORS[row->kind].r,
	               DOT_COLORS[row->kind].g, DOT_COLORS[row->kind].b, 1.0f);

	if(has_target)
		quest_distance_text(quest_distance_m(player_x, player_y, target_x, target_y), distance,
		                    sizeof(distance));
	else if(row->kind != ROW_DONE)
		snprintf(distance, sizeof(distance), "--");

	fit_text(title, r->w - QL_PAD * 2.0f - 24.0f - 64.0f, fitted, sizeof(fitted));
	ui_kit_px_text(r->x + QL_PAD + 18.0f, cy, fitted, QL_TEXT,
	               row->kind == ROW_DONE ? KIT_DIM : KIT_WHITE, KIT_LEFT);
	if(distance[0])
		ui_kit_px_text(r->x + r->w - QL_PAD, cy, distance, QL_TEXT, KIT_DIM, KIT_RIGHT);
}

static void draw_list(const QuestLogState* ql, const rect* r, const list_row* rows, int count,
                      float player_x, float player_y) {
	float y = r->y + QL_TITLE_H;

	draw_frame(r, "Quests");

	if(count == 0)
		ui_kit_px_text(r->x + r->w * 0.5f, y + QL_ROW_H * 0.5f, "No quests yet.", QL_TEXT,
		               KIT_DIM, KIT_CENTER);

	for(int i = 0; i < count; i++) {
		char more[32];

		if(rows[i].kind == ROW_HEAD) {
			ui_kit_px_text(r->x + QL_PAD, y + QL_HEAD_H * 0.5f, rows[i].label, QL_SMALL,
			               KIT_HEAD, KIT_LEFT);
		} else if(rows[i].kind == ROW_MORE) {
			snprintf(more, sizeof(more), "+ %d more", rows[i].index);
			ui_kit_px_text(r->x + QL_PAD, y + QL_HEAD_H * 0.5f, more, QL_SMALL, KIT_DIM, KIT_LEFT);
		} else {
			draw_quest_row(ql, &rows[i], r, y, player_x, player_y);
		}
		y += row_height(rows[i].kind);
	}
}

static void draw_detail(const QuestLogState* ql, const rect* r) {
	detail_layout layout;

	draw_frame(r, detail_title(ql));
	layout = detail_pass(ql, r->x, r->y, 1);
	if(layout.has_btn)
		ui_kit_px_button(layout.btn_x, layout.btn_y, QL_BTN_W, QL_BTN_H, "Abandon", 0,
		                 KIT_BTN_NORMAL);
}

/**
 * Render the open quest list, and the details of the quest picked from it.
 */
void quest_log_render(const QuestLogState* ql, int vw, int vh, float player_x, float player_y) {
	list_row rows[QL_MAX_ROWS + 1];
	int count;
	rect list;

	if(!ql->is_open)
		return;

	count = build_rows(ql, rows, QL_MAX_ROWS);
	list = list_rect(ql, vw, vh, rows, count);

	ui_kit_px_begin(vw, vh);
	draw_list(ql, &list, rows, count, player_x, player_y);
	if(detail_open(ql)) {
		rect detail = detail_rect(ql, vw, vh, &list);

		draw_detail(ql, &detail);
	}
	ui_kit_px_end();
}

// --- Input ------------------------------------------------------------------

// Drag the details window; it follows the list until the player has moved it
static int drag_detail(QuestLogState* ql, int vw, int vh, const rect* detail, float mx, float my,
                       int clicked, int down) {
	return window_pos_drag_from(&ql->detail_win, detail->x, detail->y, (float)vw, (float)vh,
	                            detail->w, detail->h, detail->w - QL_CLOSE - 20.0f, QL_TITLE_H, mx,
	                            my, clicked, down);
}

// A click on the list: a quest row picks it
static void click_list(QuestLogState* ql, const rect* list, const list_row* rows, int count,
                       float my) {
	float y = list->y + QL_TITLE_H;

	for(int i = 0; i < count; i++) {
		float h = row_height(rows[i].kind);

		if(my >= y && my < y + h) {
			toggle_row(ql, &rows[i]);
			return;
		}
		y += h;
	}
}

// Put the picked quest down, which closes its details
static void drop_selection(QuestLogState* ql) {
	ql->selected = -1;
	ql->available_selected = -1;
}

/**
 * Toggle, close, drag, or pick entries in the quest windows.
 *
 * @param clicked  Nonzero on a new pointer click.
 * @param down  Nonzero while the left button is held.
 * @param key_j  Nonzero on a new quest-log binding press.
 * @param key_esc  Nonzero on a new escape-key press.
 */
void quest_log_handle_input_full(QuestLogState* ql,
                                 float mx, float my, int clicked, int down,
                                 int key_j, int key_esc,
                                 int vw, int vh) {
	list_row rows[QL_MAX_ROWS + 1];
	int count;
	rect list;
	rect detail = { 0.0f, 0.0f, 0.0f, 0.0f };
	int has_detail;

	if(key_j) {
		ql->is_open = !ql->is_open;
		return;
	}
	if(!ql->is_open)
		return;
	if(key_esc) {
		if(detail_open(ql))
			drop_selection(ql);
		else
			ql->is_open = 0;
		return;
	}

	count = build_rows(ql, rows, QL_MAX_ROWS);
	list = list_rect(ql, vw, vh, rows, count);
	has_detail = detail_open(ql);
	if(has_detail)
		detail = detail_rect(ql, vw, vh, &list);

	// A window being carried takes everything until it is dropped
	if(has_detail && drag_detail(ql, vw, vh, &detail, mx, my, clicked, down))
		return;
	if(window_pos_drag(&ql->list_win, (float)vw, (float)vh, list.w, list.h,
	                   list.w - QL_CLOSE - 20.0f, QL_TITLE_H, mx, my, clicked, down))
		return;
	if(!clicked)
		return;

	if(has_detail && rect_has(&detail, mx, my)) {
		detail_layout layout;

		if(close_hit(&detail, mx, my)) {
			drop_selection(ql);
			return;
		}

		// The abandon button sits inside the window it belongs to
		layout = detail_pass(ql, detail.x, detail.y, 0);
		if(layout.has_btn && mx >= layout.btn_x && mx < layout.btn_x + QL_BTN_W &&
		   my >= layout.btn_y && my < layout.btn_y + QL_BTN_H)
			ql->pending_abandon = ql->entries[ql->selected].id;
		return;
	}

	if(rect_has(&list, mx, my)) {
		if(close_hit(&list, mx, my)) {
			drop_selection(ql);
			ql->is_open = 0;
			return;
		}
		click_list(ql, &list, rows, count, my);
		return;
	}

	// A click on the world behind closes both
	drop_selection(ql);
	ql->is_open = 0;
}
