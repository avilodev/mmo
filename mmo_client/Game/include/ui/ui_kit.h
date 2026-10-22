#ifndef UI_KIT_H
#define UI_KIT_H

/**
 * @file
 * Declare the sprite-based widgets the settings and pause menus are built from.
 *
 * Menus lay themselves out in layout units, a screen 1800 units high whatever
 * the window is, and ui_kit_begin() scales that onto the real window. Sprite
 * art is drawn a whole number of window pixels per art pixel so it stays crisp.
 */

#define KIT_DESIGN_H   1800.0f
#define KIT_PX         3.0f      // layout units per sprite pixel
#define KIT_ARROW      44.0f     // slider arrow buttons
#define KIT_ARROW_GAP  12.0f
#define KIT_TOGGLE     36.0f
#define KIT_FIELD_H    52.0f     // dropdown box height

// Where a menu is drawn: layout units onto window pixels
typedef struct {
	float scale;    // window pixels per layout unit
	float w;        // window size in layout units
	float h;
} kit_frame;

typedef struct {
	float r;
	float g;
	float b;
} kit_color;

typedef enum {
	KIT_LEFT,
	KIT_CENTER,
	KIT_RIGHT,
} kit_align;

typedef enum {
	KIT_BTN_NORMAL,
	KIT_BTN_HOVER,
	KIT_BTN_DOWN,
} kit_btn_state;

// Slider parts: the two arrow buttons, and the track between them
typedef struct {
	float left_x;
	float right_x;
	float track_x;
	float track_w;
	float inner_x;    // the part of the track the handle travels
	float inner_w;
} kit_slider_geom;

// Which parts of a slider the pointer is on, for ui_kit_slider()
#define KIT_SLIDER_LEFT  1
#define KIT_SLIDER_RIGHT 2

extern const kit_color KIT_WHITE;
extern const kit_color KIT_DIM;
extern const kit_color KIT_GOLD;
extern const kit_color KIT_INK;
extern const kit_color KIT_HEAD;

kit_frame ui_kit_frame(int viewport_w, int viewport_h, float ui_scale);

// A window pixel position in layout units
void ui_kit_to_layout(const kit_frame* frame, float px, float py, float* x, float* y);

int ui_kit_hit(float mx, float my, float x, float y, float w, float h);

kit_slider_geom ui_kit_slider_geom(float x, float w);

// Load the sprites; safe to call again
void ui_kit_load(void);

// Draw in layout units between these
void ui_kit_begin(const kit_frame* frame);
void ui_kit_end(void);

// Text is baked at one size and scaled; cy is the middle of the line
void ui_kit_text(float x, float cy, const char* text, float size, kit_color color,
                 kit_align align);
float ui_kit_text_width(const char* text, float size);

void ui_kit_dim(float x, float y, float w, float h, float alpha);
void ui_kit_divider(float x, float y, float w);
void ui_kit_panel(float x, float y, float w, float h);
void ui_kit_tab(float x, float y, float w, float h, const char* label, int active,
                int hover);
void ui_kit_button(float x, float y, float w, float h, const char* label,
                   int primary, kit_btn_state state);
void ui_kit_close_button(float x, float y, float size, int hover);
void ui_kit_ribbon(float x, float y, float w, const char* label);
void ui_kit_slider(float x, float cy, float w, float t, int hover_parts);
void ui_kit_toggle(float x, float cy, int on);
void ui_kit_field(float x, float cy, float w, const char* text, int open);
void ui_kit_check_icon(float cx, float cy);

// The widgets above for pixel-laid-out windows; sizes here are pixels
void ui_kit_px_begin(int viewport_w, int viewport_h);
void ui_kit_px_end(void);
void ui_kit_px_rect(float x, float y, float w, float h, float r, float g, float b, float a);
void ui_kit_px_panel(float x, float y, float w, float h);
void ui_kit_px_divider(float x, float y, float w);
void ui_kit_px_text(float x, float cy, const char* text, float size, kit_color color,
                    kit_align align);
float ui_kit_px_text_width(const char* text, float size);
void ui_kit_px_tab(float x, float y, float w, float h, const char* label, int active,
                   int hover);
void ui_kit_px_button(float x, float y, float w, float h, const char* label, int primary,
                      kit_btn_state state);
void ui_kit_px_close_button(float x, float y, float size, int hover);

#endif // UI_KIT_H
