#include "party.h"
#include "player_level.h"

#include <string.h>
#include <stdio.h>
#include <math.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <time.h>

static Party g_parties[MAX_PARTIES];
static pthread_mutex_t g_parties_lock = PTHREAD_MUTEX_INITIALIZER;

static PendingInvite g_invites[MAX_PENDING_INVITES];
static pthread_mutex_t g_invites_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t g_next_party_id = 1;

// ============================================================================
// Internal helpers
// ============================================================================

static double get_time_mono(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void send_to_character(uint32_t character_id, void* packet, size_t size) {
    ActivePlayer* p = player_find_active(character_id);
    if (!p) return;
    pthread_mutex_lock(&p->lock);
    int fd = p->client_fd;
    pthread_mutex_unlock(&p->lock);
    if (fd > 0) send(fd, packet, size, 0);
}

// ============================================================================
// Party init
// ============================================================================

void party_init(void) {
    memset(g_parties, 0, sizeof(g_parties));
    for (int i = 0; i < MAX_PARTIES; i++) {
        pthread_mutex_init(&g_parties[i].lock, NULL);
    }
    memset(g_invites, 0, sizeof(g_invites));
    printf("[PARTY] Party system initialized (%d max parties, %d max size)\n",
           MAX_PARTIES, MAX_PARTY_SIZE);
}

// ============================================================================
// Party find
// ============================================================================

Party* party_find(uint32_t party_id) {
    if (party_id == 0) return NULL;
    for (int i = 0; i < MAX_PARTIES; i++) {
        if (g_parties[i].party_id == party_id) {
            return &g_parties[i];
        }
    }
    return NULL;
}

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

// ============================================================================
// Party create / add / remove
// ============================================================================

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
        printf("[PARTY] Cannot create party: all slots full\n");
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
    ActivePlayer* player = player_find_active(leader_id);
    if (player) {
        pthread_mutex_lock(&player->lock);
        player->party_id = pid;
        pthread_mutex_unlock(&player->lock);
    }

    printf("[PARTY] Party %u created by player %u\n", pid, leader_id);
    return pid;
}

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
            ActivePlayer* player = player_find_active(character_id);
            if (player) {
                pthread_mutex_lock(&player->lock);
                player->party_id = party_id;
                pthread_mutex_unlock(&player->lock);
            }

            printf("[PARTY] Player %u joined party %u (%u members)\n",
                   character_id, party_id, p->member_count);
            return 1;
        }
    }

    pthread_mutex_unlock(&p->lock);
    return 0;
}

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
    ActivePlayer* player = player_find_active(character_id);
    if (player) {
        pthread_mutex_lock(&player->lock);
        player->party_id = 0;
        pthread_mutex_unlock(&player->lock);
    }

    printf("[PARTY] Player %u left party %u (%u members remaining)\n",
           character_id, party_id, p->member_count);

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
            ActivePlayer* lp = player_find_active(last_member);
            if (lp) {
                pthread_mutex_lock(&lp->lock);
                lp->party_id = 0;
                pthread_mutex_unlock(&lp->lock);
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

        printf("[PARTY] Party %u disbanded (too few members)\n", party_id);
        return;
    }

    // If leader left, promote next member
    if (p->leader_id == character_id) {
        for (int i = 0; i < MAX_PARTY_SIZE; i++) {
            if (p->members[i] != 0) {
                p->leader_id = p->members[i];
                printf("[PARTY] Player %u promoted to leader of party %u\n",
                       p->leader_id, party_id);
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
            ActivePlayer* mp = player_find_active(p->members[i]);
            if (mp) {
                pthread_mutex_lock(&mp->lock);
                mp->party_id = 0;
                pthread_mutex_unlock(&mp->lock);
            }
            disband.header.player_id = htonl(p->members[i]);
            send_to_character(p->members[i], &disband, sizeof(disband));
        }
    }

    printf("[PARTY] Party %u disbanded\n", party_id);

    memset(p->members, 0, sizeof(p->members));
    p->party_id = 0;
    p->leader_id = 0;
    p->member_count = 0;

    pthread_mutex_unlock(&p->lock);
}

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

// ============================================================================
// Party broadcast
// ============================================================================

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

    // Build member list
    int idx = 0;
    uint32_t member_ids[MAX_PARTY_SIZE];
    int member_count = 0;

    for (int i = 0; i < MAX_PARTY_SIZE && idx < MAX_PARTY_SIZE; i++) {
        if (p->members[i] == 0) continue;
        uint32_t mid = p->members[i];
        member_ids[member_count++] = mid;

        ActivePlayer* mp = player_find_active(mid);
        if (mp) {
            pthread_mutex_lock(&mp->lock);
            pkt.members[idx].character_id = htonl(mp->character_id);
            strncpy(pkt.members[idx].name, mp->username, 31);
            pkt.members[idx].level = (uint8_t)mp->level;
            pkt.members[idx].player_class = (uint8_t)mp->player_class;
            pkt.members[idx].health = htonl(mp->health);
            pkt.members[idx].max_health = htonl(mp->max_health);
            pkt.members[idx].mana = htonl(mp->mana);
            pkt.members[idx].max_mana = htonl(mp->max_mana);
            pthread_mutex_unlock(&mp->lock);
        } else {
            pkt.members[idx].character_id = htonl(mid);
        }
        idx++;
    }

    pthread_mutex_unlock(&p->lock);

    // Send to all members
    for (int i = 0; i < member_count; i++) {
        pkt.header.player_id = htonl(member_ids[i]);
        send_to_character(member_ids[i], &pkt, sizeof(pkt));
    }
}

// ============================================================================
// Invite system
// ============================================================================

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

// ============================================================================
// XP sharing
// ============================================================================

void party_award_xp(uint32_t killer_id, uint64_t xp_amount) {
    if (xp_amount == 0) return;

    ActivePlayer* killer = player_find_active(killer_id);
    if (!killer) return;

    // Check if player is in a party
    pthread_mutex_lock(&killer->lock);
    uint32_t pid = killer->party_id;
    float kx = killer->pos_x;
    float ky = killer->pos_y;
    pthread_mutex_unlock(&killer->lock);

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

        ActivePlayer* mp = player_find_active(p->members[i]);
        if (!mp) continue;

        pthread_mutex_lock(&mp->lock);
        if (mp->is_dead) {
            pthread_mutex_unlock(&mp->lock);
            continue;
        }
        float dx = mp->pos_x - kx;
        float dy = mp->pos_y - ky;
        float dist_sq = dx * dx + dy * dy;
        pthread_mutex_unlock(&mp->lock);

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
        ActivePlayer* mp = player_find_active(nearby[i]);
        if (mp) {
            player_award_xp(mp, share);
        }
    }

    printf("[PARTY] XP %lu split among %d members (%lu each) in party %u\n",
           (unsigned long)xp_amount, nearby_count, (unsigned long)share, pid);
}
