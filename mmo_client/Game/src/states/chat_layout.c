// Where the chat box is, and how its lines wrap and scroll inside it.

#include "states/chat_layout.h"
#include "states/chat_channels.h"

#include <string.h>

chat_box chat_box_for(float viewport_height) {
	chat_box box;

	box.x = CHAT_X;
	box.w = CHAT_W;
	box.h = (float)CHAT_LINES * CHAT_LINE_H + CHAT_PAD * 2.0f;
	box.input_h = CHAT_INBOX_H;
	box.y = viewport_height - box.h - box.input_h - CHAT_BOTTOM_MARGIN;
	box.input_y = box.y + box.h;

	return box;
}

chat_box chat_box_at(const window_pos* pos, float viewport_width, float viewport_height) {
	chat_box box = chat_box_for(viewport_height);
	float total = box.h + box.input_h;
	float x = box.x;
	float y = box.y;

	if(pos->placed)
		window_pos_origin(pos, viewport_width, viewport_height, box.w, total, &x, &y);

	box.x = x;
	box.y = y;
	box.input_y = box.y + box.h;

	return box;
}

void chat_window_rect(const chat_box* box, float* x, float* y, float* w, float* h) {
	*x = box->x;
	*y = box->y;
	*w = box->w;
	*h = box->h + box->input_h;
}

// Bytes of a too-wide word that fit on one row; at least one.
static size_t fit_prefix(const char* word, size_t length, float max_width,
		chat_measure_fn measure) {
	size_t take = 1;

	while(take < length && measure(word, take + 1) <= max_width)
		take++;

	return take;
}

int chat_wrap(const char* text, float max_width, chat_measure_fn measure,
		chat_row* rows, int max_rows) {
	size_t length = strlen(text);
	size_t pos = 0;
	int count = 0;

	while(count < max_rows) {
		size_t start;
		size_t end;
		float width = 0.0f;

		while(pos < length && text[pos] == ' ')
			pos++;
		if(pos >= length)
			break;

		start = pos;
		end = pos;
		while(pos < length) {
			size_t word_end = pos;
			float word_w;
			float gap_w = 0.0f;

			while(word_end < length && text[word_end] != ' ')
				word_end++;
			word_w = measure(text + pos, word_end - pos);
			if(end > start)
				gap_w = measure(text + end, pos - end);

			// The next word starts the next row
			if(end > start && width + gap_w + word_w > max_width)
				break;

			// A word wider than a whole row is cut where it runs out
			if(end == start && word_w > max_width) {
				pos += fit_prefix(text + pos, word_end - pos, max_width, measure);
				end = pos;
				break;
			}

			width += gap_w + word_w;
			end = word_end;
			pos = word_end;
			while(pos < length && text[pos] == ' ')
				pos++;
		}

		rows[count].start = (int)start;
		rows[count].length = (int)(end - start);
		count++;
	}

	return count;
}

// Which message is newest on screen, allowing for the scroll.
static int newest_shown(const ChatState* chat) {
	int newest = chat->line_count - 1 - chat->scroll;

	return newest < 0 ? 0 : newest;
}

int chat_layout_visible(const ChatState* chat, chat_measure_fn measure,
		chat_visible* out) {
	int count = 0;
	int used = 0;

	// Newest first, until the next message would not fit
	for(int i = newest_shown(chat); i >= 0 && i < chat->line_count; i--) {
		chat_visible* v = &out[count];

		chat_line_format(&chat->lines[i], v->text, sizeof(v->text));
		v->row_count = chat_wrap(v->text, CHAT_TEXT_W, measure, v->rows, CHAT_LINES);
		if(v->row_count == 0)
			continue;
		if(used + v->row_count > CHAT_LINES)
			break;

		v->line = i;
		used += v->row_count;
		count++;
		if(count == CHAT_LINES)
			break;
	}

	// Oldest first, from the top of the panel
	for(int a = 0, b = count - 1; a < b; a++, b--) {
		chat_visible swap = out[a];

		out[a] = out[b];
		out[b] = swap;
	}
	used = 0;
	for(int i = 0; i < count; i++) {
		out[i].first_row = used;
		used += out[i].row_count;
	}

	return count;
}

int chat_hit_line(const chat_visible* visible, int count, const chat_box* box,
		float y) {
	int row;

	if(y < box->y + CHAT_PAD)
		return -1;

	row = (int)((y - box->y - CHAT_PAD) / CHAT_LINE_H);
	for(int i = 0; i < count; i++)
		if(row >= visible[i].first_row &&
		   row < visible[i].first_row + visible[i].row_count)
			return visible[i].line;

	return -1;
}

const char* chat_input_tail(const char* text, float max_width,
		chat_measure_fn measure) {
	size_t length = strlen(text);
	size_t start = length;

	while(start > 0 && measure(text + start - 1, length - start + 1) <= max_width)
		start--;

	return text + start;
}

void chat_scroll_by(ChatState* chat, int delta) {
	chat->scroll += delta;

	if(chat->scroll > chat->line_count - 1)
		chat->scroll = chat->line_count - 1;
	if(chat->scroll < 0)
		chat->scroll = 0;
}
