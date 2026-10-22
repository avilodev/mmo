/**
 * @file
 * Check which sprite each bindable key gets on the key bindings page.
 *
 * Run from mmo_client, where the sprite paths resolve. A key with no sprite
 * must come back NULL so the page draws a text key cap for it instead.
 */
#include "ui/key_icon_file.h"
#include "core/keybinds.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_every_file_exists(void) {
	char path[160];

	for(int i = 0; i < key_icon_count(); i++) {
		FILE* f;

		snprintf(path, sizeof(path), "%s%s", KEY_ICON_DIR, key_icon_file_at(i));
		f = fopen(path, "rb");

		if(!f)
			printf("  missing sprite %s\n", path);

		assert(f);
		fclose(f);
	}
}

static void test_known_keys(void) {
	assert(strcmp(key_icon_file(GLFW_KEY_W), "keyboard_w.png") == 0);
	assert(strcmp(key_icon_file(GLFW_KEY_0), "keyboard_0.png") == 0);
	assert(strcmp(key_icon_file(GLFW_KEY_F12), "keyboard_f12.png") == 0);
	assert(strcmp(key_icon_file(GLFW_KEY_TAB), "keyboard_tab.png") == 0);
	assert(strcmp(key_icon_file(GLFW_KEY_EQUAL), "keyboard_equals.png") == 0);
	assert(strcmp(key_icon_file(KEYBIND_MOUSE_LEFT), "mouse_left.png") == 0);
	assert(strcmp(key_icon_file(KEYBIND_MOUSE_RIGHT), "mouse_right.png") == 0);
}

// Left and right share one sprite, as the pack has no split
static void test_modifiers_share_a_sprite(void) {
	assert(strcmp(key_icon_file(GLFW_KEY_LEFT_SHIFT), "keyboard_shift.png") == 0);
	assert(strcmp(key_icon_file(GLFW_KEY_RIGHT_SHIFT), "keyboard_shift.png") == 0);
	assert(strcmp(key_icon_file(GLFW_KEY_LEFT_CONTROL), "keyboard_ctrl.png") == 0);
	assert(strcmp(key_icon_file(GLFW_KEY_RIGHT_CONTROL), "keyboard_ctrl.png") == 0);
	assert(strcmp(key_icon_file(GLFW_KEY_LEFT_ALT), "keyboard_alt.png") == 0);
	assert(strcmp(key_icon_file(GLFW_KEY_RIGHT_ALT), "keyboard_alt.png") == 0);
}

static void test_keys_without_a_sprite(void) {
	assert(!key_icon_file(GLFW_KEY_KP_7));
	assert(!key_icon_file(GLFW_KEY_KP_DIVIDE));
	assert(!key_icon_file(KEYBIND_MOUSE_BASE + GLFW_MOUSE_BUTTON_4));
	assert(!key_icon_file(GLFW_KEY_UNKNOWN));
	assert(key_icon_index(GLFW_KEY_UNKNOWN) == -1);
	assert(!key_icon_file_at(-1));
	assert(!key_icon_file_at(key_icon_count()));
}

// Letters, digits and function keys are common binds; none may lack a sprite
static void test_common_keys_covered(void) {
	for(int key = GLFW_KEY_A; key <= GLFW_KEY_Z; key++)
		assert(key_icon_file(key));

	for(int key = GLFW_KEY_0; key <= GLFW_KEY_9; key++)
		assert(key_icon_file(key));

	for(int key = GLFW_KEY_F1; key <= GLFW_KEY_F12; key++)
		assert(key_icon_file(key));
}

// A sprite for a key the menu cannot bind would be dead weight
static void test_sprites_are_bindable(void) {
	int covered = 0;

	for(int n = 0; keybinds_nameable_key(n) >= 0; n++) {
		if(key_icon_file(keybinds_nameable_key(n)))
			covered++;
	}

	assert(covered == key_icon_count());
}

int main(void) {
	test_every_file_exists();
	test_known_keys();
	test_modifiers_share_a_sprite();
	test_keys_without_a_sprite();
	test_common_keys_covered();
	test_sprites_are_bindable();
	printf("key_icon_file_test: OK\n");

	return 0;
}
