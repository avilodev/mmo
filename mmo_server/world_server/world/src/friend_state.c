// What this world knows about its players' friends, kept in storage.

#include "friend_state.h"

int friend_state_set_presence(uint32_t account_id, uint32_t world_id,
                              uint32_t character_id, const char* name) {
	return presence_set(account_id, world_id, character_id, name);
}

int friend_state_clear_presence(uint32_t account_id, uint32_t character_id) {
	return presence_clear_if_character(account_id, character_id);
}

int friend_state_refresh(const uint32_t* account_ids, int count) {
	return presence_refresh_many(account_ids, count);
}

int friend_state_presence_of(const uint32_t* account_ids, int count,
                             PresenceRecord* out) {
	return presence_get_many(account_ids, count, out);
}

int friend_state_friends(uint32_t account_id, FriendCacheEntry* out, int max) {
	return presence_friends_load(account_id, out, max);
}

int friend_state_requests(uint32_t account_id, FriendRequestCache* out, int max) {
	return presence_requests_load(account_id, out, max);
}

int friend_state_blocks(uint32_t account_id, uint32_t* out, int max) {
	return presence_blocks_load(account_id, out, max);
}

int friend_state_start_feed(uint32_t world_id, FriendEventFn on_event,
                            PresenceEventFn on_presence, void* user) {
	return friend_bus_start(world_id, on_event, on_presence, user);
}

void friend_state_stop_feed(void) {
	friend_bus_stop();
	presence_consumer_close();
}
