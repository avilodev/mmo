// What happens to a player when a connection enters or leaves the world.

#include "player_session.h"
#include "ability_handler.h"
#include "friends.h"
#include "log.h"
#include "party.h"
#include "player_data.h"
#include "player_level.h"
#include "quest_system.h"
#include "session_registry.h"
#include "shop_session.h"
#include "utils.h"
#include "world_clock.h"
#include "weather_service.h"
#include "world_entry_service.h"

player_session_result player_session_open(int fd, uint32_t account_id,
                                          uint32_t character_id, int* out_slot,
                                          int* out_added) {
    /* The only way this fails is a descriptor outside the registry's table.
     *
     * The message here used to be "Account already logged in", which was never
     * what a failure meant: a duplicate login is resolved inside
     * session_registry_add() by kicking the stale session, and that path
     * succeeds. The client was told its account was in use when what actually
     * happened was that the server had run past its own table, so the one
     * report of a real capacity problem was disguised as a routine one. */
	if(session_registry_add(fd, account_id, character_id) != 0) {
		LOG_ERROR("[NET] fd %d could not be registered for account %u",
		          fd, account_id);
		return PLAYER_SESSION_BUSY;
	}

	*out_slot = -1;
	*out_added = player_add_active(character_id, fd, out_slot);

	if(*out_added == PLAYER_ADD_FAILED) {
		session_registry_remove(fd);
		return PLAYER_SESSION_LOAD_FAILED;
	}

	return PLAYER_SESSION_OK;
}

void player_session_abort(int fd, uint32_t character_id) {
	session_registry_remove(fd);
	player_remove_active_if_fd(character_id, fd);
}

void player_session_announce(int fd, uint32_t account_id, uint32_t character_id,
                             int added) {
	// The time and the forecast do not depend on the character, so they go out on both paths
	world_clock_send(fd, character_id);
	weather_service_send(fd, character_id);

    /* Only send the session contents when there is a session to send.
     *
     * PLAYER_ADD_LOADING means this connection rebound onto a slot whose
     * database read is still in flight: a reconnect racing its own previous
     * login. Everything below reads that slot, and reading it now returns the
     * zeroes it was cleared to -- a level-0 character with no items, no stats
     * and no quests, which the client then displays. The connection is
     * accepted; the client asks for its data again over the normal request
     * path once the login already running has published it. */
    if (added == PLAYER_ADD_LOADING) {
        LOG_INFO("[NET] character %u reconnected while its login was still "
                 "loading; deferring the session payload", character_id);
    } else {
        player_send_data_response(fd, character_id);

        char friend_name[32] = {0};

        ActivePlayer* p = player_acquire(character_id);
        if (p) {
            /* The slot learns which account is playing it here, and nowhere
             * else. Friends attributes every mutation to this field rather
             * than to anything the client sends, so it has to be written on
             * the one path that has already checked ownership above. */
            p->account_id = account_id;

            snprintf(friend_name, sizeof(friend_name), "%s", p->username);

            player_send_stats_locked(fd, p);
            ability_send_data(fd, p);
			p->last_activity_at = get_time_seconds();
			p->status_choice = 0;
            p->is_ready = 1;   /* handshake complete; broadcasts may start */
            player_release(p);
        }
        quest_send_all(character_id, fd);

        /* After the slot is published, because this both reads it and sends to
         * it. The name given here is the one every friend of this account sees
         * in their panel until the character logs out. */
        world_friends_player_entered(account_id, character_id, friend_name);
    }
}

void player_session_linkdead(int fd, uint32_t account_id, uint32_t character_id) {
	// Out of the registry first, so a fast reconnect is not blocked behind the save
	session_registry_remove(fd);

	if(!player_mark_linkdead(character_id, fd, get_time_seconds())) {
		LOG_INFO("[LINKDEAD] fd=%d: char=%u is not this connection's any more "
		         "(rebound or gone) -- nothing to hold", fd, character_id);
		return;
	}

	// An open shop does not survive a drop; the player reopens it on return
	shop_session_close(character_id);

	// Save it now so a crash while it waits loses nothing; skip if it was rebound
	PlayerSaveData save_data;
	int do_save = 0;
	ActivePlayer* player = player_acquire(character_id);

	if(player) {
		if(player->is_loaded && player->client_fd < 0 && player->linkdead_at > 0.0) {
			player_snapshot_for_save(player, &save_data);
			player->is_dirty = 0;
			player->is_dirty_critical = 0;
			do_save = 1;
		}
		player_release(player);
	}

	if(do_save) {
		if(!player_commit_save(&save_data))
			LOG_ERROR("[LINKDEAD] failed to save character %u", character_id);
		player_snapshot_release(&save_data);
	}

	LOG_INFO("[LINKDEAD] account %u, character %u kept in the world for %.0fs",
	         account_id, character_id, (double)LINKDEAD_GRACE_SECONDS);
}

// Remove the link-dead characters that have been so for at least grace seconds.
static int sweep_linkdead(double grace) {
	uint32_t expired[LINKDEAD_SWEEP_MAX];
	int count = player_linkdead_expired(get_time_seconds(), grace,
	                                    expired, LINKDEAD_SWEEP_MAX);

	for(int i = 0; i < count; i++) {
		uint32_t account_id = 0;

		// it may have come back since it was listed
		if(!player_remove_linkdead(expired[i], &account_id))
			continue;

		party_handle_disconnect(expired[i]);
		world_friends_player_left(account_id, expired[i]);
		shop_session_close(expired[i]);
		world_entry_clear_live(expired[i]);

		LOG_INFO("[LINKDEAD] account %u, character %u did not return -- removed",
		         account_id, expired[i]);
	}
	return count;
}

void player_session_sweep_linkdead(void) {
	sweep_linkdead(LINKDEAD_GRACE_SECONDS);
}

void player_session_release_linkdead(void) {
	while(sweep_linkdead(0.0) == LINKDEAD_SWEEP_MAX)
		;
}

void player_session_close(int fd, uint32_t account_id, uint32_t character_id) {
    int client_fd = fd;

    /* Out of the registry FIRST, so a fast reconnect is not blocked behind
     * the database write below. */
    session_registry_remove(client_fd);

    /* Save only while we still own the slot. When a stale session is kicked,
     * the new connection may already have loaded the same character. */
    ActivePlayer* player = player_acquire(character_id);
    int do_save = 0;
    PlayerSaveData save_data;

    if (player) {
        if (player->client_fd == client_fd) {
            if (player->is_loaded) {
                /* Copied out under the slot lock and written after releasing
                 * it, so broadcast threads are not blocked for the full
                 * duration of the write. */
                player_snapshot_for_save(player, &save_data);
                do_save = 1;
                /* Clear dirty so player_remove_active will not repeat the
                 * write while holding the registry lock. Both bits: the
                 * snapshot above carries the milestone state too, so
                 * leaving is_dirty_critical set would have the milestone
                 * pass write it a second time. */
                player->is_dirty = 0;
                player->is_dirty_critical = 0;
            }
            player_release(player);
            if (player_remove_active_if_fd(character_id, client_fd)) {
                party_handle_disconnect(character_id);
                /* Only on the branch that actually took the slot down. The
                 * else below is a reconnect that rebound it, and clearing
                 * presence there would mark a player offline who is, at
                 * that moment, online on this very server. */
                world_friends_player_left(account_id, character_id);
            } else {
                /* A reconnect rebound the slot after our snapshot. Do not
                 * write stale data over the live session. */
                do_save = 0;
                LOG_INFO("[CLEANUP] fd=%d: slot rebound during cleanup — "
                         "preserving the new session", client_fd);
            }
        } else {
            LOG_INFO("[CLEANUP] fd=%d: slot now owned by fd=%d (char=%u) — "
                     "skipping save", client_fd, player->client_fd, character_id);
            player_release(player);
        }
    } else {
        LOG_INFO("[CLEANUP] fd=%d: no player slot for char=%u (already removed?)",
                 client_fd, character_id);
    }

    if (do_save && !player_commit_save(&save_data))
        LOG_ERROR("[CLEANUP] failed to save character %u", character_id);

    /* Per-character state keyed on nothing but the character id, which the
     * next login would otherwise inherit: an open shop must not survive a
     * disconnect, or a reconnecting player resumes trading at a merchant
     * they are no longer standing next to. */
    shop_session_close(character_id);
}
