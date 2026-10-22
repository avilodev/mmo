/**
 * @file
 * Draw the friends panel.
 *
 * The drawing half only. Everything that decides what is true lives in
 * friends_panel.c, which is linked into tests that have no renderer; the two
 * share their geometry through friends_panel_layout.h so a click lands on what
 * it looks like it hit.
 */

#include "ui/friends_panel.h"
#include "ui/ui_kit.h"
#include "friends_panel_layout.h"
#include "render/renderer.h"

#include <stdio.h>
#include <string.h>

#define TEXT_SIZE 16.0f
#define SMALL_SIZE 13.0f
#define TITLE_SIZE 20.0f

/** Draw the dot that says a friend is on or off. */
static void draw_presence_dot(float x, float cy, int online) {
	ui_kit_px_rect(x, cy - 4.0f, 8.0f, 8.0f, online ? 0.25f : 0.40f, online ? 0.80f : 0.40f,
	               online ? 0.35f : 0.40f, 1.0f);
}

/** Draw the friends tab's rows. */
static void render_friend_rows(const FriendsState* fs, float px, float py) {
	float row_y = fp_rows_top(py);

	if(fs->friend_count == 0) {
		ui_kit_px_text(px + FP_PAD, row_y + FP_ROW_H * 0.5f,
		               fs->have_list ? "No friends yet." : "Loading...",
		               TEXT_SIZE, KIT_DIM, KIT_LEFT);
		return;
	}

	for(int i = fs->scroll; i < fs->friend_count && i < fs->scroll + FP_VISIBLE; i++) {
		const FriendRow* f = &fs->friends[i];
		int hot = (i == fs->hovered);
		float cy = row_y + FP_ROW_H * 0.5f;
		char line[96];

		if(hot)
			ui_kit_px_rect(px + 6.0f, row_y, FP_PW - 12.0f, FP_ROW_H - 2.0f, 1.0f, 1.0f, 1.0f,
			               0.07f);

		draw_presence_dot(px + FP_PAD, cy, f->online);

		if(f->online)
			snprintf(line, sizeof(line), "%s  -  World %u", f->name, f->world_id);
		else
			snprintf(line, sizeof(line), "%s  -  Offline", f->name);
		ui_kit_px_text(px + FP_PAD + 16.0f, cy, line, TEXT_SIZE,
		               f->online ? KIT_WHITE : KIT_DIM, KIT_LEFT);

		if(hot)
			ui_kit_px_button(px + FP_PW - FP_PAD - FP_BTN_W, row_y + FP_BTN_Y, FP_BTN_W,
			                 FP_BTN_H, "Remove", 0, KIT_BTN_HOVER);

		row_y += FP_ROW_H;
	}
}

/** Draw the requests tab's rows. */
static void render_request_rows(const FriendsState* fs, float px, float py) {
	float row_y = fp_rows_top(py);

	if(fs->request_count == 0) {
		ui_kit_px_text(px + FP_PAD, row_y + FP_ROW_H * 0.5f, "No requests waiting.", TEXT_SIZE,
		               KIT_DIM, KIT_LEFT);
		return;
	}

	for(int i = fs->scroll; i < fs->request_count && i < fs->scroll + FP_VISIBLE; i++) {
		const FriendRequestRow* r = &fs->requests[i];
		int hot = (i == fs->hovered);
		kit_btn_state state = hot ? KIT_BTN_HOVER : KIT_BTN_NORMAL;
		float decline_x = px + FP_PW - FP_PAD - FP_BTN_W;

		if(hot)
			ui_kit_px_rect(px + 6.0f, row_y, FP_PW - 12.0f, FP_ROW_H - 2.0f, 1.0f, 1.0f, 1.0f,
			               0.07f);

		ui_kit_px_text(px + FP_PAD, row_y + FP_ROW_H * 0.5f,
		               r->from_name[0] ? r->from_name : "(unknown)", TEXT_SIZE, KIT_WHITE,
		               KIT_LEFT);

		// Both buttons always drawn: these are the actions the tab exists for
		ui_kit_px_button(decline_x, row_y + FP_BTN_Y, FP_BTN_W, FP_BTN_H, "Decline", 0, state);
		ui_kit_px_button(decline_x - FP_BTN_W - 4.0f, row_y + FP_BTN_Y, FP_BTN_W, FP_BTN_H,
		                 "Accept", 1, state);

		row_y += FP_ROW_H;
	}
}

void friends_panel_render(const FriendsState* fs, int vw, int vh) {
	char requests_label[32];
	float tab_w = (FP_PW - 2.0f * FP_PAD) * 0.5f;
	float ph;
	float px;
	float py;

	if(!fs || !fs->is_open)
		return;

	ph = fp_panel_height();
	fp_panel_origin(fs, vw, vh, &px, &py);

	// The requests tab carries its count so a waiting request is not missed
	if(fs->request_count > 0)
		snprintf(requests_label, sizeof(requests_label), "Requests (%d)", fs->request_count);
	else
		snprintf(requests_label, sizeof(requests_label), "Requests");

	ui_kit_px_begin(vw, vh);
	ui_kit_px_rect(px, py, FP_PW, ph, 0.05f, 0.05f, 0.09f, 0.97f);
	ui_kit_px_panel(px, py, FP_PW, ph);
	ui_kit_px_text(px + FP_PAD, py + FP_TITLE_H * 0.5f, "Friends", TITLE_SIZE, KIT_GOLD,
	               KIT_LEFT);
	ui_kit_px_close_button(px + FP_PW - FP_CLOSE - 8.0f, py + 8.0f, FP_CLOSE, 0);
	ui_kit_px_tab(px + FP_PAD, py + FP_TITLE_H, tab_w, FP_TAB_H - 2.0f, "Friends",
	              fs->tab == FRIENDS_TAB_LIST, 0);
	ui_kit_px_tab(px + FP_PAD + tab_w, py + FP_TITLE_H, tab_w, FP_TAB_H - 2.0f, requests_label,
	              fs->tab == FRIENDS_TAB_REQUESTS, 0);

	if(fs->tab == FRIENDS_TAB_REQUESTS)
		render_request_rows(fs, px, py);
	else
		render_friend_rows(fs, px, py);

	ui_kit_px_end();
}
