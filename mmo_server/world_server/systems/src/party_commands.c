// Party commands: invite, accept, decline, leave and kick, decided on the server.

#include "party_commands.h"
#include "outbox.h"
#include "log.h"
#include "player_data.h"
#include "player_effects.h"
#include "player_level.h"
#include "item_registry.h"
#include "loot.h"
#include "str_fixed.h"
#include "utils.h"
#include "zone_system.h"
#include "emote_handler.h"
#include "npc_query.h"
#include "friends.h"
#include "party.h"
#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <arpa/inet.h>

/** Validate and deliver a party invitation by player name. */
void party_command_invite(int client_fd, uint32_t character_id, const char* target_name) {
    // Acquire inviter, extract all needed fields, release immediately
    ActivePlayer* inviter = player_acquire(character_id);
    if (!inviter) return;
    uint32_t inviter_party_id = inviter->party_id;
    uint32_t inviter_account  = inviter->account_id;
    char inviter_name[32];
    strncpy(inviter_name, inviter->username, 31);
    inviter_name[31] = '\0';
    player_release(inviter);

    /* Find the target by name.
     *
     * One index lookup, on a network loop thread, in place of a walk over
     * every online player taking each one's mutex until the strings matched. */
    uint32_t target_id = player_find_by_name(target_name);

    if (target_id == 0) {
        LOG_WARN_RL(5, 60, "[PARTY] Invite failed: player '%s' not found", target_name);
        return;
    }

    // Can't invite yourself
    if (target_id == character_id) {
        LOG_WARN_RL(5, 60, "[PARTY] Invite failed: can't invite yourself");
        return;
    }

    /* A block covers invitations too. An invite is a popup on somebody's
     * screen with the sender's name on it, which is the thing a block is for.
     *
     * Swallowed rather than refused with a reason, the same way a whisper and
     * a friend request are: every distinguishable failure tells the sender the
     * block exists, and a sender who knows makes another account. The inviter
     * sees exactly what they would see inviting somebody who is not there. */
    {
        ActivePlayer* target = player_acquire(target_id);
        uint32_t target_account = target ? target->account_id : 0;
        if (target) player_release(target);

        if (world_friends_is_blocked(target_account, inviter_account)) {
            LOG_DEBUG("[PARTY] account %u is blocked by %u; invite swallowed",
                      inviter_account, target_account);
            return;
        }
    }

    /* Advisory: the invitation is only a hint that an accept may succeed, and
     * party_add_member() re-decides membership atomically when it arrives. These
     * checks exist so the inviter learns immediately that it will not work. */
    if (party_id_of_player(target_id) != 0) {
        LOG_WARN_RL(5, 60, "[PARTY] Invite failed: '%s' already in a party", target_name);
        return;
    }

    if (inviter_party_id != 0) {
        // Already in a party — check if leader and if party is full
        PartySnapshot inviter_party;
        if (party_snapshot(inviter_party_id, &inviter_party)) {
            if (inviter_party.leader_id != character_id) {
                LOG_WARN_RL(5, 60, "[PARTY] Invite failed: not party leader");
                return;
            }
            if (inviter_party.member_count >= MAX_PARTY_SIZE) {
                LOG_WARN_RL(5, 60, "[PARTY] Invite failed: party full");
                return;
            }
        }
    }

    // Create the invite
    if (!party_invite_create(character_id, target_id, inviter_party_id)) {
        LOG_WARN_RL(5, 60, "[PARTY] Invite failed: target already has pending invite or no slots");
        return;
    }

    // Send notification to target
    PartyInviteNotifyPacket notify = {0};
    notify.header.type = PACKET_PARTY_INVITE_NOTIFY;
    notify.header.player_id = htonl(target_id);
    notify.header.payload_size = htons(sizeof(PartyInviteNotifyPacket) - sizeof(PacketHeader));
    notify.from_id = htonl(character_id);
    STR_COPY_FIELD(notify.from_name, inviter_name);

    ActivePlayer* target = player_acquire(target_id);
    if (target) {
        int fd = target->client_fd;
        player_release(target);
        outbox_send(fd, &notify, sizeof(notify));
    }

    LOG_DEBUG("[PARTY] Player %u invited '%s' (%u) to party", character_id, target_name, target_id);
    (void)client_fd;
}

/** Consume a pending invitation and join or create its party. */
void party_command_accept(int client_fd, uint32_t character_id) {
    (void)client_fd;

    PendingInvite inv;
    if (!party_invite_find_for_player(character_id, &inv)) {
        LOG_WARN_RL(5, 60, "[PARTY] Accept failed: no pending invite for player %u", character_id);
        return;
    }

    uint32_t from_id = inv.from_id;
    uint32_t existing_party = inv.party_id;

    // Clear the invite
    party_invite_remove(character_id);

    /* No "is the acceptor already in a party" check here. It cannot be made to
     * mean anything from outside the module -- the answer can change between the
     * test and the insert -- so party_add_member() decides it under the pool
     * lock and refuses there. */
    uint32_t party_id;

    if (existing_party != 0) {
        // Join existing party
        PartySnapshot target;
        if (!party_snapshot(existing_party, &target)) {
            LOG_WARN_RL(5, 60, "[PARTY] Accept failed: party %u no longer exists", existing_party);
            return;
        }
        party_id = existing_party;
    } else {
        // reuse a party the inviter joined since the invitation
        party_id = party_id_of_player(from_id);
        if (party_id == 0) {
            party_id = party_create(from_id);
            if (party_id == 0) {
                /* Lost a race, or the pool is full. Re-read rather than fail:
                 * a concurrent accept may have just created the inviter's party. */
                party_id = party_id_of_player(from_id);
            } else {
                party_broadcast_update(party_id);
            }

            if (party_id == 0) {
                LOG_WARN_RL(5, 60, "[PARTY] Accept failed: couldn't create party");
                return;
            }
        }
    }

    if (!party_add_member(party_id, character_id)) {
        LOG_WARN_RL(5, 60, "[PARTY] Accept failed: couldn't add player %u to party %u", character_id, party_id);
        return;
    }

    LOG_DEBUG("[PARTY] Player %u accepted invite, joined party %u", character_id, party_id);
    party_broadcast_update(party_id);
}

/** Remove the character's pending party invitation. */
void party_command_decline(int client_fd, uint32_t character_id) {
    (void)client_fd;
    party_invite_remove(character_id);
    LOG_DEBUG("[PARTY] Player %u declined party invite", character_id);
}

/** Remove the character from its current party. */
void party_command_leave(int client_fd, uint32_t character_id) {
    (void)client_fd;
    party_remove_member(character_id);
    LOG_DEBUG("[PARTY] Player %u left their party", character_id);
}

/** Remove a target member when requested by the party leader. */
void party_command_kick(int client_fd, uint32_t character_id, uint32_t target_id) {
    (void)client_fd;

    // Can't kick yourself (use leave)
    if (target_id == character_id) return;

    /* One snapshot answers both questions -- leadership and target membership --
     * from a single consistent instant, instead of two unsynchronized reads of
     * a pool slot that another thread may recycle in between. */
    PartySnapshot party;
    if (!party_snapshot_of_player(character_id, &party)) return;

    // Only leader can kick
    if (party.leader_id != character_id) return;

    // Verify target is in the same party
    int found = 0;
    for (int i = 0; i < MAX_PARTY_SIZE; i++) {
        if (party.members[i] == target_id) {
            found = 1;
            break;
        }
    }
    if (!found) return;

    LOG_DEBUG("[PARTY] Player %u kicked player %u from party", character_id, target_id);
    party_remove_member(target_id);
}
