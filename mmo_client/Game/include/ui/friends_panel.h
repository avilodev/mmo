#ifndef FRIENDS_PANEL_H
#define FRIENDS_PANEL_H

/**
 * @file
 * Hold the client's friends list and pending requests, and draw the panel.
 *
 * The panel owns no truth. Everything in it arrives from the server -- the
 * list, the presence, the requests -- and every action leaves as a packet and
 * changes nothing here until the server says so. A removed friend stays on
 * screen until the refreshed list drops them, which is what makes a refused
 * action look like a refusal rather than like a success that undid itself.
 *
 * Friendships are between accounts, not characters, so a friend is identified
 * by account id and displayed under a character name. Online, that is the
 * character they are on; offline, it is the one they were last seen playing,
 * which is also what the panel's commands take. The status beside it is only
 * ever "on world N" or "Offline" -- the game keeps no history to show.
 *
 * Adding a friend by name is a slash command (/friend), not a text field in
 * here: the client has exactly one text input, the chat bar, and a second one
 * would be a second copy of its key handling. The panel is list and buttons.
 */

#include "protocol.h"

#include <stdint.h>

/** Match the protocol's per-packet ceilings; the server enforces the real caps. */
#define FRIENDS_MAX         MAX_FRIENDS_PER_PACKET
#define FRIEND_REQUESTS_MAX MAX_FRIEND_REQUESTS_PER_PACKET

/** How long a result message stays on the panel, in seconds. */
#define FRIEND_NOTICE_SECONDS 6.0f

/** Track one friend as last reported by the server.
 *
 * Current status only. The game keeps no record of where a player used to be,
 * so there is nothing here about a last world or a last time seen -- a friend
 * is on a world right now, or they are offline.
 */
typedef struct {
    uint32_t account_id;
    uint32_t world_id;        /**< Meaningless when offline. */
    char     name[32];        /**< Who they are; present whether online or not. */
    uint8_t  online;
} FriendRow;

/** Track one request waiting for an answer. */
typedef struct {
    uint32_t from_account;
    char     from_name[32];
    int64_t  created_at;
} FriendRequestRow;

/** What the panel wants sent, drained by the input layer.
 *
 * The panel draws and hit-tests and knows nothing about sockets, exactly as the
 * quest log does with its abandon request. One slot is enough because a click
 * is drained on the same frame it is made.
 */
typedef enum {
    FRIEND_PENDING_NONE = 0,
    FRIEND_PENDING_ACCEPT,
    FRIEND_PENDING_DECLINE,
    FRIEND_PENDING_REMOVE,
    FRIEND_PENDING_REFRESH
} FriendPendingKind;

/** Which list the panel is showing. */
typedef enum {
    FRIENDS_TAB_LIST = 0,
    FRIENDS_TAB_REQUESTS = 1
} FriendsTab;

/** Track the friends list, the pending requests, and the panel's own state. */
typedef struct {
    FriendRow        friends[FRIENDS_MAX];
    int              friend_count;

    FriendRequestRow requests[FRIEND_REQUESTS_MAX];
    int              request_count;

    int              is_open;
    int              tab;             /**< FriendsTab. */
    int              scroll;          /**< First visible row of the active tab. */
    int              hovered;         /**< Hovered row, or -1. */

    /** Set once the list has arrived, so "loading" and "no friends" differ.
     *
     * Without this an empty panel is ambiguous, and the ambiguity resolves the
     * wrong way: a player whose list has not arrived yet is told they have no
     * friends, which is a statement they act on. */
    int              have_list;

    /** One queued action for the input layer to send. */
    FriendPendingKind pending_kind;
    char              pending_name[32];

    /** The last result the server reported, shown for FRIEND_NOTICE_SECONDS. */
    char              notice[96];
    float             notice_timer;
} FriendsState;

void friends_panel_init(FriendsState* fs);

/** Show a hidden panel or hide a shown one.
 *
 * Opening queues a refresh: the caches this reads are the server's, and asking
 * on open is what keeps a panel that has been shut for an hour from showing an
 * hour-old list.
 */
void friends_panel_toggle(FriendsState* fs);

/** Replace the friends list from a FRIEND_LIST_RESPONSE. */
void friends_panel_set_list(FriendsState* fs, const FriendListResponsePacket* pkt,
                            int packet_bytes);

/** Replace the pending requests from a FRIEND_REQUESTS_LIST. */
void friends_panel_set_requests(FriendsState* fs, const FriendRequestsListPacket* pkt,
                                int packet_bytes);

/** Apply one presence change in place.
 *
 * Only touches a friend already in the list. A presence update for somebody who
 * is not in it means the list is behind, and the refreshed list is what fixes
 * that -- inventing a row here would show a friend with no name and no history.
 */
void friends_panel_apply_presence(FriendsState* fs,
                                  const FriendPresenceUpdatePacket* pkt);

/** Record a result so the player sees why an action did or did not work. */
void friends_panel_set_result(FriendsState* fs, uint8_t action, uint8_t result,
                              const char* subject_name);

/** Show an arbitrary line in the notice area, for locally-detected problems. */
void friends_panel_notice(FriendsState* fs, const char* text);

/** Age the notice out. Call once per frame. */
void friends_panel_update(FriendsState* fs, float delta_time);

/** Take the queued action, clearing it.
 *
 * @param out_name  Receives the subject's name; must hold at least 32 bytes.
 * @return The action, or FRIEND_PENDING_NONE when nothing is queued.
 */
FriendPendingKind friends_panel_take_pending(FriendsState* fs, char* out_name,
                                             size_t out_size);

/** Report how many requests are waiting, for the HUD badge. */
int friends_panel_request_count(const FriendsState* fs);

void friends_panel_render(const FriendsState* fs, int vw, int vh);

/**
 * Handle one frame of panel input.
 *
 * @param key_toggle  Nonzero on the frame the bound key is pressed.
 * @param key_esc     Nonzero on the frame Escape is pressed.
 * @param page_up     Nonzero on the frame Page Up is pressed.
 * @param page_down   Nonzero on the frame Page Down is pressed.
 * @return Nonzero when the panel consumed the input, so gameplay ignores it.
 *
 * Paged from the keyboard rather than scrolled with the wheel, because this
 * client has no wheel input at all -- adding a GLFW scroll callback for one
 * panel would be a change to the input system rather than to this one.
 */
int friends_panel_handle_input(FriendsState* fs,
                               float mx, float my, int clicked,
                               int key_toggle, int key_esc,
                               int page_up, int page_down,
                               int vw, int vh);

#endif // FRIENDS_PANEL_H
