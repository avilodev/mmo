/**
 * @file
 * Manage world-server parties, invitations, membership broadcasts, and shared XP.
 */

#include "party.h"
#include "str_fixed.h"
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

/* --- Pool lookups -------------------------------------------------------- *
 *
 * Both of these hand back a pointer into the pool. They are private to this
 * file and require g_parties_lock to be held across the lookup AND across every
 * use of what they return: party_remove_member() and party_disband() zero a
 * slot's party_id, and a caller holding a stale pointer would then lock and
 * mutate a slot that has since been handed to a different party. They used to
 * be exported, and packet_handler.c called them with no lock at all.
 *
 * Both used to answer by scanning: MAX_PARTIES slots for an identifier, and
 * MAX_PARTIES x MAX_PARTY_SIZE for a member. They are called from the packet
 * path -- every invite, accept, leave, kick and party chat line -- and once per
 * party per broadcast pass, so the cost was paid at the tick rate against the
 * size of the table rather than against the number of parties that exist.
 *
 * Two open-addressed indexes answer them in constant time instead. Both live
 * under g_parties_lock, the same lock the pool itself lives under, so there is
 * no second thing to get wrong: a slot and its index entries are written in the
 * same critical section or not at all.
 */

/** Capacity of both party indexes.
 *
 * A power of two, because the probe walk masks rather than divides, and large
 * enough that the member table -- the fuller of the two, with one entry per
 * character in a party -- stays under half occupancy at MAX_PARTIES full
 * parties. Open addressing degrades sharply past about three-quarters.
 *
 * 4096 covers 200 parties of 5 at 24% occupancy and costs 32 KB for both
 * tables. The static assertions below fail the build rather than the runtime
 * if MAX_PARTIES or MAX_PARTY_SIZE ever outgrow it.
 */
#define PARTY_INDEX_CAP  4096
#define PARTY_INDEX_MASK (PARTY_INDEX_CAP - 1)
_Static_assert((PARTY_INDEX_CAP & PARTY_INDEX_MASK) == 0,
               "the party index capacity must be a power of two");
_Static_assert(PARTY_INDEX_CAP > MAX_PARTIES * MAX_PARTY_SIZE * 2,
               "the member index must stay under half full at full occupancy");

#define PARTY_IDX_EMPTY (-1)

/** One index bucket. slot is PARTY_IDX_EMPTY, or a pool index. */
typedef struct {
    uint32_t key;
    int32_t  slot;
} PartyIndexEntry;

/** party_id -> pool slot. */
static PartyIndexEntry g_party_by_id[PARTY_INDEX_CAP];
/** character_id -> pool slot of the party holding them. */
static PartyIndexEntry g_party_by_member[PARTY_INDEX_CAP];

static inline uint32_t party_index_hash(uint32_t key) {
    key ^= key >> 16;
    key *= 0x7feb352dU;
    key ^= key >> 15;
    key *= 0x846ca68bU;
    key ^= key >> 16;
    return key;
}

static void party_index_reset(PartyIndexEntry* table) {
    for (int i = 0; i < PARTY_INDEX_CAP; i++) {
        table[i].key  = 0;
        table[i].slot = PARTY_IDX_EMPTY;
    }
}

/** Find a key's slot. Caller must hold g_parties_lock. */
static int party_index_find(const PartyIndexEntry* table, uint32_t key) {
    if (key == 0) return -1;
    uint32_t pos = party_index_hash(key) & PARTY_INDEX_MASK;
    for (int probe = 0; probe < PARTY_INDEX_CAP; probe++) {
        const PartyIndexEntry* e = &table[pos];
        if (e->slot == PARTY_IDX_EMPTY) return -1;
        if (e->key == key) return e->slot;
        pos = (pos + 1) & PARTY_INDEX_MASK;
    }
    return -1;
}

/** Bind a key to a slot, replacing any existing binding. Caller holds the lock. */
static void party_index_insert(PartyIndexEntry* table, uint32_t key, int slot) {
    if (key == 0) return;
    uint32_t pos = party_index_hash(key) & PARTY_INDEX_MASK;
    for (int probe = 0; probe < PARTY_INDEX_CAP; probe++) {
        PartyIndexEntry* e = &table[pos];
        if (e->slot == PARTY_IDX_EMPTY || e->key == key) {
            e->key  = key;
            e->slot = slot;
            return;
        }
        pos = (pos + 1) & PARTY_INDEX_MASK;
    }
    /* Unreachable: both tables are sized above their maximum occupancy by the
     * static assertions above. Never corrupt silently if that ever changes. */
    LOG_ERROR("[PARTY] index full inserting key %u", key);
}

/**
 * Unbind a key, closing the probe chain behind it. Caller holds the lock.
 *
 * Backward-shift deletion rather than tombstones: parties form and disband for
 * as long as the server runs, and a table that only ever accumulates
 * tombstones degrades until something rebuilds it.
 */
static void party_index_remove(PartyIndexEntry* table, uint32_t key) {
    if (key == 0) return;

    uint32_t pos = party_index_hash(key) & PARTY_INDEX_MASK;
    int found = -1;
    for (int probe = 0; probe < PARTY_INDEX_CAP; probe++) {
        if (table[pos].slot == PARTY_IDX_EMPTY) return;
        if (table[pos].key == key) { found = (int)pos; break; }
        pos = (pos + 1) & PARTY_INDEX_MASK;
    }
    if (found < 0) return;

    uint32_t hole = (uint32_t)found;
    table[hole].key  = 0;
    table[hole].slot = PARTY_IDX_EMPTY;

    uint32_t scan = (hole + 1) & PARTY_INDEX_MASK;
    while (table[scan].slot != PARTY_IDX_EMPTY) {
        uint32_t home    = party_index_hash(table[scan].key) & PARTY_INDEX_MASK;
        uint32_t to_hole = (scan - hole) & PARTY_INDEX_MASK;
        uint32_t to_home = (scan - home) & PARTY_INDEX_MASK;
        if (to_home >= to_hole) {
            table[hole] = table[scan];
            table[scan].key  = 0;
            table[scan].slot = PARTY_IDX_EMPTY;
            hole = scan;
        }
        scan = (scan + 1) & PARTY_INDEX_MASK;
    }
}

/** Find a party by identifier. Caller must hold g_parties_lock. */
static Party* party_find_locked(uint32_t party_id) {
    int slot = party_index_find(g_party_by_id, party_id);
    if (slot < 0) return NULL;
    /* The index is only ever written under this lock alongside the slot it
     * names, so a stale entry is a bug rather than a race -- verified anyway,
     * because handing back the wrong party would corrupt someone's membership
     * rather than merely fail. */
    if (g_parties[slot].party_id != party_id) {
        LOG_ERROR("[PARTY] index says party %u is in slot %d, which holds %u",
                  party_id, slot, g_parties[slot].party_id);
        return NULL;
    }
    return &g_parties[slot];
}

/** Find a character's party. Caller must hold g_parties_lock. */
static Party* party_find_of_player_locked(uint32_t character_id) {
    int slot = party_index_find(g_party_by_member, character_id);
    if (slot < 0) return NULL;
    if (g_parties[slot].party_id == 0) {
        LOG_ERROR("[PARTY] member index says character %u is in empty slot %d",
                  character_id, slot);
        return NULL;
    }
    return &g_parties[slot];
}

/**
 * Initialize party slots, their mutexes, and pending invitations.
 */
void party_init(void) {
    memset(g_parties, 0, sizeof(g_parties));
    for (int i = 0; i < MAX_PARTIES; i++) {
        pthread_mutex_init(&g_parties[i].lock, NULL);
    }
    party_index_reset(g_party_by_id);
    party_index_reset(g_party_by_member);
    memset(g_invites, 0, sizeof(g_invites));
    LOG_INFO("[PARTY] Party system initialized (%d max parties, %d max size)", MAX_PARTIES, MAX_PARTY_SIZE);
}

/**
 * Empty a party slot and drop everything the indexes said about it.
 *
 * Caller must hold g_parties_lock and p->lock. One function rather than the
 * four open-coded copies this replaced, because the slot and its two index
 * entries have to go together: a slot cleared without its index entries leaves
 * a party id and a set of characters pointing at a slot some other party will
 * be given next.
 */
static void party_clear_slot_locked(Party* p) {
    party_index_remove(g_party_by_id, p->party_id);
    for (int i = 0; i < MAX_PARTY_SIZE; i++) {
        if (p->members[i] != 0) party_index_remove(g_party_by_member, p->members[i]);
    }

    memset(p->members, 0, sizeof(p->members));
    p->party_id     = 0;
    p->leader_id    = 0;
    p->member_count = 0;
}

/** Copy one party's membership. Caller must hold g_parties_lock and p->lock. */
static void party_copy_locked(const Party* p, PartySnapshot* out) {
    out->party_id     = p->party_id;
    out->leader_id    = p->leader_id;
    out->member_count = p->member_count;
    memcpy(out->members, p->members, sizeof(out->members));
}

/** Copy a party by identifier. */
int party_snapshot(uint32_t party_id, PartySnapshot* out) {
    if (!out) return 0;
    memset(out, 0, sizeof(*out));

    pthread_mutex_lock(&g_parties_lock);
    Party* p = party_find_locked(party_id);
    if (!p) {
        pthread_mutex_unlock(&g_parties_lock);
        return 0;
    }
    pthread_mutex_lock(&p->lock);
    party_copy_locked(p, out);
    pthread_mutex_unlock(&p->lock);
    pthread_mutex_unlock(&g_parties_lock);
    return 1;
}

/** Copy the party containing a character. */
int party_snapshot_of_player(uint32_t character_id, PartySnapshot* out) {
    if (!out) return 0;
    memset(out, 0, sizeof(*out));

    pthread_mutex_lock(&g_parties_lock);
    Party* p = party_find_of_player_locked(character_id);
    if (!p) {
        pthread_mutex_unlock(&g_parties_lock);
        return 0;
    }
    pthread_mutex_lock(&p->lock);
    party_copy_locked(p, out);
    pthread_mutex_unlock(&p->lock);
    pthread_mutex_unlock(&g_parties_lock);
    return 1;
}

/** Return the party a character belongs to, or 0. */
uint32_t party_id_of_player(uint32_t character_id) {
    pthread_mutex_lock(&g_parties_lock);
    Party* p = party_find_of_player_locked(character_id);
    uint32_t party_id = p ? p->party_id : 0;
    pthread_mutex_unlock(&g_parties_lock);
    return party_id;
}

/** Report whether a character leads a party. */
int party_is_leader(uint32_t party_id, uint32_t character_id) {
    if (party_id == 0 || character_id == 0) return 0;

    pthread_mutex_lock(&g_parties_lock);
    Party* p = party_find_locked(party_id);
    int is_leader = 0;
    if (p) {
        pthread_mutex_lock(&p->lock);
        is_leader = (p->leader_id == character_id);
        pthread_mutex_unlock(&p->lock);
    }
    pthread_mutex_unlock(&g_parties_lock);
    return is_leader;
}

/** Report whether a character is a member of a party. */
int party_has_member(uint32_t party_id, uint32_t character_id) {
    if (party_id == 0 || character_id == 0) return 0;

    pthread_mutex_lock(&g_parties_lock);
    Party* p = party_find_locked(party_id);
    int found = 0;
    if (p) {
        pthread_mutex_lock(&p->lock);
        for (int i = 0; i < MAX_PARTY_SIZE; i++) {
            if (p->members[i] == character_id) { found = 1; break; }
        }
        pthread_mutex_unlock(&p->lock);
    }
    pthread_mutex_unlock(&g_parties_lock);
    return found;
}

/**
 * Create a party and assign its identifier to the leader.
 *
 * @return The assigned party identifier, or 0 when the pool is full.
 */
uint32_t party_create(uint32_t leader_id) {
    if (leader_id == 0) return 0;

    pthread_mutex_lock(&g_parties_lock);

    /* Refuse a leader who already belongs to a party, under the same lock hold
     * as the slot allocation. Without it, two accepts racing on one inviter
     * create two parties and strand the first. */
    Party* existing = party_find_of_player_locked(leader_id);
    if (existing) {
        uint32_t existing_id = existing->party_id;
        pthread_mutex_unlock(&g_parties_lock);
        LOG_DEBUG("[PARTY] Player %u already leads or belongs to party %u",
                  leader_id, existing_id);
        return 0;
    }

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

    party_index_insert(g_party_by_id, pid, slot);
    party_index_insert(g_party_by_member, leader_id, slot);

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
    if (party_id == 0 || character_id == 0) return 0;

    pthread_mutex_lock(&g_parties_lock);

    /* Decided here, not by the caller.
     *
     * The membership test and the insertion have to be one atomic step. When the
     * caller checked first and called second, two accepts racing for the same
     * character both saw "not in a party" and both inserted -- leaving one
     * character in two parties, with a player->party_id that named only one of
     * them. */
    if (party_find_of_player_locked(character_id)) {
        pthread_mutex_unlock(&g_parties_lock);
        LOG_DEBUG("[PARTY] Player %u is already in a party", character_id);
        return 0;
    }

    Party* p = party_find_locked(party_id);
    if (!p) {
        pthread_mutex_unlock(&g_parties_lock);
        return 0;
    }

    /* g_parties_lock stays held through the mutation, so no other thread can
     * disband this slot between the lookup and the insert. It is released before
     * player_acquire(), because a slot mutex must never be taken beneath it. */
    pthread_mutex_lock(&p->lock);

    int added = 0;
    uint8_t new_count = 0;
    if (p->member_count < MAX_PARTY_SIZE) {
        for (int i = 0; i < MAX_PARTY_SIZE; i++) {
            if (p->members[i] == 0) {
                p->members[i] = character_id;
                p->member_count++;
                new_count = p->member_count;
                added = 1;
                party_index_insert(g_party_by_member, character_id,
                                   (int)(p - g_parties));
                break;
            }
        }
    }

    pthread_mutex_unlock(&p->lock);
    pthread_mutex_unlock(&g_parties_lock);

    if (!added) return 0;

    // Set party_id on the player
    ActivePlayer* player = player_acquire(character_id);
    if (player) {
        player->party_id = party_id;
        player_release(player);
    }

    LOG_DEBUG("[PARTY] Player %u joined party %u (%u members)",
              character_id, party_id, new_count);
    return 1;
}

/**
 * Remove a character and promote or disband the party as required.
 */
void party_remove_member(uint32_t character_id) {
    pthread_mutex_lock(&g_parties_lock);
    Party* p = party_find_of_player_locked(character_id);
    if (!p) {
        pthread_mutex_unlock(&g_parties_lock);
        return;
    }

    uint32_t party_id = p->party_id;
    pthread_mutex_lock(&p->lock);

    /* The members array and the member index are written in the same critical
     * section: g_parties_lock is what orders them, so it is held across both
     * rather than released the moment the slot lock is taken. It is still
     * released before player_acquire() below -- a slot mutex must never be
     * taken beneath it. */
    for (int i = 0; i < MAX_PARTY_SIZE; i++) {
        if (p->members[i] == character_id) {
            p->members[i] = 0;
            p->member_count--;
            party_index_remove(g_party_by_member, character_id);
            break;
        }
    }

    pthread_mutex_unlock(&g_parties_lock);

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
        Party* pp = party_find_locked(party_id);
        if (pp) {
            pthread_mutex_lock(&pp->lock);
            party_clear_slot_locked(pp);
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
    Party* p = party_find_locked(party_id);
    if (!p) {
        pthread_mutex_unlock(&g_parties_lock);
        return;
    }

    /* g_parties_lock is released for the notification pass -- it calls
     * player_acquire(), and a slot mutex must never be taken beneath it -- and
     * retaken for the clear, because emptying the slot also empties its index
     * entries and the two must move together. p->lock is held throughout, so
     * nothing else can reuse the slot in between. */
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

    pthread_mutex_lock(&g_parties_lock);
    party_clear_slot_locked(p);
    pthread_mutex_unlock(&g_parties_lock);

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
    Party* p = party_find_locked(party_id);
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
            STR_COPY_FIELD(pkt.members[idx].name, mp->username);
            pkt.members[idx].level = (uint8_t)mp->level;
            pkt.members[idx].player_class = (uint8_t)mp->race_id;
            pkt.members[idx].health = htonl(mp->health);
            pkt.members[idx].max_health = htonl(mp->max_health);
            pkt.members[idx].mana = htonl(mp->resource);
            pkt.members[idx].max_mana = htonl(mp->max_resource);
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
 * Copy a target's pending, unexpired invitation.
 *
 * Returns a copy under g_invites_lock rather than a pointer into the pool. The
 * pool entry is cleared by party_invite_remove() and by expiry, so a caller
 * reading through a returned pointer was reading a slot another thread was free
 * to reuse -- and this function also mutated the pool with no lock held while
 * expiring entries.
 *
 * @return 1 when an unexpired invitation was copied, otherwise 0.
 */
int party_invite_find_for_player(uint32_t to_id, PendingInvite* out) {
    if (!out || to_id == 0) return 0;

    double now = get_time_mono();
    int found = 0;

    pthread_mutex_lock(&g_invites_lock);
    for (int i = 0; i < MAX_PENDING_INVITES; i++) {
        if (g_invites[i].to_id != to_id) continue;

        if ((now - g_invites[i].invite_time) > INVITE_EXPIRY_SECONDS) {
            g_invites[i].from_id = 0;
            g_invites[i].to_id = 0;
            break;   // expired
        }

        *out = g_invites[i];
        found = 1;
        break;
    }
    pthread_mutex_unlock(&g_invites_lock);
    return found;
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
 * Award XP to one character, resolved by identifier.
 *
 * Every award goes through this rather than through a previously acquired
 * ActivePlayer*. player_release() ends the caller's claim on the slot, and the
 * slot is reused by the next login, so re-locking a released pointer can credit
 * the XP to whoever holds that slot now.
 */
static void award_xp_to(uint32_t character_id, uint64_t xp_amount) {
    ActivePlayer* p = player_acquire(character_id);
    if (!p) return;
    player_award_xp_locked(p, xp_amount);
    player_release(p);
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
        award_xp_to(killer_id, xp_amount);
        return;
    }

    /* Snapshot the membership, then work from the copy.
     *
     * Holding the party's mutex across a run of player_acquire() calls made the
     * party pool an ordering dependency of every player slot it contains, on a
     * path that runs on every kill. The copy costs one small memcpy and leaves
     * the two locks unrelated. */
    PartySnapshot party;
    if (!party_snapshot(pid, &party)) {
        award_xp_to(killer_id, xp_amount);
        return;
    }

    // Collect nearby living party members
    uint32_t nearby[MAX_PARTY_SIZE];
    int nearby_count = 0;
    const float XP_SHARE_RANGE = 800.0f;
    const float range_sq = XP_SHARE_RANGE * XP_SHARE_RANGE;

    for (int i = 0; i < MAX_PARTY_SIZE; i++) {
        if (party.members[i] == 0) continue;

        ActivePlayer* mp = player_acquire(party.members[i]);
        if (!mp) continue;

        int is_dead = mp->is_dead;
        float dx = mp->pos_x - kx;
        float dy = mp->pos_y - ky;
        player_release(mp);

        if (is_dead) continue;
        if (dx * dx + dy * dy <= range_sq) {
            nearby[nearby_count++] = party.members[i];
        }
    }

    if (nearby_count == 0) {
        // Shouldn't happen since killer should be nearby, but fallback
        award_xp_to(killer_id, xp_amount);
        return;
    }

    // Split evenly
    uint64_t share = xp_amount / (uint64_t)nearby_count;
    if (share == 0) share = 1;

    for (int i = 0; i < nearby_count; i++)
        award_xp_to(nearby[i], share);

    LOG_DEBUG("[PARTY] XP %lu split among %d members (%lu each) in party %u",
              (unsigned long)xp_amount, nearby_count, (unsigned long)share, pid);
}
