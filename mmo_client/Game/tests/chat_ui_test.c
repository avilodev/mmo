// Check the chat box's channels, wrapping, scrolling, input editing and log.
// No GLFW and no renderer: text is measured at a fixed width per character.

#include "states/chat_channels.h"
#include "states/chat_input.h"
#include "states/chat_layout.h"
#include "states/chat_log.h"
#include "protocol.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Every character is 10 pixels wide, so 40 fit in a row of 400.
static float measure(const char* text, size_t length) {
	(void)text;

	return (float)length * 10.0f;
}

static ChatState* new_chat(void) {
	ChatState* chat = calloc(1, sizeof(*chat));

	assert(chat);
	chat->history_pos = -1;

	return chat;
}

static void test_channels(void) {
	ChatLine line;
	char text[CHAT_FORMAT_MAX];
	float r, g, b, r2, g2, b2;

	// every channel has its own tag, name and colour
	for(int id = 0; id <= CHAT_CHANNEL_SYSTEM; id++) {
		const chat_channel_info* info = chat_channel_lookup((uint8_t)id);

		assert(info->id == id);
		assert(info->tag[0] == '[' && info->name[0] != '\0');
		for(int other = id + 1; other <= CHAT_CHANNEL_SYSTEM; other++) {
			const chat_channel_info* o = chat_channel_lookup((uint8_t)other);

			assert(strcmp(info->tag, o->tag) != 0);
			assert(strcmp(info->name, o->name) != 0);
		}
	}
	assert(chat_channel_lookup(200)->id == CHAT_CHANNEL_LOCAL);

	// Tab goes round the four speakable channels, never to System
	assert(chat_channel_next(CHAT_CHANNEL_LOCAL, 1) == CHAT_CHANNEL_GLOBAL);
	assert(chat_channel_next(CHAT_CHANNEL_GLOBAL, 1) == CHAT_CHANNEL_WHISPER);
	assert(chat_channel_next(CHAT_CHANNEL_WHISPER, 1) == CHAT_CHANNEL_PARTY);
	assert(chat_channel_next(CHAT_CHANNEL_PARTY, 1) == CHAT_CHANNEL_LOCAL);
	// ...and skips Whisper when there is nobody to whisper
	assert(chat_channel_next(CHAT_CHANNEL_GLOBAL, 0) == CHAT_CHANNEL_PARTY);

	memset(&line, 0, sizeof(line));
	snprintf(line.sender, sizeof(line.sender), "Ann");
	snprintf(line.text, sizeof(line.text), "hi");
	line.channel = CHAT_CHANNEL_GLOBAL;
	chat_line_format(&line, text, sizeof(text));
	assert(strcmp(text, "[G] Ann: hi") == 0);

	// the sender's own whisper names where it went
	line.channel = CHAT_CHANNEL_WHISPER;
	line.flags = CHAT_FLAG_ECHO;
	chat_line_format(&line, text, sizeof(text));
	assert(strcmp(text, "[W] To Ann: hi") == 0);
	assert(!chat_line_replyable(&line));
	line.flags = 0;
	assert(chat_line_replyable(&line));

	// client-made lines are their text alone, and are not replyable
	line.style = CHAT_STYLE_ERROR;
	chat_line_format(&line, text, sizeof(text));
	assert(strcmp(text, "hi") == 0);
	assert(!chat_line_replyable(&line));

	// channels read differently on screen
	line.style = CHAT_STYLE_NORMAL;
	line.channel = CHAT_CHANNEL_LOCAL;
	chat_line_color(&line, &r, &g, &b);
	line.channel = CHAT_CHANNEL_PARTY;
	chat_line_color(&line, &r2, &g2, &b2);
	assert(r != r2 || g != g2 || b != b2);
}

static void test_badge(void) {
	char badge[48];

	chat_badge_format(CHAT_CHANNEL_LOCAL, "", badge, sizeof(badge));
	assert(strcmp(badge, "[Local] ") == 0);
	chat_badge_format(CHAT_CHANNEL_WHISPER, "", badge, sizeof(badge));
	assert(strcmp(badge, "[Whisper] ") == 0);
	chat_badge_format(CHAT_CHANNEL_WHISPER, "Ann", badge, sizeof(badge));
	assert(strcmp(badge, "[-> Ann] ") == 0);

	// the badge is a different width on each channel, which is why the typed
	// text cannot start at a fixed offset
	chat_badge_format(CHAT_CHANNEL_PARTY, "", badge, sizeof(badge));
	assert(strlen(badge) != strlen("[Whisper] "));
}

static void test_box(void) {
	chat_box box = chat_box_for(1080.0f);

	assert(box.x == CHAT_X && box.w == CHAT_W);
	assert(box.input_y == box.y + box.h);
	assert(box.input_y + box.input_h + CHAT_BOTTOM_MARGIN == 1080.0f);
}

static void test_box_at(void) {
	window_pos pos;
	chat_box home = chat_box_for(1080.0f);
	chat_box box;
	float x, y, w, h;

	// Not moved: the same corner as always
	window_pos_reset(&pos);
	box = chat_box_at(&pos, 1920.0f, 1080.0f);
	assert(box.x == home.x && box.y == home.y && box.input_y == home.input_y);

	// The window the outline surrounds is the messages and the input line
	chat_window_rect(&box, &x, &y, &w, &h);
	assert(x == box.x && y == box.y && w == box.w);
	assert(y + h == box.input_y + box.input_h);

	// Moved: the box follows, and keeps its shape
	pos.placed = 1;
	pos.x = 600.0f;
	pos.y = 300.0f;
	box = chat_box_at(&pos, 1920.0f, 1080.0f);
	assert(box.x == 600.0f && box.y == 300.0f);
	assert(box.h == home.h && box.input_y == box.y + box.h && box.input_h == home.input_h);

	// A box moved off the screen is pulled back on
	pos.x = 5000.0f;
	pos.y = 5000.0f;
	box = chat_box_at(&pos, 1920.0f, 1080.0f);
	chat_window_rect(&box, &x, &y, &w, &h);
	assert(x + w <= 1920.0f && y + h <= 1080.0f);
}

// Checks every row is within the width and that the rows cover the words.
static void expect_rows(const char* text, float width, int want_rows) {
	chat_row rows[CHAT_LINES];
	int count = chat_wrap(text, width, measure, rows, CHAT_LINES);

	assert(count == want_rows);
	for(int i = 0; i < count; i++) {
		assert(rows[i].length > 0);
		assert((float)rows[i].length * 10.0f <= width);
		assert(text[rows[i].start] != ' ');
		assert(text[rows[i].start + rows[i].length - 1] != ' ');
	}
}

static void test_wrap(void) {
	chat_row rows[CHAT_LINES];
	int count;

	expect_rows("short", 400.0f, 1);
	expect_rows("", 400.0f, 0);
	expect_rows("    ", 400.0f, 0);

	// 3 words of 5, width for 11 characters: two fit, the third wraps
	expect_rows("aaaaa bbbbb ccccc", 110.0f, 2);
	count = chat_wrap("aaaaa bbbbb ccccc", 110.0f, measure, rows, CHAT_LINES);
	assert(rows[0].start == 0 && rows[0].length == 11);
	assert(rows[1].start == 12 && rows[1].length == 5);

	// a word wider than the row is cut where it runs out
	expect_rows("abcdefghijklmnopqrstuvwxyz", 100.0f, 3);
	count = chat_wrap("abcdefghijklmnopqrstuvwxyz", 100.0f, measure, rows, CHAT_LINES);
	assert(rows[0].length == 10 && rows[1].length == 10 && rows[2].length == 6);

	// a long word after a short one starts its own row, then is cut
	expect_rows("hi abcdefghijklmnopqrst", 100.0f, 3);

	// never more rows than there is room for
	count = chat_wrap("a b c d e f g h i j k l m n o p", 10.0f, measure, rows, 4);
	assert(count == 4);

	// a width too small for even one character still makes progress
	count = chat_wrap("abc", 1.0f, measure, rows, CHAT_LINES);
	assert(count == 3);
}

static void test_input_tail(void) {
	// 10 characters fit; the tail keeps the end, where the cursor is
	assert(strcmp(chat_input_tail("abc", 100.0f, measure), "abc") == 0);
	assert(strcmp(chat_input_tail("abcdefghijklmnop", 100.0f, measure),
			"ghijklmnop") == 0);
	assert(strcmp(chat_input_tail("", 100.0f, measure), "") == 0);
	assert(strcmp(chat_input_tail("abc", 0.0f, measure), "") == 0);
}

static void add_normal(ChatState* chat, const char* sender, const char* text) {
	chat_log_receive(chat, CHAT_CHANNEL_LOCAL, 0, sender, text);
}

static void test_layout(void) {
	ChatState* chat = new_chat();
	chat_visible visible[CHAT_LINES];
	chat_box box = chat_box_for(1080.0f);
	char text[64];
	int count;

	assert(chat_layout_visible(chat, measure, visible) == 0);

	// each short line is one row, oldest at the top
	for(int i = 0; i < 3; i++) {
		snprintf(text, sizeof(text), "line %d", i);
		add_normal(chat, "Ann", text);
	}
	count = chat_layout_visible(chat, measure, visible);
	assert(count == 3);
	assert(visible[0].line == 0 && visible[0].first_row == 0);
	assert(visible[2].line == 2 && visible[2].first_row == 2);

	// only the newest rows fit; a message is never split across the top edge
	for(int i = 0; i < 20; i++)
		add_normal(chat, "Ann", "x");
	count = chat_layout_visible(chat, measure, visible);
	assert(count == CHAT_LINES);
	assert(visible[CHAT_LINES - 1].line == chat->line_count - 1);

	// a long message takes several rows and pushes the older ones off
	add_normal(chat, "Ann", "one two three four five six seven eight nine ten eleven "
			"twelve thirteen fourteen fifteen sixteen seventeen eighteen");
	count = chat_layout_visible(chat, measure, visible);
	assert(visible[count - 1].row_count > 1);
	assert(visible[count - 1].first_row + visible[count - 1].row_count <= CHAT_LINES);
	{
		int used = 0;

		for(int i = 0; i < count; i++)
			used += visible[i].row_count;
		assert(used <= CHAT_LINES);
	}

	// a click lands on the message whose row it is on
	{
		float y = box.y + CHAT_PAD + CHAT_LINE_H * 0.5f;

		assert(chat_hit_line(visible, count, &box, y) == visible[0].line);
		assert(chat_hit_line(visible, count, &box, box.y) == -1);
		assert(chat_hit_line(visible, count, &box, box.y - 50.0f) == -1);
	}

	free(chat);
}

static void test_scroll(void) {
	ChatState* chat = new_chat();
	chat_visible visible[CHAT_LINES];
	int count;

	for(int i = 0; i < 30; i++) {
		char text[16];

		snprintf(text, sizeof(text), "m%d", i);
		add_normal(chat, "Ann", text);
	}

	chat_scroll_by(chat, 5);
	assert(chat->scroll == 5);
	count = chat_layout_visible(chat, measure, visible);
	assert(visible[count - 1].line == 29 - 5);

	chat_scroll_by(chat, -100);
	assert(chat->scroll == 0);
	chat_scroll_by(chat, 1000);
	assert(chat->scroll == chat->line_count - 1);
	chat_scroll_by(chat, -1000);

	// new messages leave a scrolled-back reader where they were
	chat_scroll_by(chat, 4);
	add_normal(chat, "Ann", "new");
	assert(chat->scroll == 5);
	count = chat_layout_visible(chat, measure, visible);
	assert(strcmp(chat->lines[visible[count - 1].line].text, "m25") == 0);

	// but a reader at the bottom stays at the bottom
	chat_scroll_by(chat, -100);
	add_normal(chat, "Ann", "newer");
	assert(chat->scroll == 0);

	// and sending (closing the input) returns to the newest
	chat_scroll_by(chat, 6);
	chat_close(chat);
	assert(chat->scroll == 0);

	free(chat);
}

static void test_log(void) {
	ChatState* chat = new_chat();

	// the oldest line goes when the log is full
	for(int i = 0; i < MAX_CHAT_LINES + 5; i++) {
		char text[16];

		snprintf(text, sizeof(text), "m%d", i);
		add_normal(chat, "Ann", text);
	}
	assert(chat->line_count == MAX_CHAT_LINES);
	assert(strcmp(chat->lines[0].text, "m5") == 0);

	// a client line has every field set, whatever the slot held before
	chat->lines[MAX_CHAT_LINES - 1].flags = CHAT_FLAG_ECHO;
	chat_log_add(chat, CHAT_CHANNEL_SYSTEM, CHAT_STYLE_ERROR, NULL, NULL);
	assert(chat->lines[MAX_CHAT_LINES - 1].flags == 0);
	assert(chat->lines[MAX_CHAT_LINES - 1].sender[0] == '\0');

	// the system channel becomes a notice, not a "[S] : text" line
	chat_log_receive(chat, CHAT_CHANNEL_SYSTEM, 0, "", "No player named X is online.");
	assert(chat->lines[chat->line_count - 1].style == CHAT_STYLE_SYSTEM);

	// whispers set who /r and Tab answer: the sender, or the recipient of an echo
	chat_log_receive(chat, CHAT_CHANNEL_WHISPER, 0, "Bob", "psst");
	assert(strcmp(chat->whisper_reply_target, "Bob") == 0);
	chat_log_receive(chat, CHAT_CHANNEL_WHISPER, CHAT_FLAG_ECHO, "Cy", "psst back");
	assert(strcmp(chat->whisper_reply_target, "Cy") == 0);
	assert(chat->lines[chat->line_count - 1].flags == CHAT_FLAG_ECHO);

	// other channels leave it alone
	chat_log_receive(chat, CHAT_CHANNEL_GLOBAL, 0, "Dee", "hello");
	assert(strcmp(chat->whisper_reply_target, "Cy") == 0);

	free(chat);
}

static void type_text(ChatState* chat, const char* text) {
	for(const char* c = text; *c; c++)
		chat_type(chat, (unsigned char)*c);
}

static void test_typing(void) {
	ChatState* chat = new_chat();

	chat_open(chat);
	assert(chat->is_typing);
	type_text(chat, "hello");
	assert(strcmp(chat->input_buf, "hello") == 0 && chat->input_len == 5);

	// only printable ASCII goes in
	chat_type(chat, 7);
	chat_type(chat, 127);
	chat_type(chat, 0x20AC);
	assert(chat->input_len == 5);

	// the buffer never overflows
	for(int i = 0; i < MAX_CHAT_INPUT_LEN * 2; i++)
		chat_type(chat, 'x');
	assert(chat->input_len == MAX_CHAT_INPUT_LEN - 1);
	assert(chat->input_buf[MAX_CHAT_INPUT_LEN - 1] == '\0');

	chat_close(chat);
	assert(!chat->is_typing && chat->input_len == 0 && chat->input_buf[0] == '\0');
	assert(!chat_backspace(chat));

	free(chat);
}

static void test_backspace(void) {
	ChatState* chat = new_chat();

	type_text(chat, "abcdef");

	// not held: nothing happens
	chat_backspace_update(chat, 0, 0, 1.0f);
	assert(chat->input_len == 6);

	// the press deletes one
	chat_backspace_update(chat, 1, 1, 0.016f);
	assert(chat->input_len == 5);

	// held: nothing until the first delay, then one per repeat
	chat_backspace_update(chat, 0, 1, 0.2f);
	assert(chat->input_len == 5);
	chat_backspace_update(chat, 0, 1, 0.25f);
	assert(chat->input_len == 4);
	chat_backspace_update(chat, 0, 1, 0.03f);
	assert(chat->input_len == 4);
	chat_backspace_update(chat, 0, 1, 0.03f);
	assert(chat->input_len == 3);

	free(chat);
}

static void test_cycle(void) {
	ChatState* chat = new_chat();

	// nobody to whisper: Local, Global, Party, Local
	chat_cycle_channel(chat);
	assert(chat->active_channel == CHAT_CHANNEL_GLOBAL);
	chat_cycle_channel(chat);
	assert(chat->active_channel == CHAT_CHANNEL_PARTY);
	chat_cycle_channel(chat);
	assert(chat->active_channel == CHAT_CHANNEL_LOCAL);

	// someone to whisper: Whisper is in the round
	snprintf(chat->whisper_reply_target, sizeof(chat->whisper_reply_target), "Ann");
	chat_cycle_channel(chat);
	chat_cycle_channel(chat);
	assert(chat->active_channel == CHAT_CHANNEL_WHISPER);

	free(chat);
}

static void test_submit(void) {
	ChatState* chat = new_chat();
	chat_outgoing out;
	int before;

	// blank lines are not sent
	assert(!chat_input_submit(chat, &out));
	type_text(chat, "   ");
	assert(!chat_input_submit(chat, &out));
	chat_close(chat);

	// an ordinary line goes to the active channel, trimmed
	chat->active_channel = CHAT_CHANNEL_GLOBAL;
	type_text(chat, "  hi there  ");
	assert(chat_input_submit(chat, &out));
	assert(out.channel == CHAT_CHANNEL_GLOBAL);
	assert(strcmp(out.text, "hi there") == 0 && out.target[0] == '\0');
	chat_close(chat);

	// a whisper with nobody to whisper is refused, and says so
	chat->active_channel = CHAT_CHANNEL_WHISPER;
	type_text(chat, "psst");
	before = chat->line_count;
	assert(!chat_input_submit(chat, &out));
	assert(chat->line_count == before + 1);
	assert(chat->lines[before].style == CHAT_STYLE_ERROR);
	chat_close(chat);

	// with someone, the target travels apart from the text
	snprintf(chat->whisper_reply_target, sizeof(chat->whisper_reply_target), "Ann");
	type_text(chat, "psst");
	assert(chat_input_submit(chat, &out));
	assert(out.channel == CHAT_CHANNEL_WHISPER);
	assert(strcmp(out.target, "Ann") == 0 && strcmp(out.text, "psst") == 0);

	// a corrupt channel falls back to Local rather than being sent as-is
	chat->active_channel = 200;
	assert(chat_input_submit(chat, &out) && out.channel == CHAT_CHANNEL_LOCAL);

	free(chat);
}

static void test_history(void) {
	ChatState* chat = new_chat();

	chat_history_add(chat, "");
	chat_history_add(chat, "first");
	chat_history_add(chat, "first");
	chat_history_add(chat, "second");
	assert(chat->history_count == 2);

	// up walks back, and stops at the oldest
	chat_open(chat);
	type_text(chat, "draft");
	chat_history_step(chat, 1);
	assert(strcmp(chat->input_buf, "second") == 0);
	chat_history_step(chat, 1);
	assert(strcmp(chat->input_buf, "first") == 0);
	chat_history_step(chat, 1);
	assert(strcmp(chat->input_buf, "first") == 0);

	// down walks forward, and ends on what was being typed
	chat_history_step(chat, -1);
	assert(strcmp(chat->input_buf, "second") == 0);
	chat_history_step(chat, -1);
	assert(strcmp(chat->input_buf, "draft") == 0 && chat->input_len == 5);
	chat_history_step(chat, -1);
	assert(strcmp(chat->input_buf, "draft") == 0);

	// typing after a recall starts a fresh line
	chat_history_step(chat, 1);
	type_text(chat, "!");
	assert(chat->history_pos == -1 && strcmp(chat->input_buf, "second!") == 0);

	// the oldest is dropped when it is full
	for(int i = 0; i < CHAT_HISTORY_LINES + 5; i++) {
		char line[16];

		snprintf(line, sizeof(line), "line%d", i);
		chat_history_add(chat, line);
	}
	assert(chat->history_count == CHAT_HISTORY_LINES);
	assert(strcmp(chat->history[0], "line5") == 0);

	// with nothing kept, stepping does nothing
	free(chat);
	chat = new_chat();
	chat_history_step(chat, 1);
	assert(chat->history_pos == -1);

	free(chat);
}

static void test_begin_whisper(void) {
	ChatState* chat = new_chat();

	chat_begin_whisper(chat, "Ann");
	assert(chat->is_typing);
	assert(strcmp(chat->input_buf, "/w Ann ") == 0);
	assert(chat->input_len == 7);

	free(chat);
}

int main(void) {
	test_channels();
	test_badge();
	test_box();
	test_box_at();
	test_wrap();
	test_input_tail();
	test_layout();
	test_scroll();
	test_log();
	test_typing();
	test_backspace();
	test_cycle();
	test_submit();
	test_history();
	test_begin_whisper();
	printf("chat_ui_test: OK\n");

	return 0;
}
