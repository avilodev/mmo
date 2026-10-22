/**
 * @file
 * Draw the sprite-based menu widgets.
 */
#include "ui/ui_kit.h"
#include "renderer.h"
#include "texture/texture.h"
#include "core/client_log.h"
#include <math.h>
#include <stdio.h>

#define KIT_DIR        "Game/Sprites/UI/Kit/"

const kit_color KIT_WHITE = { 0.94f, 0.93f, 0.91f };
const kit_color KIT_DIM = { 0.67f, 0.66f, 0.69f };
const kit_color KIT_GOLD = { 1.00f, 0.80f, 0.47f };
const kit_color KIT_INK = { 0.24f, 0.16f, 0.09f };
const kit_color KIT_HEAD = { 0.55f, 0.65f, 0.78f };

typedef enum {
	S_FRAME_GREY,
	S_FRAME_ORANGE,
	S_BTN,
	S_BTN_HOVER,
	S_BTN_DOWN,
	S_BTN_ROUND,
	S_BAR,
	S_BAR_FILL,
	S_HANDLE,
	S_FIELD,
	S_ARROW,
	S_CHECK,
	S_CROSS,
	S_DROPDOWN,
	S_TOGGLE_ON,
	S_TOGGLE_OFF,
	S_PANEL_FRAME,
	S_RIBBON_L,
	S_RIBBON_M,
	S_RIBBON_R,
	S_COUNT,
} sprite_id;

typedef struct {
	const char* file;
	int w;
	int h;
	unsigned int texture;
} kit_sprite;

static kit_sprite SPRITES[S_COUNT] = {
	[S_FRAME_GREY] = { "UI_Flat_Frame01a.png", 96, 64, 0 },
	[S_FRAME_ORANGE] = { "UI_Flat_Frame03a.png", 96, 64, 0 },
	[S_BTN] = { "UI_Flat_Button01a_1.png", 32, 32, 0 },
	[S_BTN_HOVER] = { "UI_Flat_Button01a_2.png", 32, 32, 0 },
	[S_BTN_DOWN] = { "UI_Flat_Button01a_3.png", 32, 32, 0 },
	[S_BTN_ROUND] = { "UI_Flat_Button02a_1.png", 32, 32, 0 },
	[S_BAR] = { "UI_Flat_Bar01a.png", 32, 8, 0 },
	[S_BAR_FILL] = { "UI_Flat_BarFill01a.png", 32, 3, 0 },
	[S_HANDLE] = { "UI_Flat_Handle03a.png", 8, 10, 0 },
	[S_FIELD] = { "UI_Flat_InputField01a.png", 64, 32, 0 },
	[S_ARROW] = { "UI_Flat_IconArrow01a.png", 16, 12, 0 },
	[S_CHECK] = { "UI_Flat_IconCheck01a.png", 17, 14, 0 },
	[S_CROSS] = { "UI_Flat_IconCross01a.png", 15, 15, 0 },
	[S_DROPDOWN] = { "UI_Flat_IconDropdown01a.png", 7, 5, 0 },
	[S_TOGGLE_ON] = { "UI_Flat_ToggleOn01a.png", 10, 10, 0 },
	[S_TOGGLE_OFF] = { "UI_Flat_ToggleOff01a.png", 10, 10, 0 },
	[S_PANEL_FRAME] = { "kenney_panel_frame.png", 32, 32, 0 },
	[S_RIBBON_L] = { "kenney_ribbon_left.png", 32, 32, 0 },
	[S_RIBBON_M] = { "kenney_ribbon_mid.png", 32, 32, 0 },
	[S_RIBBON_R] = { "kenney_ribbon_right.png", 32, 32, 0 },
};

static int s_loaded;
static float s_scale = 1.0f;    // window pixels per layout unit, while drawing

kit_frame ui_kit_frame(int viewport_w, int viewport_h, float ui_scale) {
	kit_frame frame;

	frame.scale = (float)viewport_h / KIT_DESIGN_H * ui_scale;

	if(frame.scale < 0.05f)
		frame.scale = 0.05f;

	frame.w = (float)viewport_w / frame.scale;
	frame.h = (float)viewport_h / frame.scale;

	return frame;
}

void ui_kit_to_layout(const kit_frame* frame, float px, float py, float* x, float* y) {
	*x = px / frame->scale;
	*y = py / frame->scale;
}

int ui_kit_hit(float mx, float my, float x, float y, float w, float h) {
	return mx >= x && mx <= x + w && my >= y && my <= y + h;
}

kit_slider_geom ui_kit_slider_geom(float x, float w) {
	kit_slider_geom g;

	g.left_x = x;
	g.right_x = x + w - KIT_ARROW;
	g.track_x = x + KIT_ARROW + KIT_ARROW_GAP;
	g.track_w = w - 2.0f * (KIT_ARROW + KIT_ARROW_GAP);
	g.inner_x = g.track_x + 2.0f * KIT_PX;
	g.inner_w = g.track_w - 4.0f * KIT_PX;

	return g;
}

void ui_kit_load(void) {
	char path[160];

	if(s_loaded)
		return;

	for(int i = 0; i < S_COUNT; i++) {
		snprintf(path, sizeof(path), "%s%s", KIT_DIR, SPRITES[i].file);
		SPRITES[i].texture = texture_load(path);

		if(!SPRITES[i].texture)
			CLOG_WARN("[UI_KIT] Could not load %s", path);
	}

	s_loaded = 1;
}

void ui_kit_begin(const kit_frame* frame) {
	s_scale = frame->scale;
	ui_kit_load();
	glMatrixMode(GL_MODELVIEW);
	glPushMatrix();
	glScalef(frame->scale, frame->scale, 1.0f);
}

void ui_kit_end(void) {
	glMatrixMode(GL_MODELVIEW);
	glPopMatrix();
}

// Pixel-space widgets: the same drawing, with positions and sizes in pixels
void ui_kit_px_begin(int viewport_w, int viewport_h) {
	kit_frame frame = ui_kit_frame(viewport_w, viewport_h, 1.0f);

	ui_kit_begin(&frame);
}

void ui_kit_px_end(void) {
	ui_kit_end();
}

void ui_kit_px_rect(float x, float y, float w, float h, float r, float g, float b, float a) {
	renderer_draw_rect(x / s_scale, y / s_scale, w / s_scale, h / s_scale, r, g, b, a);
}

void ui_kit_px_panel(float x, float y, float w, float h) {
	ui_kit_panel(x / s_scale, y / s_scale, w / s_scale, h / s_scale);
}

void ui_kit_px_divider(float x, float y, float w) {
	ui_kit_divider(x / s_scale, y / s_scale, w / s_scale);
}

void ui_kit_px_text(float x, float cy, const char* text, float size, kit_color color,
                    kit_align align) {
	ui_kit_text(x / s_scale, cy / s_scale, text, size / s_scale, color, align);
}

float ui_kit_px_text_width(const char* text, float size) {
	return ui_kit_text_width(text, size / s_scale) * s_scale;
}

void ui_kit_px_tab(float x, float y, float w, float h, const char* label, int active,
                   int hover) {
	ui_kit_tab(x / s_scale, y / s_scale, w / s_scale, h / s_scale, label, active, hover);
}

void ui_kit_px_button(float x, float y, float w, float h, const char* label, int primary,
                      kit_btn_state state) {
	ui_kit_button(x / s_scale, y / s_scale, w / s_scale, h / s_scale, label, primary, state);
}

void ui_kit_px_close_button(float x, float y, float size, int hover) {
	ui_kit_close_button(x / s_scale, y / s_scale, size / s_scale, hover);
}

// One piece of a sprite; a flat grey block stands in when it did not load
static void quad(sprite_id id, float x, float y, float w, float h,
                 float u0, float v0, float u1, float v1) {
	if(w <= 0.0f || h <= 0.0f)
		return;

	if(SPRITES[id].texture)
		renderer_draw_sprite_uv(x, y, w, h, SPRITES[id].texture, u0, v0, u1, v1);
	else
		renderer_draw_rect(x, y, w, h, 0.6f, 0.6f, 0.65f, 1.0f);
}

static void sprite(sprite_id id, float x, float y, float w, float h) {
	quad(id, x, y, w, h, 0.0f, 0.0f, 1.0f, 1.0f);
}

// Layout units for a sprite pixel drawn about nominal units wide, made a whole
// number of window pixels so the art stays crisp
static float art_px(float nominal) {
	float pixels = floorf(nominal * s_scale + 0.5f);

	if(pixels < 1.0f)
		pixels = 1.0f;

	return pixels / s_scale;
}

static void sprite_centered(sprite_id id, float cx, float cy, float scale) {
	float w = (float)SPRITES[id].w * art_px(scale);
	float h = (float)SPRITES[id].h * art_px(scale);

	sprite(id, cx - w * 0.5f, cy - h * 0.5f, w, h);
}

/**
 * Draw a sprite with fixed corners and stretched edges and middle.
 *
 * @param margin  Corner size in sprite pixels.
 * @param px      Layout units per sprite pixel.
 */
static void nine(sprite_id id, float x, float y, float w, float h, float margin,
                 float px) {
	float sw = (float)SPRITES[id].w;
	float sh = (float)SPRITES[id].h;
	float cx = margin * art_px(px);
	float cy = margin * art_px(px);
	float us[4] = { 0.0f, margin / sw, 1.0f - margin / sw, 1.0f };
	float vs[4] = { 0.0f, margin / sh, 1.0f - margin / sh, 1.0f };
	float xs[4];
	float ys[4];

	if(cx * 2.0f > w)
		cx = w * 0.5f;

	if(cy * 2.0f > h)
		cy = h * 0.5f;

	xs[0] = x;
	xs[1] = x + cx;
	xs[2] = x + w - cx;
	xs[3] = x + w;
	ys[0] = y;
	ys[1] = y + cy;
	ys[2] = y + h - cy;
	ys[3] = y + h;

	for(int j = 0; j < 3; j++) {
		for(int i = 0; i < 3; i++)
			quad(id, xs[i], ys[j], xs[i + 1] - xs[i], ys[j + 1] - ys[j],
			     us[i], vs[j], us[i + 1], vs[j + 1]);
	}
}

// The bake whose size is nearest the text's size on screen
static int text_level(float size) {
	return renderer_font_level_for(size * s_scale);
}

float ui_kit_text_width(const char* text, float size) {
	int level = text_level(size);

	return renderer_text_width_level(text, level) * size / renderer_font_level_size(level);
}

void ui_kit_text(float x, float cy, const char* text, float size, kit_color color,
                 kit_align align) {
	int level = text_level(size);
	float k = size / renderer_font_level_size(level);
	float w = renderer_text_width_level(text, level) * k;

	if(align == KIT_CENTER)
		x -= w * 0.5f;
	else if(align == KIT_RIGHT)
		x -= w;

	glPushMatrix();
	glTranslatef(x, cy + size * 0.35f, 0.0f);
	glScalef(k, k, 1.0f);
	renderer_draw_text_level(0.0f, 0.0f, text, level, color.r, color.g, color.b);
	glPopMatrix();
}

void ui_kit_dim(float x, float y, float w, float h, float alpha) {
	renderer_draw_rect(x, y, w, h, 0.0f, 0.0f, 0.0f, alpha);
}

static void tint(float x, float y, float w, float h, float alpha) {
	renderer_draw_rect(x, y, w, h, 1.0f, 1.0f, 1.0f, alpha);
}

void ui_kit_divider(float x, float y, float w) {
	renderer_draw_rect(x, y, w, 1.5f, 1.0f, 1.0f, 1.0f, 0.15f);
}

void ui_kit_panel(float x, float y, float w, float h) {
	renderer_draw_rect(x, y, w, h, 0.04f, 0.04f, 0.07f, 0.62f);
	nine(S_PANEL_FRAME, x - 6.0f, y - 6.0f, w + 12.0f, h + 12.0f, 12.0f, 1.0f);
}

void ui_kit_tab(float x, float y, float w, float h, const char* label, int active,
                int hover) {
	nine(active ? S_FRAME_ORANGE : S_FRAME_GREY, x, y, w, h, 4.0f, KIT_PX);

	if(hover && !active)
		tint(x + 6.0f, y + 6.0f, w - 12.0f, h - 12.0f, 0.18f);

	ui_kit_text(x + w * 0.5f, y + h * 0.5f - 2.0f, label, 30.0f, KIT_INK, KIT_CENTER);
}

void ui_kit_button(float x, float y, float w, float h, const char* label,
                   int primary, kit_btn_state state) {
	if(primary) {
		nine(S_FRAME_ORANGE, x, y, w, h, 4.0f, KIT_PX);

		if(state != KIT_BTN_NORMAL)
			tint(x + 6.0f, y + 6.0f, w - 12.0f, h - 12.0f, 0.2f);
	} else {
		nine(state == KIT_BTN_DOWN ? S_BTN_DOWN :
		     state == KIT_BTN_HOVER ? S_BTN_HOVER : S_BTN,
		     x, y, w, h, 5.0f, 2.0f);
	}

	ui_kit_text(x + w * 0.5f, y + h * 0.5f - 2.0f, label, 30.0f, KIT_INK, KIT_CENTER);
}

void ui_kit_close_button(float x, float y, float size, int hover) {
	nine(S_BTN_ROUND, x, y, size, size, 6.0f, 2.0f);

	if(hover)
		tint(x + 6.0f, y + 6.0f, size - 12.0f, size - 12.0f, 0.2f);

	sprite_centered(S_CROSS, x + size * 0.5f, y + size * 0.5f - 1.0f, 2.0f);
}

void ui_kit_ribbon(float x, float y, float w, const char* label) {
	float tile = 32.0f * art_px(2.0f);

	sprite(S_RIBBON_M, x + tile, y, w - 2.0f * tile, tile);
	sprite(S_RIBBON_L, x, y, tile, tile);
	sprite(S_RIBBON_R, x + w - tile, y, tile, tile);
	ui_kit_text(x + w * 0.5f, y + tile * 0.5f - 2.0f, label, 34.0f, KIT_WHITE,
	            KIT_CENTER);
}

static void slider_arrow(float x, float cy, int right, int hover) {
	nine(hover ? S_BTN_HOVER : S_BTN, x, cy - KIT_ARROW * 0.5f, KIT_ARROW, KIT_ARROW,
	     5.0f, 2.0f);

	if(right) {
		sprite_centered(S_ARROW, x + KIT_ARROW * 0.5f, cy - 1.0f, 1.5f);
		return;
	}

	// The sprite points right; the left arrow is it mirrored
	float aw = 16.0f * art_px(1.5f);
	float ah = 12.0f * art_px(1.5f);

	quad(S_ARROW, x + (KIT_ARROW - aw) * 0.5f, cy - ah * 0.5f - 1.0f, aw, ah,
	     1.0f, 0.0f, 0.0f, 1.0f);
}

void ui_kit_slider(float x, float cy, float w, float t, int hover_parts) {
	kit_slider_geom g = ui_kit_slider_geom(x, w);
	float fill_w;
	float hw;
	float hh;

	if(t < 0.0f)
		t = 0.0f;

	if(t > 1.0f)
		t = 1.0f;

	slider_arrow(g.left_x, cy, 0, hover_parts & KIT_SLIDER_LEFT);
	slider_arrow(g.right_x, cy, 1, hover_parts & KIT_SLIDER_RIGHT);
	nine(S_BAR, g.track_x, cy - 12.0f, g.track_w, 24.0f, 3.0f, KIT_PX);

	fill_w = g.inner_w * t;
	sprite(S_BAR_FILL, g.inner_x, cy - 4.5f, fill_w, 9.0f);
	hw = 8.0f * art_px(KIT_PX);
	hh = 10.0f * art_px(KIT_PX);
	sprite(S_HANDLE, g.inner_x + fill_w - hw * 0.5f, cy - hh * 0.5f, hw, hh);
}

void ui_kit_toggle(float x, float cy, int on) {
	float size = 10.0f * art_px(KIT_PX);

	sprite(on ? S_TOGGLE_ON : S_TOGGLE_OFF, x, cy - size * 0.5f, size, size);
}

void ui_kit_field(float x, float cy, float w, const char* text, int open) {
	nine(S_FIELD, x, cy - KIT_FIELD_H * 0.5f, w, KIT_FIELD_H, 5.0f, 2.0f);

	if(open)
		tint(x + 6.0f, cy - KIT_FIELD_H * 0.5f + 6.0f, w - 12.0f,
		     KIT_FIELD_H - 12.0f, 0.25f);

	ui_kit_text(x + 20.0f, cy - 2.0f, text, 26.0f, KIT_INK, KIT_LEFT);
	sprite_centered(S_DROPDOWN, x + w - 34.0f, cy, 4.0f);
}

void ui_kit_check_icon(float cx, float cy) {
	sprite_centered(S_CHECK, cx, cy, 2.0f);
}
