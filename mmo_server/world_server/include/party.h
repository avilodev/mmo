#ifndef PARTY_H
#define PARTY_H

#include "types.h"
#include "player_data.h"

/** Bound party and pending-invitation pools and set invitation lifetime. */
#define MAX_PARTIES 200
#define MAX_PENDING_INVITES 100
#define INVITE_EXPIRY_SECONDS 30.0

/** Hold one party's membership under its per-party mutex.
 *
 * The pool that holds these is private to party.c. Nothing outside that file
 * may hold a `Party*`: the slot it points at is recycled by party_remove_member()
 * and party_disband(), so a pointer obtained by a lookup is only meaningful
 * while the pool lock that produced it is still held. Callers use PartySnapshot
 * and the query functions below instead.
 */
typedef struct {
    uint32_t party_id;          // 0 = unused slot
    uint32_t leader_id;         // character_id of leader
    uint32_t members[MAX_PARTY_SIZE]; // character_ids (0 = empty slot)
    uint8_t  member_count;
    pthread_mutex_t lock;
} Party;

/** A lock-free copy of one party's membership, taken at a single instant.
 *
 * Every read outside party.c goes through one of these. The value may be stale
 * by the time it is used -- a member can leave in between -- but it is never
 * torn, and it never aliases a slot another thread is free to recycle.
 */
typedef struct {
    uint32_t party_id;
    uint32_t leader_id;
    uint32_t members[MAX_PARTY_SIZE];
    uint8_t  member_count;
} PartySnapshot;

/** Track one expiring party invitation. */
typedef struct {
    uint32_t from_id;           // inviter character_id
    uint32_t to_id;             // invitee character_id
    uint32_t party_id;          // party to join (0 = create new on accept)
    double   invite_time;       // CLOCK_MONOTONIC timestamp
} PendingInvite;

void party_init(void);

/** Copy a party by identifier. Returns 1 when found, 0 otherwise. */
int party_snapshot(uint32_t party_id, PartySnapshot* out);

/** Copy the party containing a character. Returns 1 when found, 0 otherwise. */
int party_snapshot_of_player(uint32_t character_id, PartySnapshot* out);

/** Return the party a character belongs to, or 0 when it belongs to none. */
uint32_t party_id_of_player(uint32_t character_id);

/** Report whether a character leads a party. */
int party_is_leader(uint32_t party_id, uint32_t character_id);

/** Report whether a character is a member of a party. */
int party_has_member(uint32_t party_id, uint32_t character_id);

// return the new party identifier or zero on failure
uint32_t party_create(uint32_t leader_id);

/** Add a character to a party.
 *
 * Refuses a character that is already in any party, decided under the pool lock
 * together with the insertion, so two concurrent accepts cannot both pass a
 * separate "are you in a party" check and land the same character twice.
 *
 * @return 1 on success, or 0 when the party is absent or full, or the character
 *         already belongs to a party.
 */
int party_add_member(uint32_t party_id, uint32_t character_id);

void party_remove_member(uint32_t character_id);

void party_disband(uint32_t party_id);

void party_handle_disconnect(uint32_t character_id);

void party_broadcast_update(uint32_t party_id);

int party_invite_create(uint32_t from_id, uint32_t to_id, uint32_t party_id);

/** Copy a character's pending, unexpired invitation.
 *
 * Returns a copy rather than a pointer into the invite pool for the same reason
 * PartySnapshot exists: the slot is recycled by party_invite_remove() and by
 * expiry, so a pointer to it is only valid while the invite lock is held.
 *
 * @param out  Receives the invitation; untouched when none applies.
 * @return     1 when an unexpired invitation was copied, otherwise 0.
 */
int party_invite_find_for_player(uint32_t to_id, PendingInvite* out);
void party_invite_remove(uint32_t to_id);
void party_invite_cleanup_expired(void);

// split XP among living nearby members or grant it to the killer
void party_award_xp(uint32_t killer_id, uint64_t xp_amount);

#endif
