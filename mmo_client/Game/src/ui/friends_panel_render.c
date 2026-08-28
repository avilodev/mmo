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
#include "friends_panel_layout.h"
#include "render/renderer.h"

#include <stdio.h>
#include <string.h>

/** Draw a one-pixel border around a rectangle. */
static void draw_border(float x, float y, float w, float h,
                        float r, float g, float b, float a) {
    renderer_draw_rect(x,            y,            w,    1.5f, r, g, b, a);
    renderer_draw_rect(x,            y + h - 1.5f, w,    1.5f, r, g, b, a);
    renderer_draw_rect(x,            y,            1.5f, h,    r, g, b, a);
    renderer_draw_rect(x + w - 1.5f, y,            1.5f, h,    r, g, b, a);
}

/** Draw one tab header, brighter when it is the active one. */
static void draw_tab(float x, float y, float w, const char* label, int active) {
    if (active) renderer_draw_rect(x, y, w, FP_TAB_H, 0.18f, 0.26f, 0.38f, 1.0f);
    renderer_draw_rect(x, y + FP_TAB_H - 2.0f, w, 2.0f,
                       0.35f, 0.50f, 0.70f, active ? 1.0f : 0.25f);
    renderer_draw_text_centered(x, y, w, FP_TAB_H, label);
}

/** Draw the button at the right of a row. */
static void draw_button(float x, float y, const char* label, int hot) {
    renderer_draw_rect(x, y, FP_BTN_W, FP_BTN_H,
                       hot ? 0.22f : 0.14f, hot ? 0.32f : 0.20f,
                       hot ? 0.46f : 0.30f, 1.0f);
    draw_border(x, y, FP_BTN_W, FP_BTN_H, 0.40f, 0.55f, 0.75f, 0.7f);
    renderer_draw_text_centered(x, y, FP_BTN_W, FP_BTN_H, label);
}

/** Draw the friends tab's rows. */
static void render_friend_rows(const FriendsState* fs, float px, float py) {
    float row_y = fp_rows_top(py);

    if (fs->friend_count == 0) {
        /* "Loading" and "none" are different statements, and saying the wrong
         * one is worse than saying nothing: a player told they have no friends
         * closes the panel. */
        renderer_draw_text(px + FP_PAD, row_y + 20.0f,
                           fs->have_list ? "No friends yet. Use /friend <name> to add one."
                                         : "Loading...");
        return;
    }

    for (int i = fs->scroll;
         i < fs->friend_count && i < fs->scroll + FP_VISIBLE;
         i++) {
        const FriendRow* f = &fs->friends[i];
        int hot = (i == fs->hovered);

        if (hot)
            renderer_draw_rect(px + 6.0f, row_y, FP_PW - 12.0f, FP_ROW_H - 3.0f,
                               0.14f, 0.16f, 0.22f, 0.8f);

        /* A dot rather than a word, so the row's text is the name. */
        renderer_draw_rect(px + FP_PAD, row_y + 12.0f, 10.0f, 10.0f,
                           f->online ? 0.25f : 0.40f,
                           f->online ? 0.80f : 0.40f,
                           f->online ? 0.35f : 0.40f, 1.0f);

        char line[96];
        if (f->online)
            snprintf(line, sizeof(line), "%s  -  World %u", f->name, f->world_id);
        else
            snprintf(line, sizeof(line), "%s  -  Offline", f->name);
        renderer_draw_text(px + FP_PAD + 20.0f, row_y + FP_ROW_H - 12.0f, line);

        if (hot)
            draw_button(px + FP_PW - FP_PAD - FP_BTN_W, row_y + 5.0f, "Remove", 1);

        row_y += FP_ROW_H;
    }
}

/** Draw the requests tab's rows. */
static void render_request_rows(const FriendsState* fs, float px, float py) {
    float row_y = fp_rows_top(py);

    if (fs->request_count == 0) {
        renderer_draw_text(px + FP_PAD, row_y + 20.0f, "No requests waiting.");
        return;
    }

    for (int i = fs->scroll;
         i < fs->request_count && i < fs->scroll + FP_VISIBLE;
         i++) {
        const FriendRequestRow* r = &fs->requests[i];
        int hot = (i == fs->hovered);

        if (hot)
            renderer_draw_rect(px + 6.0f, row_y, FP_PW - 12.0f, FP_ROW_H - 3.0f,
                               0.14f, 0.16f, 0.22f, 0.8f);

        renderer_draw_text(px + FP_PAD, row_y + FP_ROW_H - 12.0f,
                           r->from_name[0] ? r->from_name : "(unknown)");

        /* Both buttons always drawn, not only on hover: these are the actions
         * the tab exists for, and a player should not have to discover them. */
        draw_button(px + FP_PW - FP_PAD - FP_BTN_W, row_y + 5.0f, "Decline", hot);
        draw_button(px + FP_PW - FP_PAD - FP_BTN_W * 2.0f - 6.0f, row_y + 5.0f,
                    "Accept", hot);

        row_y += FP_ROW_H;
    }
}

void friends_panel_render(const FriendsState* fs, int vw, int vh) {
    if (!fs || !fs->is_open) return;

    float ph = fp_panel_height();
    float px, py;
    fp_panel_origin(vw, vh, &px, &py);

    renderer_draw_rect(px, py, FP_PW, ph, 0.08f, 0.08f, 0.14f, 0.97f);
    draw_border(px, py, FP_PW, ph, 0.35f, 0.50f, 0.70f, 0.8f);

    renderer_draw_rect(px, py, FP_PW, FP_TITLE_H, 0.12f, 0.18f, 0.28f, 1.0f);
    renderer_draw_rect(px, py + FP_TITLE_H, FP_PW, 1.5f, 0.35f, 0.50f, 0.70f, 0.8f);
    renderer_draw_text(px + FP_PW * 0.5f - 36.0f, py + 28.0f, "FRIENDS");
    renderer_draw_text(px + FP_PW - 52.0f, py + 28.0f, "[U]");

    /* Tabs. The requests tab carries its count, because an unanswered request
     * the player never notices is the one failure this panel can have. */
    char requests_label[32];
    if (fs->request_count > 0)
        snprintf(requests_label, sizeof(requests_label), "REQUESTS (%d)", fs->request_count);
    else
        snprintf(requests_label, sizeof(requests_label), "%s", "REQUESTS");

    float tab_w = FP_PW * 0.5f;
    draw_tab(px,         py + FP_TITLE_H, tab_w, "FRIENDS",      fs->tab == FRIENDS_TAB_LIST);
    draw_tab(px + tab_w, py + FP_TITLE_H, tab_w, requests_label, fs->tab == FRIENDS_TAB_REQUESTS);

    if (fs->tab == FRIENDS_TAB_REQUESTS) render_request_rows(fs, px, py);
    else                                 render_friend_rows(fs, px, py);

    /* Footer: the last result, or the hint that says how to add somebody. */
    const char* footer = (fs->notice_timer > 0.0f && fs->notice[0])
                       ? fs->notice
                       : "/friend <name>   /unfriend <name>   /block <name>";
    renderer_draw_text(px + FP_PAD, py + ph - 12.0f, footer);

    /* Only shown when there is something off screen, so the hint appears
     * exactly when it is true. */
    if (fp_row_count(fs) > FP_VISIBLE) {
        char pages[48];
        snprintf(pages, sizeof(pages), "%d-%d of %d  [PgUp/PgDn]",
                 fs->scroll + 1,
                 (fs->scroll + FP_VISIBLE < fp_row_count(fs)) ? fs->scroll + FP_VISIBLE
                                                           : fp_row_count(fs),
                 fp_row_count(fs));
        renderer_draw_text(px + FP_PW - FP_PAD - renderer_text_width(pages),
                           py + ph - 12.0f, pages);
    }
}

