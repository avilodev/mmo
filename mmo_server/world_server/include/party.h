#ifndef PARTY_H
#define PARTY_H

#include "types.h"
#include "player_data.h"

#define MAX_PARTIES 200
#define MAX_PENDING_INVITES 100
#define INVITE_EXPIRY_SECONDS 30.0

typedef struct {
    uint32_t party_id;          // 0 = unused slot
    uint32_t leader_id;         // character_id of leader
    uint32_t members[MAX_PARTY_SIZE]; // character_ids (0 = empty slot)
    uint8_t  member_count;
    pthread_mutex_t lock;
} Party;

typedef struct {
    uint32_t from_id;           // inviter character_id
    uint32_t to_id;             // invitee character_id
    uint32_t party_id;          // party to join (0 = create new on accept)
    double   invite_time;       // CLOCK_MONOTONIC timestamp
} PendingInvite;

// Initialize the party system
void party_init(void);

// Find a party by ID (returns NULL if not found)
Party* party_find(uint32_t party_id);

// Find the party a player belongs to (returns NULL if not in party)
Party* party_find_by_player(uint32_t character_id);

// Create a new party with the given leader. Returns party_id, or 0 on failure.
uint32_t party_create(uint32_t leader_id);

// Add a member to a party. Returns 1 on success, 0 on failure.
int party_add_member(uint32_t party_id, uint32_t character_id);

// Remove a member from their party. Handles leader succession or disbanding.
void party_remove_member(uint32_t character_id);

// Disband a party and notify all members.
void party_disband(uint32_t party_id);

// Called when a player disconnects — cleans up party membership.
void party_handle_disconnect(uint32_t character_id);

// Send a PACKET_PARTY_UPDATE to all members of a party.
void party_broadcast_update(uint32_t party_id);

// Invite system
int party_invite_create(uint32_t from_id, uint32_t to_id, uint32_t party_id);
PendingInvite* party_invite_find_for_player(uint32_t to_id);
void party_invite_remove(uint32_t to_id);
void party_invite_cleanup_expired(void);

// XP sharing: awards XP to killer and nearby party members (split evenly)
// Falls through to regular player_award_xp if not in a party.
void party_award_xp(uint32_t killer_id, uint64_t xp_amount);

#endif
