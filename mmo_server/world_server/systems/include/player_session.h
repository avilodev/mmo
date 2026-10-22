#ifndef PLAYER_SESSION_H
#define PLAYER_SESSION_H

// What happens to a player when a connection enters or leaves the world.
// Network code calls these and handles the socket; nothing here touches one.

#include <stdint.h>

typedef enum {
	PLAYER_SESSION_OK = 0,
	PLAYER_SESSION_BUSY,
	PLAYER_SESSION_LOAD_FAILED
} player_session_result;

// claim a session and a player slot for the connection
player_session_result player_session_open(int fd, uint32_t account_id,
                                          uint32_t character_id, int* out_slot,
                                          int* out_added);

// undo player_session_open when the connection could not be completed
void player_session_abort(int fd, uint32_t character_id);

// send the new player their session and tell their friends they arrived
void player_session_announce(int fd, uint32_t account_id, uint32_t character_id,
                             int added);

// seconds a character whose connection dropped stays in the world awaiting its player
#define LINKDEAD_GRACE_SECONDS 60.0

// most link-dead characters removed in one sweep; the rest wait for the next
#define LINKDEAD_SWEEP_MAX 256

// save and detach the player before the connection goes away
void player_session_close(int fd, uint32_t account_id, uint32_t character_id);

// the connection dropped without a logout: save, then keep the character standing in
// the world, hittable and visible, until it reconnects or the grace period ends
void player_session_linkdead(int fd, uint32_t account_id, uint32_t character_id);

// remove the link-dead characters whose grace period is over; blocks on the
// database and Redis, so it runs on the save thread, never the broadcast thread
void player_session_sweep_linkdead(void);

// remove every link-dead character now, for shutdown, so none is left marked live
void player_session_release_linkdead(void);

#endif /* PLAYER_SESSION_H */
