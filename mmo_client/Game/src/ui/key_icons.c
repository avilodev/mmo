/**
 * @file
 * Draw a key or mouse button as its sprite, or as a text key cap without one.
 */
#include "ui/key_icons.h"
#include "ui/key_icon_file.h"
#include "core/keybinds.h"
#include "ui/ui_kit.h"
#include "renderer.h"
#include "texture/texture.h"
#include <stdio.h>

#define KEY_ICON_MAX      128
#define KEY_ICON_MARGIN   0.125f   // empty edge of a sprite, as a fraction of it
#define KEY_CAP_H         0.74f    // text key cap height, as a fraction of size
#define KEY_CAP_TEXT      0.62f    // its text, as a fraction of the cap height
#define KEY_CAP_PAD       0.30f    // text to cap edge, as a fraction of the cap

static unsigned int s_textures[KEY_ICON_MAX];
static int s_loaded;

void key_icons_load(void) {
	char path[160];

	if(s_loaded)
		return;

	for(int i = 0; i < key_icon_count() && i < KEY_ICON_MAX; i++) {
		snprintf(path, sizeof(path), "%s%s", KEY_ICON_DIR, key_icon_file_at(i));
		s_textures[i] = texture_load(path);
	}

	s_loaded = 1;
}

void key_icons_unload(void) {
	for(int i = 0; i < KEY_ICON_MAX; i++) {
		if(s_textures[i])
			texture_unload(s_textures[i]);

		s_textures[i] = 0;
	}

	s_loaded = 0;
}

// The sprite of a key, or 0 when it has none or did not load
static unsigned int sprite_of(int key) {
	int index = key_icon_index(key);

	if(index < 0 || index >= KEY_ICON_MAX)
		return 0;

	return s_textures[index];
}

static const char* cap_text(int key) {
	const char* name = keybinds_key_name(key);

	return name ? name : "-";
}

float key_icon_width(int key, float size) {
	float cap_h = size * KEY_CAP_H;
	float w;

	if(sprite_of(key))
		return size * (1.0f - 2.0f * KEY_ICON_MARGIN);

	w = ui_kit_text_width(cap_text(key), cap_h * KEY_CAP_TEXT) + 2.0f * cap_h * KEY_CAP_PAD;

	return w > cap_h ? w : cap_h;
}

float key_icon_draw(int key, float x, float cy, float size) {
	unsigned int sprite = sprite_of(key);
	float w = key_icon_width(key, size);
	float cap_h = size * KEY_CAP_H;

	if(sprite) {
		float margin = size * KEY_ICON_MARGIN;

		renderer_draw_sprite(x - margin, cy - size * 0.5f, size, size, sprite);

		return w;
	}

	renderer_draw_rect(x, cy - cap_h * 0.5f, w, cap_h, 0.92f, 0.92f, 0.95f, 1.0f);
	ui_kit_text(x + w * 0.5f, cy, cap_text(key), cap_h * KEY_CAP_TEXT, KIT_INK,
	            KIT_CENTER);

	return w;
}
