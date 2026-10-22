#ifndef CHAT_LAYOUT_H
#define CHAT_LAYOUT_H

// Where the chat box is, and how its lines wrap and scroll inside it.

// The draw code and the click code both ask here, so they cannot disagree.

#include "core/game_types.h"
#include "ui/window_pos.h"

#include <stddef.h>

#define CHAT_X 14.0f
#define CHAT_W 420.0f
#define CHAT_LINE_H 20.0f
#define CHAT_LINES 8 // rows of text the box shows
#define CHAT_PAD 8.0f
#define CHAT_INBOX_H 30.0f
#define CHAT_BOTTOM_MARGIN 20.0f
#define CHAT_TEXT_W (CHAT_W - CHAT_PAD * 2.0f)
#define CHAT_FORMAT_MAX 320 // a formatted line, with its tag and sender
#define CHAT_EDGE_BAND 12.0f // how thick the outline is to grab, in pixels

typedef struct {
	float x, y, w, h; // the message panel
	float input_y; // top of the input box under it
	float input_h;
} chat_box;

// The box for a window this tall; it sits at the bottom left.
chat_box chat_box_for(float viewport_height);

// The box where the player left it, else at the bottom left
chat_box chat_box_at(const window_pos* pos, float viewport_width, float viewport_height);

// The whole of the box, messages and input line, which the outline surrounds
void chat_window_rect(const chat_box* box, float* x, float* y, float* w, float* h);

// Width of the first `length` bytes of the text, in pixels.
typedef float (*chat_measure_fn)(const char* text, size_t length);

// One row of a wrapped line: a byte range of its text.
typedef struct {
	int start;
	int length;
} chat_row;

// Breaks text into rows no wider than max_width; returns how many it made.
int chat_wrap(const char* text, float max_width, chat_measure_fn measure,
		chat_row* rows, int max_rows);

// One message as it is drawn: its text, its rows, and which rows it sits on.
typedef struct {
	int line; // index into ChatState.lines
	int first_row; // 0 is the top row of the panel
	int row_count;
	char text[CHAT_FORMAT_MAX];
	chat_row rows[CHAT_LINES];
} chat_visible;

// Fills out (room for CHAT_LINES) oldest first with the messages that fit.
int chat_layout_visible(const ChatState* chat, chat_measure_fn measure,
		chat_visible* out);

// The ChatState line under a y position in the panel, or -1.
int chat_hit_line(const chat_visible* visible, int count, const chat_box* box,
		float y);

// The end of the text that fits in max_width, so a long input shows its tail.
const char* chat_input_tail(const char* text, float max_width,
		chat_measure_fn measure);

// Scrolls toward older messages for a positive delta, and stays in range.
void chat_scroll_by(ChatState* chat, int delta);

#endif /* CHAT_LAYOUT_H */
