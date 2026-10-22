// Directory answers: who is online, by id and as a paged roster.

#include "player_directory.h"
#include "status_rules.h"
#include "outbox.h"
#include "log.h"
#include "player_data.h"
#include "str_fixed.h"
#include "utils.h"

#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

// record the latency a client reported
void player_directory_record_ping(uint32_t character_id, int player_slot,
                                  uint16_t ping_ms) {
	ActivePlayer* player = player_acquire_hint(character_id, player_slot);

	if(player) {
		player->ping_ms = ping_ms;
		player_release(player);
	}
}

// send a client its character data
void player_directory_send_data(int client_fd, uint32_t character_id) {
	player_send_data_response(client_fd, character_id);
}

/**
 * Answer a name query: resolve as many character identifiers as are online.
 *
 * This is what replaced the client labelling nearby players "Player_<id>". The
 * alternative was a name field in every nearby-player broadcast -- 32 bytes
 * per player, thirty-two players, twenty times a second, to resend a constant.
 * A name changes never; asking once and caching is the right shape.
 *
 * Deliberately narrower than the session roster this sits next to. It answers
 * only identifiers the caller already named, so it tells a client nothing it
 * could not already see, and it cannot be walked to enumerate the population:
 * character ids are a dense serial range, but a client that guesses one learns
 * only a name it could have learned by standing next to that player.
 */
void player_directory_names(int client_fd, const uint32_t* character_ids, int asked) {
    NameQueryResponsePacket resp;
    memset(&resp, 0, sizeof(resp));
    resp.header.type = PACKET_NAME_QUERY_RESPONSE;

    int found = 0;
    for (uint8_t i = 0; i < asked; i++) {
        uint32_t id = ntohl(character_ids[i]);
        if (id == 0) continue;

        ActivePlayer* p = player_acquire(id);
        if (!p) continue;   /* offline: left out, not returned blank */

        resp.entries[found].character_id = htonl(id);
        STR_COPY_FIELD(resp.entries[found].name, p->username);
        player_release(p);
        found++;
    }

    if (found == 0) return;

    resp.count = (uint8_t)found;

    /* Only the entries that were filled. The packet's array is sized for the
     * worst case; sending all of it would send 31 zeroed entries to answer one
     * name. */
    size_t send_size = offsetof(NameQueryResponsePacket, entries) +
                       (size_t)found * sizeof(NameQueryEntry);
    resp.header.payload_size = htons((uint16_t)(send_size - sizeof(PacketHeader)));

    outbox_send(client_fd, &resp, send_size);
    LOG_DEBUG("[NAME] resolved %d of %d requested names", found, asked);
}

/**
 * Build and send one page of online player summaries.
 *
 * Gated, because this is an enumeration surface.
 *
 * It returns a paginated roster of *every* online player -- id, name, level,
 * race and ping -- to any authenticated client, with no scoping to who is
 * nearby, who is in a party, or who is a friend. Whatever it was built for
 * (a "who" list, a debug view), what it hands out is the population of the
 * world on demand: a complete list of who is playing right now, refreshable,
 * and correlating names with characters and activity times.
 *
 * The list itself is a reasonable feature to have, so it is disabled by
 * default rather than deleted, and a deployment that wants it says so with
 * `session_list = on` in the world's .conf. When it is off the request is
 * ignored -- not answered with an error, because a distinguishable refusal
 * still confirms the world is there and the client is authenticated.
 */
void player_directory_session_page(int client_fd, uint16_t requested_page) {
    extern ActivePlayer active_players[];

    // snapshot online records before pagination
    typedef struct {
        uint32_t character_id;
        char     name[32];
        int      level;
        uint8_t  race_id;
		uint8_t  status;
        uint16_t ping_ms;
    } Snap;

    Snap* snaps = NULL;
    int   snap_count = 0;

	double now = get_time_seconds();

    player_registry_rdlock();
    int online_count = 0;
    const int* online = player_active_list_locked(&online_count);
    // Count first
    for (int n = 0; n < online_count; n++) {
        int i = online[n];
        if (active_players[i].is_loaded && active_players[i].is_ready)
            snap_count++;
    }
    if (snap_count > 0) {
        snaps = malloc((size_t)snap_count * sizeof(Snap));
        if (!snaps) {
            // Allocation failure: treat as zero players rather than crash
            snap_count = 0;
        } else {
            int idx = 0;
            for (int n = 0; n < online_count && idx < snap_count; n++) {
                int i = online[n];
                ActivePlayer* ap = &active_players[i];
                if (!ap->is_loaded || !ap->is_ready) continue;
                // copy each record under its player lock
                pthread_mutex_lock(&ap->lock);
                snaps[idx].character_id = ap->character_id;
                strncpy(snaps[idx].name, ap->username, 31);
                snaps[idx].name[31]     = '\0';
                snaps[idx].level        = ap->level;
                snaps[idx].race_id = (uint8_t)ap->race_id;
				snaps[idx].status = status_effective(ap->status_choice, ap->last_activity_at, now);
                snaps[idx].ping_ms      = ap->ping_ms;
                pthread_mutex_unlock(&ap->lock);

				// Appearing Offline: nobody else is shown them.
				if(snaps[idx].status == SESSION_STATUS_OFFLINE)
					continue;
                idx++;
            }
            snap_count = idx;
        }
    }
    player_registry_unlock();

    uint32_t total_players = (uint32_t)snap_count;
    uint16_t total_pages   = (uint16_t)((snap_count + SESSION_LIST_PAGE_SIZE - 1) / SESSION_LIST_PAGE_SIZE);
    if (total_pages == 0) total_pages = 1;
    if (requested_page >= total_pages) requested_page = total_pages - 1;

    int page_start = (int)requested_page * SESSION_LIST_PAGE_SIZE;
    int page_count = snap_count - page_start;
    if (page_count > SESSION_LIST_PAGE_SIZE) page_count = SESSION_LIST_PAGE_SIZE;
    if (page_count < 0) page_count = 0;

    SessionListResponsePacket resp;
    memset(&resp, 0, sizeof(resp));
    resp.header.type         = PACKET_SESSION_LIST_RESPONSE;
    resp.total_players       = htonl(total_players);
    resp.total_pages         = htons(total_pages);
    resp.current_page        = htons(requested_page);
    resp.count               = (uint8_t)page_count;

    for (int i = 0; i < page_count; i++) {
        Snap* s = &snaps[page_start + i];
        SessionPlayerEntry* e = &resp.entries[i];
        e->player_id    = htonl(s->character_id);
        strncpy(e->name, s->name, 31);
        e->name[31]     = '\0';
        e->level        = (uint8_t)(s->level > 255 ? 255 : s->level);
		e->status       = s->status;
        e->player_race  = s->race_id;
        e->ping_ms      = htons(s->ping_ms);
    }

    free(snaps);

    size_t send_size = offsetof(SessionListResponsePacket, entries) +
                       (size_t)page_count * sizeof(SessionPlayerEntry);

	// The client frames on this length, so it is what is sent, not the struct.
	resp.header.payload_size = htons((uint16_t)(send_size - sizeof(PacketHeader)));
    outbox_send(client_fd, &resp, send_size);
    LOG_DEBUG("[SESSION] Sent page %u/%u (%d entries, %u total) to fd=%d", requested_page + 1, total_pages, page_count, total_players, client_fd);
}
