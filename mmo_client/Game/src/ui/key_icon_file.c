/**
 * @file
 * Map each bindable key to the sprite that shows it.
 */
#include "ui/key_icon_file.h"
#include "core/keybinds.h"
#include <stddef.h>

typedef struct {
	int key;
	const char* file;
} key_icon_entry;

// Keys left out have no sprite and are drawn as a text key cap
static const key_icon_entry KEY_ICONS[] = {
	{GLFW_KEY_A, "keyboard_a.png"}, {GLFW_KEY_B, "keyboard_b.png"},
	{GLFW_KEY_C, "keyboard_c.png"}, {GLFW_KEY_D, "keyboard_d.png"},
	{GLFW_KEY_E, "keyboard_e.png"}, {GLFW_KEY_F, "keyboard_f.png"},
	{GLFW_KEY_G, "keyboard_g.png"}, {GLFW_KEY_H, "keyboard_h.png"},
	{GLFW_KEY_I, "keyboard_i.png"}, {GLFW_KEY_J, "keyboard_j.png"},
	{GLFW_KEY_K, "keyboard_k.png"}, {GLFW_KEY_L, "keyboard_l.png"},
	{GLFW_KEY_M, "keyboard_m.png"}, {GLFW_KEY_N, "keyboard_n.png"},
	{GLFW_KEY_O, "keyboard_o.png"}, {GLFW_KEY_P, "keyboard_p.png"},
	{GLFW_KEY_Q, "keyboard_q.png"}, {GLFW_KEY_R, "keyboard_r.png"},
	{GLFW_KEY_S, "keyboard_s.png"}, {GLFW_KEY_T, "keyboard_t.png"},
	{GLFW_KEY_U, "keyboard_u.png"}, {GLFW_KEY_V, "keyboard_v.png"},
	{GLFW_KEY_W, "keyboard_w.png"}, {GLFW_KEY_X, "keyboard_x.png"},
	{GLFW_KEY_Y, "keyboard_y.png"}, {GLFW_KEY_Z, "keyboard_z.png"},

	{GLFW_KEY_0, "keyboard_0.png"}, {GLFW_KEY_1, "keyboard_1.png"},
	{GLFW_KEY_2, "keyboard_2.png"}, {GLFW_KEY_3, "keyboard_3.png"},
	{GLFW_KEY_4, "keyboard_4.png"}, {GLFW_KEY_5, "keyboard_5.png"},
	{GLFW_KEY_6, "keyboard_6.png"}, {GLFW_KEY_7, "keyboard_7.png"},
	{GLFW_KEY_8, "keyboard_8.png"}, {GLFW_KEY_9, "keyboard_9.png"},

	{GLFW_KEY_F1, "keyboard_f1.png"}, {GLFW_KEY_F2, "keyboard_f2.png"},
	{GLFW_KEY_F3, "keyboard_f3.png"}, {GLFW_KEY_F4, "keyboard_f4.png"},
	{GLFW_KEY_F5, "keyboard_f5.png"}, {GLFW_KEY_F6, "keyboard_f6.png"},
	{GLFW_KEY_F7, "keyboard_f7.png"}, {GLFW_KEY_F8, "keyboard_f8.png"},
	{GLFW_KEY_F9, "keyboard_f9.png"}, {GLFW_KEY_F10, "keyboard_f10.png"},
	{GLFW_KEY_F11, "keyboard_f11.png"}, {GLFW_KEY_F12, "keyboard_f12.png"},

	{GLFW_KEY_SPACE, "keyboard_space.png"},
	{GLFW_KEY_ENTER, "keyboard_enter.png"},
	{GLFW_KEY_TAB, "keyboard_tab.png"},
	{GLFW_KEY_BACKSPACE, "keyboard_backspace.png"},
	{GLFW_KEY_HOME, "keyboard_home.png"},
	{GLFW_KEY_END, "keyboard_end.png"},
	{GLFW_KEY_PAGE_UP, "keyboard_page_up.png"},
	{GLFW_KEY_PAGE_DOWN, "keyboard_page_down.png"},
	{GLFW_KEY_INSERT, "keyboard_insert.png"},
	{GLFW_KEY_DELETE, "keyboard_delete.png"},
	{GLFW_KEY_CAPS_LOCK, "keyboard_capslock.png"},

	{GLFW_KEY_LEFT, "keyboard_arrow_left.png"},
	{GLFW_KEY_RIGHT, "keyboard_arrow_right.png"},
	{GLFW_KEY_UP, "keyboard_arrow_up.png"},
	{GLFW_KEY_DOWN, "keyboard_arrow_down.png"},

	{GLFW_KEY_LEFT_SHIFT, "keyboard_shift.png"},
	{GLFW_KEY_RIGHT_SHIFT, "keyboard_shift.png"},
	{GLFW_KEY_LEFT_CONTROL, "keyboard_ctrl.png"},
	{GLFW_KEY_RIGHT_CONTROL, "keyboard_ctrl.png"},
	{GLFW_KEY_LEFT_ALT, "keyboard_alt.png"},
	{GLFW_KEY_RIGHT_ALT, "keyboard_alt.png"},

	{GLFW_KEY_KP_ENTER, "keyboard_numpad_enter.png"},
	{GLFW_KEY_KP_ADD, "keyboard_numpad_plus.png"},

	{GLFW_KEY_MINUS, "keyboard_minus.png"},
	{GLFW_KEY_EQUAL, "keyboard_equals.png"},
	{GLFW_KEY_COMMA, "keyboard_comma.png"},
	{GLFW_KEY_PERIOD, "keyboard_period.png"},
	{GLFW_KEY_SEMICOLON, "keyboard_semicolon.png"},
	{GLFW_KEY_APOSTROPHE, "keyboard_apostrophe.png"},
	{GLFW_KEY_LEFT_BRACKET, "keyboard_bracket_open.png"},
	{GLFW_KEY_RIGHT_BRACKET, "keyboard_bracket_close.png"},
	{GLFW_KEY_BACKSLASH, "keyboard_slash_back.png"},
	{GLFW_KEY_SLASH, "keyboard_slash_forward.png"},
	{GLFW_KEY_GRAVE_ACCENT, "keyboard_tilde.png"},

	{KEYBIND_MOUSE_LEFT, "mouse_left.png"},
	{KEYBIND_MOUSE_RIGHT, "mouse_right.png"},
	{KEYBIND_MOUSE_MIDDLE, "mouse_scroll.png"},
};

#define KEY_ICON_COUNT ((int)(sizeof(KEY_ICONS) / sizeof(KEY_ICONS[0])))

int key_icon_index(int key) {
	for(int i = 0; i < KEY_ICON_COUNT; i++) {
		if(KEY_ICONS[i].key == key)
			return i;
	}

	return -1;
}

const char* key_icon_file(int key) {
	return key_icon_file_at(key_icon_index(key));
}

int key_icon_count(void) {
	return KEY_ICON_COUNT;
}

const char* key_icon_file_at(int index) {
	if(index < 0 || index >= KEY_ICON_COUNT)
		return NULL;

	return KEY_ICONS[index].file;
}
