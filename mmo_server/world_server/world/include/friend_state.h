#ifndef FRIEND_STATE_H
#define FRIEND_STATE_H

// What this world knows about its players' friends: who is online, the cached
// friend, request and block lists, and the live event feed. Game logic asks
// this module, and this module asks storage.

#include "friend_bus.h"
#include "presence.h"

#include <stdint.h>

// record which character an account is playing, return 1 on success
int friend_state_set_presence(uint32_t account_id, uint32_t world_id,
                              uint32_t character_id, const char* name);

// clear an account's presence only if it still names this character
int friend_state_clear_presence(uint32_t account_id, uint32_t character_id);

// keep the presence of these accounts from expiring
int friend_state_refresh(const uint32_t* account_ids, int count);

// look up the presence of several accounts
int friend_state_presence_of(const uint32_t* account_ids, int count,
                             PresenceRecord* out);

// read an account's cached friends, requests and blocks
int friend_state_friends(uint32_t account_id, FriendCacheEntry* out, int max);
int friend_state_requests(uint32_t account_id, FriendRequestCache* out, int max);
int friend_state_blocks(uint32_t account_id, uint32_t* out, int max);

// start and stop the live feed of friend and presence events
int friend_state_start_feed(uint32_t world_id, FriendEventFn on_event,
                            PresenceEventFn on_presence, void* user);
void friend_state_stop_feed(void);

#endif /* FRIEND_STATE_H */
