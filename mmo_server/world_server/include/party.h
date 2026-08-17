#ifndef PARTY_H
#define PARTY_H

#include "types.h"
#include "player_data.h"

/** Bound party and pending-invitation pools and set invitation lifetime. */
#define MAX_PARTIES 200
#define MAX_PENDING_INVITES 100
#define INVITE_EXPIRY_SECONDS 30.0

/** Hold one party's membership under its per-party mutex. */
typedef struct {
    uint32_t party_id;          // 0 = unused slot
    uint32_t leader_id;         // character_id of leader
    uint32_t members[MAX_PARTY_SIZE]; // character_ids (0 = empty slot)
    uint8_t  member_count;
    pthread_mutex_t lock;
} Party;

/** Track one expiring party invitation. */
typedef struct {
    uint32_t from_id;           // inviter character_id
    uint32_t to_id;             // invitee character_id
    uint32_t party_id;          // party to join (0 = create new on accept)
    double   invite_time;       // CLOCK_MONOTONIC timestamp
} PendingInvite;

void party_init(void);

// return mutable pool storage that requires external synchronization
Party* party_find(uint32_t party_id);

// return mutable pool storage that requires external synchronization
Party* party_find_by_player(uint32_t character_id);

// return the new party identifier or zero on failure
uint32_t party_create(uint32_t leader_id);

int party_add_member(uint32_t party_id, uint32_t character_id);

void party_remove_member(uint32_t character_id);

void party_disband(uint32_t party_id);

void party_handle_disconnect(uint32_t character_id);

void party_broadcast_update(uint32_t party_id);

int party_invite_create(uint32_t from_id, uint32_t to_id, uint32_t party_id);
// return mutable internal invitation storage or NULL when absent
PendingInvite* party_invite_find_for_player(uint32_t to_id);
void party_invite_remove(uint32_t to_id);
void party_invite_cleanup_expired(void);

// split XP among living nearby members or grant it to the killer
void party_award_xp(uint32_t killer_id, uint64_t xp_amount);

#endif
