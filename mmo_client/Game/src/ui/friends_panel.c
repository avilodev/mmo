/**
 * @file
 * Hold the friends panel's state and turn clicks in it into queued actions.
 *
 * Two tabs over one frame: the friends list, and the requests waiting to be
 * answered. Nothing here talks to the network -- a click sets one pending
 * action and the input layer sends it, the same split the quest log uses for
 * abandoning a quest -- and nothing here draws, which is what lets this half be
 * linked into a test with no renderer. The drawing is friends_panel_render.c.
 */

#include "ui/friends_panel.h"
#include "friends_panel_layout.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

/** Clamp the scroll offset to what the active tab can actually show. */
static void clamp_scroll(FriendsState* fs) {
    int max_scroll = fp_row_count(fs) - FP_VISIBLE;
    if (max_scroll < 0) max_scroll = 0;
    if (fs->scroll > max_scroll) fs->scroll = max_scroll;
    if (fs->scroll < 0) fs->scroll = 0;
}

/* --- Lifecycle ----------------------------------------------------------- */

void friends_panel_init(FriendsState* fs) {
    if (!fs) return;
    memset(fs, 0, sizeof(*fs));
    fs->hovered = -1;
}

void friends_panel_toggle(FriendsState* fs) {
    if (!fs) return;
    fs->is_open = !fs->is_open;
    if (fs->is_open) {
        fs->scroll = 0;
        fs->hovered = -1;
        /* Asked for on every open rather than cached for the session: the list
         * is served from the server's own cache and costs it two Redis reads,
         * which is cheaper than showing an hour-old roster. */
        fs->pending_kind = FRIEND_PENDING_REFRESH;
        fs->pending_name[0] = '\0';
    }
}

/* --- Server updates ------------------------------------------------------ */

/** Copy a wire name into a terminated buffer.
 *
 * Nothing off the wire is terminated, and the next thing that happens to these
 * is a printf. Every name that enters this file goes through here.
 */
static void copy_name(char* out, size_t out_size, const char* raw, size_t raw_size) {
    size_t n = raw_size < out_size - 1 ? raw_size : out_size - 1;
    memcpy(out, raw, n);
    out[n] = '\0';
}

void friends_panel_set_list(FriendsState* fs, const FriendListResponsePacket* pkt,
                            int packet_bytes) {
    if (!fs || !pkt) return;

    /* The server trims the packet to the entries it filled, so the claimed
     * count is checked against the bytes that actually arrived before any of
     * them is read. */
    int count = pkt->count;
    if (count > FRIENDS_MAX) count = FRIENDS_MAX;

    size_t base = offsetof(FriendListResponsePacket, friends);
    while (count > 0 &&
           (size_t)packet_bytes < base + (size_t)count * sizeof(FriendWireEntry)) {
        count--;
    }

    fs->friend_count = count;
    for (int i = 0; i < count; i++) {
        const FriendWireEntry* e = &pkt->friends[i];
        FriendRow* row = &fs->friends[i];

        row->account_id = ntohl(e->account_id);
        row->online     = e->online ? 1 : 0;
        row->world_id   = row->online ? ntohl(e->world_id) : 0u;
        copy_name(row->name, sizeof(row->name), e->name, sizeof(e->name));
    }

    fs->have_list = 1;
    clamp_scroll(fs);
}

void friends_panel_set_requests(FriendsState* fs, const FriendRequestsListPacket* pkt,
                                int packet_bytes) {
    if (!fs || !pkt) return;

    int count = pkt->count;
    if (count > FRIEND_REQUESTS_MAX) count = FRIEND_REQUESTS_MAX;

    size_t base = offsetof(FriendRequestsListPacket, requests);
    while (count > 0 &&
           (size_t)packet_bytes < base + (size_t)count * sizeof(FriendRequestWireEntry)) {
        count--;
    }

    fs->request_count = count;
    for (int i = 0; i < count; i++) {
        const FriendRequestWireEntry* e = &pkt->requests[i];
        FriendRequestRow* row = &fs->requests[i];

        row->from_account = ntohl(e->from_account);
        row->created_at   = (int64_t)mmo_ntohll((uint64_t)e->created_at);
        copy_name(row->from_name, sizeof(row->from_name), e->from_name, sizeof(e->from_name));
    }

    clamp_scroll(fs);
}

void friends_panel_apply_presence(FriendsState* fs,
                                  const FriendPresenceUpdatePacket* pkt) {
    if (!fs || !pkt) return;

    uint32_t account = ntohl(pkt->account_id);

    for (int i = 0; i < fs->friend_count; i++) {
        if (fs->friends[i].account_id != account) continue;

        fs->friends[i].online = pkt->online ? 1 : 0;
        if (pkt->online) {
            fs->friends[i].world_id = ntohl(pkt->world_id);
            copy_name(fs->friends[i].name, sizeof(fs->friends[i].name),
                      pkt->name, sizeof(pkt->name));
        } else {
            /* The name is kept: it is who the friend is, and a row that loses
             * its label the moment somebody logs out is a row nobody can act
             * on. Only the world goes, because there is no longer one. */
            fs->friends[i].world_id = 0;
        }
        return;
    }
    /* Not in the list: the list is behind, and the next refresh fixes it. */
}

/* --- Notices ------------------------------------------------------------- */

/** Phrase one result for the player. */
static const char* result_text(uint8_t action, uint8_t result) {
    switch (result) {
        case FRIEND_WIRE_OK:
            switch (action) {
                case FRIEND_ACTION_REQUEST: return "Friend request sent to %s.";
                case FRIEND_ACTION_ACCEPT:  return "You are now friends with %s.";
                case FRIEND_ACTION_DECLINE: return "Declined %s's request.";
                case FRIEND_ACTION_REMOVE:  return "Removed %s.";
                case FRIEND_ACTION_BLOCK:   return "Blocked %s.";
                case FRIEND_ACTION_UNBLOCK: return "Unblocked %s.";
                default:                    return "Done.";
            }
        case FRIEND_WIRE_ALREADY_FRIENDS: return "You are already friends with %s.";
        case FRIEND_WIRE_ALREADY_PENDING: return "You have already asked %s.";
        case FRIEND_WIRE_MUTUAL:          return "%s had already asked - you are now friends.";
        case FRIEND_WIRE_SELF:            return "You cannot befriend yourself.";
        /* Deliberately the same answer for "no such player" and "they blocked
         * you": the server never distinguishes them, so neither can this. */
        case FRIEND_WIRE_NOT_FOUND:       return "No player named %s was found.";
        case FRIEND_WIRE_FRIEND_CAP:      return "Your friends list is full.";
        case FRIEND_WIRE_PENDING_CAP:     return "Too many requests are pending.";
        case FRIEND_WIRE_UNAVAILABLE:     return "Friends are unavailable right now.";
        case FRIEND_WIRE_ERROR:
        default:                          return "That did not work. Try again.";
    }
}

void friends_panel_set_result(FriendsState* fs, uint8_t action, uint8_t result,
                              const char* subject_name) {
    if (!fs) return;

    char subject[32];
    snprintf(subject, sizeof(subject), "%s", subject_name ? subject_name : "");
    if (!subject[0]) snprintf(subject, sizeof(subject), "%s", "that player");

    /* The format string comes from the table above, never from the server, so
     * a name carrying a percent sign is data rather than a directive. */
    snprintf(fs->notice, sizeof(fs->notice), result_text(action, result), subject);
    fs->notice_timer = FRIEND_NOTICE_SECONDS;
}

void friends_panel_notice(FriendsState* fs, const char* text) {
    if (!fs || !text) return;
    snprintf(fs->notice, sizeof(fs->notice), "%s", text);
    fs->notice_timer = FRIEND_NOTICE_SECONDS;
}

void friends_panel_update(FriendsState* fs, float delta_time) {
    if (!fs) return;
    if (fs->notice_timer > 0.0f) {
        fs->notice_timer -= delta_time;
        if (fs->notice_timer <= 0.0f) {
            fs->notice_timer = 0.0f;
            fs->notice[0] = '\0';
        }
    }
}

FriendPendingKind friends_panel_take_pending(FriendsState* fs, char* out_name,
                                             size_t out_size) {
    if (!fs) return FRIEND_PENDING_NONE;

    FriendPendingKind kind = fs->pending_kind;
    if (kind == FRIEND_PENDING_NONE) return FRIEND_PENDING_NONE;

    if (out_name && out_size) snprintf(out_name, out_size, "%s", fs->pending_name);

    fs->pending_kind = FRIEND_PENDING_NONE;
    fs->pending_name[0] = '\0';
    return kind;
}

int friends_panel_request_count(const FriendsState* fs) {
    return fs ? fs->request_count : 0;
}

/* --- Input --------------------------------------------------------------- */

/** Queue one action against a named player. */
static void queue(FriendsState* fs, FriendPendingKind kind, const char* name) {
    fs->pending_kind = kind;
    snprintf(fs->pending_name, sizeof(fs->pending_name), "%s", name ? name : "");
}

/** Hit-test the row strip. @return the row index, or -1. */
static int row_at(const FriendsState* fs, float py, float my) {
    float top = fp_rows_top(py);
    if (my < top) return -1;

    int offset = (int)((my - top) / FP_ROW_H);
    if (offset < 0 || offset >= FP_VISIBLE) return -1;

    int index = fs->scroll + offset;
    return (index < fp_row_count(fs)) ? index : -1;
}

/** Act on a click that landed on a friends-tab row. */
static void click_friend_row(FriendsState* fs, int index, float px, float mx, float row_y_off) {
    float btn_x = px + FP_PW - FP_PAD - FP_BTN_W;
    if (mx >= btn_x && mx <= btn_x + FP_BTN_W && row_y_off >= 5.0f
        && row_y_off <= 5.0f + FP_BTN_H) {
        queue(fs, FRIEND_PENDING_REMOVE, fs->friends[index].name);
    }
}

/** Act on a click that landed on a requests-tab row. */
static void click_request_row(FriendsState* fs, int index, float px, float mx, float row_y_off) {
    if (row_y_off < 5.0f || row_y_off > 5.0f + FP_BTN_H) return;

    float decline_x = px + FP_PW - FP_PAD - FP_BTN_W;
    float accept_x  = decline_x - FP_BTN_W - 6.0f;

    if (mx >= decline_x && mx <= decline_x + FP_BTN_W)
        queue(fs, FRIEND_PENDING_DECLINE, fs->requests[index].from_name);
    else if (mx >= accept_x && mx <= accept_x + FP_BTN_W)
        queue(fs, FRIEND_PENDING_ACCEPT, fs->requests[index].from_name);
}

int friends_panel_handle_input(FriendsState* fs,
                               float mx, float my, int clicked,
                               int key_toggle, int key_esc,
                               int page_up, int page_down,
                               int vw, int vh) {
    if (!fs) return 0;

    if (key_toggle) { friends_panel_toggle(fs); return 1; }
    if (!fs->is_open) return 0;
    if (key_esc)     { fs->is_open = 0; return 1; }

    float ph = fp_panel_height();
    float px, py;
    fp_panel_origin(vw, vh, &px, &py);

    int inside = (mx >= px && mx <= px + FP_PW && my >= py && my <= py + ph);

    fs->hovered = inside ? row_at(fs, py, my) : -1;

    if (page_up || page_down) {
        fs->scroll += page_down ? FP_VISIBLE : -FP_VISIBLE;
        clamp_scroll(fs);
        return 1;
    }

    if (!clicked) return inside;

    if (!inside) { fs->is_open = 0; return 1; }

    /* Tabs. Switching resets the scroll, because the two lists have unrelated
     * lengths and carrying an offset across them lands past the end. */
    float tab_y = py + FP_TITLE_H;
    if (my >= tab_y && my <= tab_y + FP_TAB_H) {
        int want = (mx < px + FP_PW * 0.5f) ? FRIENDS_TAB_LIST : FRIENDS_TAB_REQUESTS;
        if (want != fs->tab) {
            fs->tab = want;
            fs->scroll = 0;
            fs->hovered = -1;
        }
        return 1;
    }

    int index = row_at(fs, py, my);
    if (index >= 0) {
        float row_y_off = my - (fp_rows_top(py) + (float)(index - fs->scroll) * FP_ROW_H);
        if (fs->tab == FRIENDS_TAB_REQUESTS) click_request_row(fs, index, px, mx, row_y_off);
        else                                 click_friend_row(fs, index, px, mx, row_y_off);
    }

    return 1;
}
