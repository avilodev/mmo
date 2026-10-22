/**
 * @file
 * Render the key bindings page and rebind actions from key presses.
 */
#include "ui/keybinds_page.h"
#include "ui/key_icons.h"
#include "ui/ui_kit.h"
#include "core/keybinds.h"
#include "input/input.h"
#include "renderer.h"
#include <string.h>

#define KP_ROW         56.0f
#define KP_HEADER_H    24.0f    // space above the list
#define KP_LABEL_X     50.0f
#define KP_KEY_FRAC    0.465f   // key column, as a fraction of the page width
#define KP_KEY_W       300.0f
#define KP_ICON        56.0f
#define KP_NOTCH_ROWS  2        // rows one wheel notch moves
#define KP_MAX_ENTRIES 64

static float s_x;
static float s_y;
static float s_w;
static float s_h;
static int s_scroll;            // first entry shown
static int s_capture = -1;      // action waiting for a key, or -1
static int s_click_used;        // this frame's click was taken as a binding

// Group headings and actions in list order; a heading is -1 - its first action
static int s_entries[KP_MAX_ENTRIES];
static int s_entry_count;

static void build_entries(void) {
	const char* last_group = NULL;

	if(s_entry_count)
		return;

	for(int i = 0; i < keybinds_action_count(); i++) {
		const keybind_action* action = keybinds_action_at(i);

		if(s_entry_count + 2 > KP_MAX_ENTRIES)
			break;

		if(!last_group || strcmp(last_group, action->group) != 0) {
			s_entries[s_entry_count++] = -1 - i;
			last_group = action->group;
		}

		s_entries[s_entry_count++] = i;
	}
}

static float key_x(void) {
	return s_x + s_w * KP_KEY_FRAC;
}

static float list_top(void) {
	return s_y + KP_HEADER_H;
}

static int visible_rows(void) {
	return (int)((s_y + s_h - 20.0f - list_top()) / KP_ROW);
}

static int max_scroll(void) {
	int most = s_entry_count - visible_rows();

	return most > 0 ? most : 0;
}

static float row_y(int slot) {
	return list_top() + KP_ROW * (float)slot;
}

void kp_enter(void) {
	build_entries();
	key_icons_load();
	s_scroll = 0;
	s_capture = -1;
}

void kp_leave(void) {
	key_icons_unload();
	s_capture = -1;
}

void kp_set_area(float x, float y, float w, float h) {
	s_x = x;
	s_y = y;
	s_w = w;
	s_h = h;
}

int kp_escape(void) {
	if(s_capture < 0)
		return 0;

	s_capture = -1;

	return 1;
}

int kp_capturing(void) {
	return s_capture >= 0;
}

int kp_scroll(float notches) {
	s_scroll -= (int)(notches * (float)KP_NOTCH_ROWS);

	if(s_scroll < 0)
		s_scroll = 0;

	if(s_scroll > max_scroll())
		s_scroll = max_scroll();

	return 1;
}

// The pointer is over the key column of this row
static int over_key(float mx, float my, int slot) {
	return ui_kit_hit(mx, my, key_x() - 12.0f, row_y(slot), KP_KEY_W, KP_ROW);
}

static void draw_group(int slot, const char* name) {
	float y = row_y(slot) + KP_ROW * 0.5f;
	float rule_x = s_x + KP_LABEL_X + ui_kit_text_width(name, 30.0f) + 20.0f;

	ui_kit_text(s_x + KP_LABEL_X, y, name, 30.0f, KIT_GOLD, KIT_LEFT);
	renderer_draw_rect(rule_x, y + 4.0f, s_x + s_w - 90.0f - rule_x, 2.0f,
	                   1.0f, 0.8f, 0.47f, 0.35f);
}

static void draw_action(int slot, int index, float mx, float my) {
	const keybind_action* action = keybinds_action_at(index);
	int key = keybinds_get(index);
	float y = row_y(slot);
	float cy = y + KP_ROW * 0.5f;
	float w;

	if(ui_kit_hit(mx, my, s_x + 20.0f, y, s_w - 80.0f, KP_ROW))
		renderer_draw_rect(s_x + 20.0f, y, s_w - 80.0f, KP_ROW, 1.0f, 1.0f, 1.0f, 0.09f);

	ui_kit_text(s_x + KP_LABEL_X, cy, action->label, 30.0f, KIT_WHITE, KIT_LEFT);

	if(index == s_capture) {
		ui_kit_field(key_x(), cy, KP_KEY_W, "Press a key...", 1);
		return;
	}

	w = key_icon_draw(key, key_x(), cy, KP_ICON);

	// A key changed from its default is underlined
	if(key != keybinds_default_key(index))
		renderer_draw_rect(key_x(), cy + KP_ICON * 0.5f - 4.0f, w, 3.0f,
		                   0.55f, 0.85f, 1.0f, 1.0f);
}

static void draw_scrollbar(void) {
	float track_y = list_top();
	float track_h = s_y + s_h - 20.0f - track_y;
	float thumb_h = track_h * (float)visible_rows() / (float)s_entry_count;
	float thumb_y = max_scroll() ? (track_h - thumb_h) * (float)s_scroll / (float)max_scroll() : 0.0f;

	if(max_scroll() == 0)
		return;

	renderer_draw_rect(s_x + s_w - 36.0f, track_y, 10.0f, track_h, 1.0f, 1.0f, 1.0f, 0.15f);
	renderer_draw_rect(s_x + s_w - 36.0f, track_y + thumb_y, 10.0f, thumb_h,
	                   0.47f, 0.63f, 0.86f, 0.9f);
}

void kp_draw(float mx, float my) {
	build_entries();

	for(int slot = 0; slot < visible_rows(); slot++) {
		int n = s_scroll + slot;

		if(n >= s_entry_count)
			break;

		if(s_entries[n] < 0)
			draw_group(slot, keybinds_action_at(-1 - s_entries[n])->group);
		else
			draw_action(slot, s_entries[n], mx, my);
	}

	draw_scrollbar();
}

// The slot showing this action, or -1 when it is scrolled out of view
static int slot_of(int index) {
	for(int slot = 0; slot < visible_rows(); slot++) {
		int n = s_scroll + slot;

		if(n >= s_entry_count)
			break;

		if(s_entries[n] == index)
			return slot;
	}

	return -1;
}

void kp_click(float mx, float my) {
	if(s_click_used)
		return;

	for(int slot = 0; slot < visible_rows(); slot++) {
		int n = s_scroll + slot;

		if(n >= s_entry_count)
			break;

		if(s_entries[n] >= 0 && over_key(mx, my, slot))
			s_capture = s_entries[n];
	}
}

void kp_poll_keys(float mx, float my, const InputState* input) {
	int slot;

	s_click_used = 0;

	if(s_capture < 0)
		return;

	for(int n = 0; keybinds_nameable_key(n) >= 0; n++) {
		int key = keybinds_nameable_key(n);

		if(key == GLFW_KEY_ESCAPE || !input_key_just_pressed(input, key))
			continue;

		// A left click off the box being set gives up on it
		if(key == KEYBIND_MOUSE_LEFT) {
			slot = slot_of(s_capture);

			if(slot < 0 || !over_key(mx, my, slot)) {
				s_capture = -1;
				return;
			}

			s_click_used = 1;
		}

		keybinds_set(s_capture, key);
		s_capture = -1;
		return;
	}
}
