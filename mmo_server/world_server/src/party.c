/**
 * @file
 * Manage world-server parties, invitations, membership broadcasts, and shared XP.
 */

#include "party.h"
#include "log.h"
#include "player_level.h"
#include "utils.h"

#include <string.h>
#include <stdio.h>
#include <stddef.h>
#include <math.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <time.h>

static Party g_parties[MAX_PARTIES];
static pthread_mutex_t g_parties_lock = PTHREAD_MUTEX_INITIALIZER;

static PendingInvite g_invites[MAX_PENDING_INVITES];
static pthread_mutex_t g_invites_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t g_next_party_id = 1;

static double get_time_mono(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void send_to_character(uint32_t character_id, void* packet, size_t size) {
    ActivePlayer* p = player_acquire(character_id);
    if (!p) return;
    int fd = p->client_fd;
    player_release(p);
    if (fd > 0) server_send(fd, packet, size);
}

/**
 * Initialize party slots, their mutexes, and pending invitations.
 */
void party_init(void) {
    memset(g_parties, 0, sizeof(g_parties));
    for (int i = 0; i < MAX_PARTIES; i++) {
        pthread_mutex_init(&g_parties[i].lock, NULL);
    }
    memset(g_invites, 0, sizeof(g_invites));
    LOG_INFO("[PARTY] Party system initialized (%d max parties, %d max size)", MAX_PARTIES, MAX_PARTY_SIZE);
}

/**
 * Find a party by identifier without acquiring its lock.
 *
 * The returned pool pointer requires external synchronization before access.
 *
 * @return The matching party, or NULL for zero or absence.
 */
Party* party_find(uint32_t party_id) {
    if (party_id == 0) return NULL;
    for (int i = 0; i < MAX_PARTIES; i++) {
        if (g_parties[i].party_id == party_id) {
            return &g_parties[i];
        }
    }
    return NULL;
}

/**
 * Find a character's party without acquiring its lock.
 *
 * The returned pool pointer requires external synchronization before access.
 *
 * @return The containing party, or NULL for zero or absence.
 */
Party* party_find_by_player(uint32_t character_id) {
    if (character_id == 0) return NULL;
    for (int i = 0; i < MAX_PARTIES; i++) {
        if (g_parties[i].party_id == 0) continue;
        for (int j = 0; j < MAX_PARTY_SIZE; j++) {
            if (g_parties[i].members[j] == character_id) {
                return &g_parties[i];
            }
        }
    }
    return NULL;
}

/**
 * Create a party and assign its identifier to the leader.
 *
 * @return The assigned party identifier, or 0 when the pool is full.
 */
uint32_t party_create(uint32_t leader_id) {
    pthread_mutex_lock(&g_parties_lock);

    // Find empty slot
    int slot = -1;
    for (int i = 0; i < MAX_PARTIES; i++) {
        if (g_parties[i].party_id == 0) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        pthread_mutex_unlock(&g_parties_lock);
        LOG_WARN_RL(5, 60, "[PARTY] Cannot create party: all slots full");
        return 0;
    }

    uint32_t pid = g_next_party_id++;
    Party* p = &g_parties[slot];
    pthread_mutex_lock(&p->lock);

    p->party_id = pid;
    p->leader_id = leader_id;
    memset(p->members, 0, sizeof(p->members));
    p->members[0] = leader_id;
    p->member_count = 1;

    pthread_mutex_unlock(&p->lock);
    pthread_mutex_unlock(&g_parties_lock);

    // Set party_id on the player
    ActivePlayer* player = player_acquire(leader_id);
    if (player) {
        player->party_id = pid;
        player_release(player);
    }

    LOG_DEBUG("[PARTY] Party %u created by player %u", pid, leader_id);
    return pid;
}

/**
 * Add a character to a party's first free member slot.
 *
 * @return 1 on success, or 0 for absence or a full party.
 */
int party_add_member(uint32_t party_id, uint32_t character_id) {
    pthread_mutex_lock(&g_parties_lock);
    Party* p = party_find(party_id);
    if (!p) {
        pthread_mutex_unlock(&g_parties_lock);
        return 0;
    }

    pthread_mutex_lock(&p->lock);
    pthread_mutex_unlock(&g_parties_lock);

    if (p->member_count >= MAX_PARTY_SIZE) {
        pthread_mutex_unlock(&p->lock);
        return 0;
    }

    // Find empty member slot
    for (int i = 0; i < MAX_PARTY_SIZE; i++) {
        if (p->members[i] == 0) {
            p->members[i] = character_id;
            p->member_count++;
            pthread_mutex_unlock(&p->lock);

            // Set party_id on the player
            ActivePlayer* player = player_acquire(character_id);
            if (player) {
                player->party_id = party_id;
                player_release(player);
            }

            LOG_DEBUG("[PARTY] Player %u joined party %u (%u members)", character_id, party_id, p->member_count);
            return 1;
        }
    }

    pthread_mutex_unlock(&p->lock);
    return 0;
}

/**
 * Remove a character and promote or disband the party as required.
 */
void party_remove_member(uint32_t character_id) {
    pthread_mutex_lock(&g_parties_lock);
    Party* p = party_find_by_player(character_id);
    if (!p) {
        pthread_mutex_unlock(&g_parties_lock);
        return;
    }

    uint32_t party_id = p->party_id;
    pthread_mutex_lock(&p->lock);
    pthread_mutex_unlock(&g_parties_lock);

    // Remove from members array
    for (int i = 0; i < MAX_PARTY_SIZE; i++) {
        if (p->members[i] == character_id) {
            p->members[i] = 0;
            p->member_count--;
            break;
        }
    }

    // Clear party_id on the player
    ActivePlayer* player = player_acquire(character_id);
    if (player) {
        player->party_id = 0;
        player_release(player);
    }

    LOG_DEBUG("[PARTY] Player %u left party %u (%u members remaining)", character_id, party_id, p->member_count);

    // If party is now empty or has 1 member, disband
    if (p->member_count <= 1) {
        // Get the last member before we disband
        uint32_t last_member = 0;
        for (int i = 0; i < MAX_PARTY_SIZE; i++) {
            if (p->members[i] != 0) {
                last_member = p->members[i];
                break;
            }
        }
        pthread_mutex_unlock(&p->lock);

        // Clear last member's party_id
        if (last_member) {
            ActivePlayer* lp = player_acquire(last_member);
            if (lp) {
                lp->party_id = 0;
                player_release(lp);
            }
            // Notify last member that party disbanded
            PartyDisbandPacket disband = {0};
            disband.header.type = PACKET_PARTY_DISBAND;
            disband.header.player_id = htonl(last_member);
            disband.header.payload_size = htons(sizeof(PartyDisbandPacket) - sizeof(PacketHeader));
            send_to_character(last_member, &disband, sizeof(disband));
        }

        // Clear the party slot
        pthread_mutex_lock(&g_parties_lock);
        Party* pp = party_find(party_id);
        if (pp) {
            pthread_mutex_lock(&pp->lock);
            memset(pp->members, 0, sizeof(pp->members));
            pp->party_id = 0;
            pp->leader_id = 0;
            pp->member_count = 0;
            pthread_mutex_unlock(&pp->lock);
        }
        pthread_mutex_unlock(&g_parties_lock);

        LOG_DEBUG("[PARTY] Party %u disbanded (too few members)", party_id);
        return;
    }

    // If leader left, promote next member
    if (p->leader_id == character_id) {
        for (int i = 0; i < MAX_PARTY_SIZE; i++) {
            if (p->members[i] != 0) {
                p->leader_id = p->members[i];
                LOG_DEBUG("[PARTY] Player %u promoted to leader of party %u", p->leader_id, party_id);
                break;
            }
        }
    }

    pthread_mutex_unlock(&p->lock);

    // Notify the removed player
    PartyDisbandPacket disband = {0};
    disband.header.type = PACKET_PARTY_DISBAND;
    disband.header.player_id = htonl(character_id);
    disband.header.payload_size = htons(sizeof(PartyDisbandPacket) - sizeof(PacketHeader));
    send_to_character(character_id, &disband, sizeof(disband));

    // Notify remaining members
    party_broadcast_update(party_id);
}

/**
 * Disband a party and notify every online member.
 */
void party_disband(uint32_t party_id) {
    pthread_mutex_lock(&g_parties_lock);
    Party* p = party_find(party_id);
    if (!p) {
        pthread_mutex_unlock(&g_parties_lock);
        return;
    }

    pthread_mutex_lock(&p->lock);
    pthread_mutex_unlock(&g_parties_lock);

    // Notify all members and clear their party_id
    PartyDisbandPacket disband = {0};
    disband.header.type = PACKET_PARTY_DISBAND;
    disband.header.payload_size = htons(sizeof(PartyDisbandPacket) - sizeof(PacketHeader));

    for (int i = 0; i < MAX_PARTY_SIZE; i++) {
        if (p->members[i] != 0) {
            ActivePlayer* mp = player_acquire(p->members[i]);
            if (mp) {
                mp->party_id = 0;
                player_release(mp);
            }
            disband.header.player_id = htonl(p->members[i]);
            send_to_character(p->members[i], &disband, sizeof(disband));
        }
    }

    LOG_DEBUG("[PARTY] Party %u disbanded", party_id);

    memset(p->members, 0, sizeof(p->members));
    p->party_id = 0;
    p->leader_id = 0;
    p->member_count = 0;

    pthread_mutex_unlock(&p->lock);
}

/**
 * Remove a disconnected character's invitations and party membership.
 */
void party_handle_disconnect(uint32_t character_id) {
    // Also clean up any pending invites from/to this player
    party_invite_remove(character_id);

    // Remove pending invites sent BY this player
    pthread_mutex_lock(&g_invites_lock);
    for (int i = 0; i < MAX_PENDING_INVITES; i++) {
        if (g_invites[i].from_id == character_id) {
            g_invites[i].from_id = 0;
            g_invites[i].to_id = 0;
        }
    }
    pthread_mutex_unlock(&g_invites_lock);

    party_remove_member(character_id);
}

/**
 * Broadcast current party membership and player statistics to its members.
 */
void party_broadcast_update(uint32_t party_id) {
    pthread_mutex_lock(&g_parties_lock);
    Party* p = party_find(party_id);
    if (!p) {
        pthread_mutex_unlock(&g_parties_lock);
        return;
    }

    pthread_mutex_lock(&p->lock);
    pthread_mutex_unlock(&g_parties_lock);

    PartyUpdatePacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_PARTY_UPDATE;
    pkt.header.payload_size = htons(sizeof(PartyUpdatePacket) - sizeof(PacketHeader));
    pkt.party_id = htonl(p->party_id);
    pkt.leader_id = htonl(p->leader_id);
    pkt.member_count = p->member_count;

    // collect recipients while building member data
    int idx = 0;
    uint32_t member_ids[MAX_PARTY_SIZE];
    int      member_fds[MAX_PARTY_SIZE];
    int member_count = 0;

    for (int i = 0; i < MAX_PARTY_SIZE && idx < MAX_PARTY_SIZE; i++) {
        if (p->members[i] == 0) continue;
        uint32_t mid = p->members[i];

        ActivePlayer* mp = player_acquire(mid);
        if (mp) {
            member_ids[member_count] = mid;
            member_fds[member_count] = mp->client_fd;
            member_count++;
            pkt.members[idx].character_id = htonl(mp->character_id);
            strncpy(pkt.members[idx].name, mp->username, 31);
            pkt.members[idx].level = (uint8_t)mp->level;
            pkt.members[idx].player_class = (uint8_t)mp->player_class;
            pkt.members[idx].health = htonl(mp->health);
            pkt.members[idx].max_health = htonl(mp->max_health);
            pkt.members[idx].mana = htonl(mp->mana);
            pkt.members[idx].max_mana = htonl(mp->max_mana);
            player_release(mp);
        } else {
            member_ids[member_count] = mid;
            member_fds[member_count] = -1;
            member_count++;
            pkt.members[idx].character_id = htonl(mid);
        }
        idx++;
    }

    pthread_mutex_unlock(&p->lock);

    // Send only actual data (base + member_count entries)
    size_t send_size = offsetof(PartyUpdatePacket, members) +
                       pkt.member_count * sizeof(pkt.members[0]);
    pkt.header.payload_size = htons((uint16_t)(send_size - sizeof(PacketHeader)));

    for (int i = 0; i < member_count; i++) {
        if (member_fds[i] < 0) continue;
        pkt.header.player_id = htonl(member_ids[i]);
        server_send(member_fds[i], &pkt, send_size);
    }
}

/**
 * Create a pending invitation after expiring stale entries.
 *
 * @return 1 when stored, or 0 when the target already has an invite or the pool is full.
 */
int party_invite_create(uint32_t from_id, uint32_t to_id, uint32_t party_id) {
    double now = get_time_mono();

    pthread_mutex_lock(&g_invites_lock);

    // Clean expired invites while we're here
    for (int i = 0; i < MAX_PENDING_INVITES; i++) {
        if (g_invites[i].to_id != 0 && (now - g_invites[i].invite_time) > INVITE_EXPIRY_SECONDS) {
            g_invites[i].from_id = 0;
            g_invites[i].to_id = 0;
        }
    }

    // Check if target already has a pending invite
    for (int i = 0; i < MAX_PENDING_INVITES; i++) {
        if (g_invites[i].to_id == to_id) {
            pthread_mutex_unlock(&g_invites_lock);
            return 0; // Already has pending invite
        }
    }

    // Find empty slot
    for (int i = 0; i < MAX_PENDING_INVITES; i++) {
        if (g_invites[i].to_id == 0) {
            g_invites[i].from_id = from_id;
            g_invites[i].to_id = to_id;
            g_invites[i].party_id = party_id;
            g_invites[i].invite_time = now;
            pthread_mutex_unlock(&g_invites_lock);
            return 1;
        }
    }

    pthread_mutex_unlock(&g_invites_lock);
    return 0; // No slots
}

/**
 * Find a non-expired invitation for a target without locking the invite pool.
 *
 * The returned pointer aliases mutable internal storage.
 *
 * @return The invitation, or NULL when absent or expired.
 */
PendingInvite* party_invite_find_for_player(uint32_t to_id) {
    double now = get_time_mono();
    for (int i = 0; i < MAX_PENDING_INVITES; i++) {
        if (g_invites[i].to_id == to_id) {
            if ((now - g_invites[i].invite_time) > INVITE_EXPIRY_SECONDS) {
                g_invites[i].from_id = 0;
                g_invites[i].to_id = 0;
                return NULL; // Expired
            }
            return &g_invites[i];
        }
    }
    return NULL;
}

/**
 * Remove a target's pending invitation.
 */
void party_invite_remove(uint32_t to_id) {
    pthread_mutex_lock(&g_invites_lock);
    for (int i = 0; i < MAX_PENDING_INVITES; i++) {
        if (g_invites[i].to_id == to_id) {
            g_invites[i].from_id = 0;
            g_invites[i].to_id = 0;
            break;
        }
    }
    pthread_mutex_unlock(&g_invites_lock);
}

/**
 * Remove every expired pending invitation.
 */
void party_invite_cleanup_expired(void) {
    double now = get_time_mono();
    pthread_mutex_lock(&g_invites_lock);
    for (int i = 0; i < MAX_PENDING_INVITES; i++) {
        if (g_invites[i].to_id != 0 && (now - g_invites[i].invite_time) > INVITE_EXPIRY_SECONDS) {
            g_invites[i].from_id = 0;
            g_invites[i].to_id = 0;
        }
    }
    pthread_mutex_unlock(&g_invites_lock);
}

/**
 * Split an XP award among living nearby party members or grant it to the killer.
 */
void party_award_xp(uint32_t killer_id, uint64_t xp_amount) {
    if (xp_amount == 0) return;

    ActivePlayer* killer = player_acquire(killer_id);
    if (!killer) return;

    // Check if player is in a party — extract fields, then release
    uint32_t pid = killer->party_id;
    float kx = killer->pos_x;
    float ky = killer->pos_y;
    player_release(killer);

    if (pid == 0) {
        // Not in a party, award full XP to killer
        player_award_xp(killer, xp_amount);
        return;
    }

    // Find party and get nearby members
    pthread_mutex_lock(&g_parties_lock);
    Party* p = party_find(pid);
    if (!p) {
        pthread_mutex_unlock(&g_parties_lock);
        player_award_xp(killer, xp_amount);
        return;
    }

    pthread_mutex_lock(&p->lock);
    pthread_mutex_unlock(&g_parties_lock);

    // Collect nearby party members
    uint32_t nearby[MAX_PARTY_SIZE];
    int nearby_count = 0;
    const float XP_SHARE_RANGE = 800.0f;
    const float range_sq = XP_SHARE_RANGE * XP_SHARE_RANGE;

    for (int i = 0; i < MAX_PARTY_SIZE; i++) {
        if (p->members[i] == 0) continue;

        ActivePlayer* mp = player_acquire(p->members[i]);
        if (!mp) continue;

        if (mp->is_dead) {
            player_release(mp);
            continue;
        }
        float dx = mp->pos_x - kx;
        float dy = mp->pos_y - ky;
        float dist_sq = dx * dx + dy * dy;
        player_release(mp);

        if (dist_sq <= range_sq) {
            nearby[nearby_count++] = p->members[i];
        }
    }

    pthread_mutex_unlock(&p->lock);

    if (nearby_count == 0) {
        // Shouldn't happen since killer should be nearby, but fallback
        player_award_xp(killer, xp_amount);
        return;
    }

    // Split evenly
    uint64_t share = xp_amount / (uint64_t)nearby_count;
    if (share == 0) share = 1;

    for (int i = 0; i < nearby_count; i++) {
        ActivePlayer* mp = player_acquire(nearby[i]);
        if (mp) {
            player_release(mp);
            player_award_xp(mp, share);
        }
    }

    LOG_DEBUG("[PARTY] XP %lu split among %d members (%lu each) in party %u", (unsigned long)xp_amount, nearby_count, (unsigned long)share, pid);
}
