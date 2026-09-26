/**
 * @file
 * Check the world half of friends: the reverse index, and what a client packet
 * turns into.
 *
 * The world owns none of the friend graph. What it owns is the translation in
 * both directions -- a client packet into a mutation on the queue, and a change
 * from anywhere into a packet for the right local session -- plus the index
 * that makes the second direction a hash lookup rather than a scan of the
 * player pool. Those are what this pins down.
 *
 * The index and packet-parsing cases need no Redis and always run. The cases
 * that involve the caches or the bus are skipped when there is no reachable
 * Redis, the same way presence_test does it.
 */

#include "types.h"
#include "log.h"
#include "friends.h"
#include "friend_bus.h"
#include "presence.h"
#include "protocol.h"
#include "player_data.h"
#include "players_database.h"
#include "ability_def.h"
#include "quest_system.h"
#include "session.h"
#include "utils.h"

#include <arpa/inet.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int g_failures = 0;

#define CHECK(cond, what)                                                   \
    do {                                                                    \
        if (cond) {                                                         \
            printf("  ok   %s\n", (what));                                  \
        } else {                                                            \
            printf("  FAIL %s  (%s:%d)\n", (what), __FILE__, __LINE__);     \
            g_failures++;                                                   \
        }                                                                   \
    } while (0)

/** The world this suite pretends to be. Kept away from the real 1..10 so a
 *  developer running it against a live Redis cannot disturb a running world. */
#define TEST_WORLD 7

/* --- Stubs ---------------------------------------------------------------
 *
 * player_data.c is linked for real, because the index and the send path both
 * go through the player pool and stubbing it would test nothing. Everything it
 * reaches past that is stubbed, exactly as party_test does.
 */

/** Stub successful database initialization. */
int character_database_init(const char* c) { (void)c; return 1; }
/** Stub database shutdown. */
void character_database_close(void) {}
/** Stub successful character persistence. */
int character_update_full_data(const CharacterInfo* d) { (void)d; return 1; }
/** Stub loading empty inventory and equipment arrays. */
int character_items_load(uint32_t c, ItemInstance* inv, int n_inv,
                        ItemInstance* eq, int n_eq) {
    (void)c;
    if (inv) memset(inv, 0, sizeof(*inv) * (size_t)n_inv);
    if (eq) memset(eq, 0, sizeof(*eq) * (size_t)n_eq);
    return 1;
}
/** Stub successful inventory persistence. */
int character_items_save(uint32_t c, const ItemInstance* inv, int n_inv,
                        const ItemInstance* eq, int n_eq) {
    (void)c; (void)inv; (void)n_inv; (void)eq; (void)n_eq; return 1;
}
/** Stub the whole-character save that commits scalars, currency and items together. */
int character_save_all(const CharacterInfo* d, const ItemInstance* inv, int n_inv,
                       const ItemInstance* eq, int n_eq) {
    (void)d; (void)inv; (void)n_inv; (void)eq; (void)n_eq; return 1;
}
/* world_session_mark() and world_session_clear() are NOT stubbed: session.c is
 * linked for real here because presence needs its Redis pool, and it defines
 * them. */
/** Stub an empty persisted instance-id range. */
uint64_t character_items_max_instance_id(void) { return 0; }
/** Stub the checked form of the same query. */
int character_items_max_instance_id_checked(uint64_t* out) {
    if (out) *out = 0;
    return 1;
}
/** Stub a populated level-one character record. */
int character_get_full_data(uint32_t character_id, CharacterInfo* out) {
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    out->character_id = character_id;
    out->level = 1;
    snprintf(out->name, sizeof(out->name), "Char%u", character_id);
    return 1;
}
/** Stub class-stat application. */
void player_apply_class_stats(ActivePlayer* p) { (void)p; }
/** Stub equipment-stat application. */
void player_apply_equipment_bonuses(ActivePlayer* p) { (void)p; }
/** Stub an empty hotbar for both forms. */
int ability_get_form_abilities(uint8_t race, uint8_t form, uint16_t* out, int max) {
    (void)race; (void)form; (void)out; (void)max; return 0;
}
/** Stub the hotbar rebuild. */
void ability_refresh_hotbars(ActivePlayer* p) { (void)p; }
/** Stub the stat recompute. */
void player_recompute_stats(ActivePlayer* p) { (void)p; }
/** Stub an absent ability definition. */
const AbilityDef* ability_get(uint16_t id) { (void)id; return NULL; }
/** Stub an empty persisted quest list. */
int quest_player_load(uint32_t c, PlayerQuestState* s) { (void)c; (void)s; return 0; }
/** Stub successful quest persistence. */
int quest_player_save(uint32_t c, const PlayerQuestState* s) { (void)c; (void)s; return 1; }
/** Stub quest storage teardown. */
void quest_state_release(PlayerQuestState* s) { (void)s; }
/** Stub the snapshot's deep copy. */
int quest_state_copy(PlayerQuestState* out, const PlayerQuestState* src) {
    (void)out; (void)src; return 1;
}
/** Stub the completion set. */
int quest_history_add(QuestHistory* h, uint32_t id) { (void)h; (void)id; return 1; }
/** Stub the party XP split. */
void player_award_xp_locked(ActivePlayer* p, uint64_t amount) {
    if (p) p->experience += amount;
}

/* --- The captured outbox -------------------------------------------------
 *
 * server_send() is where every answer this module produces ends up, so it is
 * captured rather than performed. The bus thread writes here too, hence the
 * lock: a torn read of a captured packet would look like a protocol bug.
 */

#define OUTBOX_MAX 32

typedef struct {
    int     fd;
    size_t  size;
    uint8_t bytes[MAX_PACKET_SIZE];
} SentPacket;

static SentPacket      g_outbox[OUTBOX_MAX];
static int             g_outbox_count = 0;
static pthread_mutex_t g_outbox_lock = PTHREAD_MUTEX_INITIALIZER;

ssize_t server_send(int fd, void* buf, size_t len) {
    pthread_mutex_lock(&g_outbox_lock);
    if (g_outbox_count < OUTBOX_MAX && len <= sizeof(g_outbox[0].bytes)) {
        SentPacket* s = &g_outbox[g_outbox_count++];
        s->fd = fd;
        s->size = len;
        memcpy(s->bytes, buf, len);
    }
    pthread_mutex_unlock(&g_outbox_lock);
    return (ssize_t)len;
}

static void outbox_clear(void) {
    pthread_mutex_lock(&g_outbox_lock);
    g_outbox_count = 0;
    pthread_mutex_unlock(&g_outbox_lock);
}

/** Copy out the first captured packet of one type. @return 1 when found. */
static int outbox_find(uint8_t type, SentPacket* out) {
    int found = 0;
    pthread_mutex_lock(&g_outbox_lock);
    for (int i = 0; i < g_outbox_count; i++) {
        if (g_outbox[i].bytes[0] == type) {
            *out = g_outbox[i];
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&g_outbox_lock);
    return found;
}

/** Wait up to `ms` for a packet of one type to be captured. */
static int outbox_wait(uint8_t type, SentPacket* out, int ms) {
    for (int waited = 0; waited < ms; waited += 20) {
        if (outbox_find(type, out)) return 1;
        usleep(20 * 1000);
    }
    return 0;
}

/* --- Helpers ------------------------------------------------------------- */

/** Whether a Redis is actually reachable, as opposed to merely configured. */
static int redis_ready(void) {
    return session_is_ready();
}


/** Bring a character online and give it an account, which friends needs. */
static void bring_online(uint32_t character_id, uint32_t account_id) {
    int slot = -1;
    player_add_active(character_id, (int)(100 + character_id), &slot);

    ActivePlayer* p = player_acquire(character_id);
    if (p) {
        p->account_id = account_id;
        player_release(p);
    }
}

/** Drain everything sitting on the mutation queue. @return how many were read. */
static int drain_queue(FriendMutation* out, int max) {
    int n = 0;
    while (n < max && friend_mutation_pop(&out[n], 1)) n++;
    return n;
}

/* --- Cases that need nothing but the index ------------------------------- */

static void test_a_short_packet_is_ignored(void) {
    printf("packets: a truncated packet queues nothing\n");

    /* Long enough to have a header and a type, too short to have a name. The
     * handler must reject it on size rather than read past the end. */
    uint8_t buf[sizeof(PacketHeader) + 2] = {0};
    ((PacketHeader*)buf)->type = PACKET_FRIEND_REQUEST;

    FriendMutation drop[4];
    drain_queue(drop, 4);

    world_friends_handle_packet(5, 5001, buf, (ssize_t)sizeof(buf));

    FriendMutation m;
    CHECK(friend_mutation_pop(&m, 1) == 0, "nothing reached the queue");
}

static void test_a_packet_from_no_session_is_ignored(void) {
    printf("packets: a packet from a character with no slot queues nothing\n");

    FriendRequestPacket pkt = {0};
    pkt.header.type = PACKET_FRIEND_REQUEST;
    snprintf(pkt.target_name, sizeof(pkt.target_name), "%s", "Somebody");

    /* Character 9999 is not online, so there is no slot to read an account
     * from. Forwarding it anyway would put an unattributable mutation on a
     * queue the realm acts on without further checks. */
    world_friends_handle_packet(5, 9999, (const uint8_t*)&pkt, (ssize_t)sizeof(pkt));

    FriendMutation m;
    CHECK(friend_mutation_pop(&m, 1) == 0, "nothing reached the queue");
}

/* --- Cases that need Redis ----------------------------------------------- */

static void test_a_request_becomes_a_mutation(void) {
    printf("redis: an add-friend packet becomes one queued mutation\n");

    FriendMutation drop[8];
    drain_queue(drop, 8);

    FriendRequestPacket pkt = {0};
    pkt.header.type = PACKET_FRIEND_REQUEST;
    snprintf(pkt.target_name, sizeof(pkt.target_name), "%s", "Thornwood");

    world_friends_handle_packet(5, 5001, (const uint8_t*)&pkt, (ssize_t)sizeof(pkt));

    FriendMutation m;
    CHECK(friend_mutation_pop(&m, 2) == 1, "one mutation was queued");
    CHECK(m.op == FRIEND_OP_REQUEST, "as a request");
    CHECK(m.actor_account == 900001, "naming who asked");
    CHECK(m.actor_character_id == 5001, "and which of their characters");
    CHECK(m.origin_world_id == TEST_WORLD, "and where to answer");
    CHECK(strcmp(m.target_name, "Thornwood") == 0, "carrying the typed name");
}

static void test_accept_and_decline_are_different_operations(void) {
    printf("redis: accept and decline do not collapse into one operation\n");

    FriendMutation drop[8];
    drain_queue(drop, 8);

    FriendRespondPacket pkt = {0};
    pkt.header.type = PACKET_FRIEND_RESPOND;
    snprintf(pkt.from_name, sizeof(pkt.from_name), "%s", "Thornwood");

    pkt.accept = 1;
    world_friends_handle_packet(5, 5001, (const uint8_t*)&pkt, (ssize_t)sizeof(pkt));

    FriendMutation m;
    CHECK(friend_mutation_pop(&m, 2) == 1, "the accept is queued");
    CHECK(m.op == FRIEND_OP_ACCEPT, "as an accept");

    pkt.accept = 0;
    world_friends_handle_packet(5, 5001, (const uint8_t*)&pkt, (ssize_t)sizeof(pkt));

    CHECK(friend_mutation_pop(&m, 2) == 1, "the decline is queued");
    CHECK(m.op == FRIEND_OP_DECLINE, "as a decline");
}

static void test_block_and_unblock_are_different_operations(void) {
    printf("redis: block and unblock do not collapse into one operation\n");

    FriendMutation drop[8];
    drain_queue(drop, 8);

    FriendBlockPacket pkt = {0};
    pkt.header.type = PACKET_FRIEND_BLOCK;
    snprintf(pkt.target_name, sizeof(pkt.target_name), "%s", "Thornwood");

    pkt.block = 1;
    world_friends_handle_packet(5, 5001, (const uint8_t*)&pkt, (ssize_t)sizeof(pkt));

    FriendMutation m;
    CHECK(friend_mutation_pop(&m, 2) == 1, "the block is queued");
    CHECK(m.op == FRIEND_OP_BLOCK, "as a block");

    pkt.block = 0;
    world_friends_handle_packet(5, 5001, (const uint8_t*)&pkt, (ssize_t)sizeof(pkt));

    CHECK(friend_mutation_pop(&m, 2) == 1, "the unblock is queued");
    CHECK(m.op == FRIEND_OP_UNBLOCK, "as an unblock");
}

static void test_an_unterminated_wire_name_is_bounded(void) {
    printf("redis: a name field with no terminator does not run off the end\n");

    FriendMutation drop[8];
    drain_queue(drop, 8);

    /* Every byte of the field filled, which is what a hostile client sends.
     * Nothing off the wire is terminated, and the very next thing that happens
     * to this name is an snprintf("%s"). */
    FriendRequestPacket pkt = {0};
    pkt.header.type = PACKET_FRIEND_REQUEST;
    memset(pkt.target_name, 'A', sizeof(pkt.target_name));

    world_friends_handle_packet(5, 5001, (const uint8_t*)&pkt, (ssize_t)sizeof(pkt));

    FriendMutation m;
    CHECK(friend_mutation_pop(&m, 2) == 1, "it is still forwarded");
    CHECK(strlen(m.target_name) < sizeof(m.target_name), "with a terminated name");
    CHECK(strlen(m.target_name) > 0, "that is not empty");
    for (size_t i = 0; i < strlen(m.target_name); i++) {
        if (m.target_name[i] != 'A') {
            CHECK(0, "made only of what was sent");
            return;
        }
    }
    CHECK(1, "made only of what was sent");
}

static void test_a_cold_cache_asks_rather_than_answering_empty(void) {
    printf("redis: opening the panel with a cold cache asks the realm\n");

    const uint32_t account = 900010;
    const uint32_t character = 5010;

    presence_friends_drop(account);
    presence_requests_drop(account);

    FriendMutation drop[8];
    drain_queue(drop, 8);
    outbox_clear();

    bring_online(character, account);

    PacketHeader req = {0};
    req.type = PACKET_FRIEND_LIST_REQUEST;
    world_friends_handle_packet(5, character, (const uint8_t*)&req, (ssize_t)sizeof(req));

    FriendMutation m;
    CHECK(friend_mutation_pop(&m, 2) == 1, "a rebuild is queued");
    CHECK(m.op == FRIEND_OP_LIST, "as a list rebuild");
    CHECK(m.actor_account == account, "for the asking account");

    /* The important half: an empty list would render as "you have no friends"
     * rather than as "still loading", which is a lie the player acts on. */
    SentPacket sent;
    CHECK(outbox_find(PACKET_FRIEND_LIST_RESPONSE, &sent) == 0,
          "and no empty list was sent in the meantime");

    player_remove_active(character);
}

static void test_a_warm_cache_answers_without_the_realm(void) {
    printf("redis: opening the panel with a warm cache costs the realm nothing\n");

    const uint32_t account = 900011;
    const uint32_t character = 5011;
    const FriendCacheEntry friends[2] = { { 900012, "Ashvale" },
                                          { 900013, "Brightmoor" } };

    presence_friends_store(account, friends, 2);
    presence_requests_store(account, NULL, 0);

    /* One of the two is online somewhere, the other is not. */
    presence_clear(friends[0].account_id);
    presence_clear(friends[1].account_id);
    presence_set(friends[0].account_id, 3, 6001, "Ashvale");

    FriendMutation drop[8];
    drain_queue(drop, 8);
    outbox_clear();

    bring_online(character, account);

    PacketHeader req = {0};
    req.type = PACKET_FRIEND_LIST_REQUEST;
    world_friends_handle_packet(5, character, (const uint8_t*)&req, (ssize_t)sizeof(req));

    SentPacket sent;
    CHECK(outbox_find(PACKET_FRIEND_LIST_RESPONSE, &sent) == 1, "the list was sent");

    FriendListResponsePacket* list = (FriendListResponsePacket*)sent.bytes;
    CHECK(list->count == 2, "with both friends");

    /* Only the entries used are on the wire, not five kilobytes of zeroes. */
    size_t expect = offsetof(FriendListResponsePacket, friends) + 2 * sizeof(FriendWireEntry);
    CHECK(sent.size == expect, "trimmed to the entries actually used");

    int saw_online = 0, saw_offline = 0, offline_named = 0, offline_worldless = 0;
    for (int i = 0; i < list->count; i++) {
        uint32_t id = ntohl(list->friends[i].account_id);
        if (id == friends[0].account_id) {
            saw_online = list->friends[i].online == 1
                      && ntohl(list->friends[i].world_id) == 3
                      && strcmp(list->friends[i].name, "Ashvale") == 0;
        } else if (id == friends[1].account_id) {
            saw_offline    = list->friends[i].online == 0;
            /* Named from the cache, not from presence: an offline account has
             * no presence record, and a row the panel cannot label is a row
             * nobody can act on. */
            offline_named  = strcmp(list->friends[i].name, "Brightmoor") == 0;
            /* And nothing else. There is no last world to report. */
            offline_worldless = list->friends[i].world_id == 0;
        }
    }
    CHECK(saw_online, "the online one carries their world and name");
    CHECK(saw_offline, "and the offline one is simply offline");
    CHECK(offline_named, "still named, from the cache rather than from presence");
    CHECK(offline_worldless, "and carrying no world at all");

    FriendMutation m;
    CHECK(friend_mutation_pop(&m, 1) == 0, "and the realm was not asked anything");

    player_remove_active(character);
    presence_clear(friends[0].account_id);
}

/* A friend playing a character they were not last seen on shows as the one
 * they are actually on. The cached name is a fallback for when there is no
 * live one, not a second opinion about a friend who is right there. */
static void test_a_live_character_name_wins_over_the_cached_one(void) {
    printf("redis: an online friend is shown as the character they are on now\n");

    const uint32_t account = 900015, character = 5015;
    const FriendCacheEntry friends[1] = { { 900016, "OldCharacter" } };

    presence_friends_store(account, friends, 1);
    presence_requests_store(account, NULL, 0);

    presence_clear(friends[0].account_id);
    presence_set(friends[0].account_id, 5, 6002, "NewCharacter");

    outbox_clear();
    bring_online(character, account);

    PacketHeader req = {0};
    req.type = PACKET_FRIEND_LIST_REQUEST;
    world_friends_handle_packet(5, character, (const uint8_t*)&req, (ssize_t)sizeof(req));

    SentPacket sent;
    CHECK(outbox_find(PACKET_FRIEND_LIST_RESPONSE, &sent) == 1, "the list was sent");

    FriendListResponsePacket* list = (FriendListResponsePacket*)sent.bytes;
    CHECK(list->count == 1, "with the one friend");
    CHECK(strcmp(list->friends[0].name, "NewCharacter") == 0,
          "named as the character they are playing");
    CHECK(ntohl(list->friends[0].world_id) == 5, "on the world they are on");

    presence_clear(friends[0].account_id);
    player_remove_active(character);
}

static void test_entering_inverts_the_friend_set(void) {
    printf("redis: entering the world registers one watch per friend\n");

    const uint32_t account = 900020;
    const uint32_t character = 5020;
    const FriendCacheEntry friends[3] = { { 900021, "One" },
                                          { 900022, "Two" },
                                          { 900023, "Three" } };

    presence_friends_store(account, friends, 3);
    presence_requests_store(account, NULL, 0);

    int before = world_friends_watch_count();
    bring_online(character, account);
    world_friends_player_entered(account, character, "Watcher");

    CHECK(world_friends_watch_count() == before + 3, "three watches appear");

    world_friends_player_left(account, character);
    CHECK(world_friends_watch_count() == before, "and leaving removes exactly those");

    player_remove_active(character);
}

static void test_leaving_spares_another_players_watches(void) {
    printf("redis: one player leaving does not unwatch another player's friends\n");

    const uint32_t a_account = 900030, a_char = 5030;
    const uint32_t b_account = 900031, b_char = 5031;

    /* Deliberately overlapping: both watch the same two accounts, which is the
     * case a naive "remove every entry for this account" would break. */
    const FriendCacheEntry shared[2] = { { 900032, "Shared1" },
                                         { 900033, "Shared2" } };
    presence_friends_store(a_account, shared, 2);
    presence_friends_store(b_account, shared, 2);
    presence_requests_store(a_account, NULL, 0);
    presence_requests_store(b_account, NULL, 0);

    int before = world_friends_watch_count();

    bring_online(a_char, a_account);
    bring_online(b_char, b_account);
    world_friends_player_entered(a_account, a_char, "Ayla");
    world_friends_player_entered(b_account, b_char, "Bryn");
    CHECK(world_friends_watch_count() == before + 4, "four watches for two players");

    world_friends_player_left(a_account, a_char);
    CHECK(world_friends_watch_count() == before + 2, "only the leaver's are removed");

    world_friends_player_left(b_account, b_char);
    CHECK(world_friends_watch_count() == before, "and then the rest");

    player_remove_active(a_char);
    player_remove_active(b_char);
}

static void test_a_watched_friend_coming_online_reaches_the_watcher(void) {
    printf("redis: a friend logging in anywhere reaches the watcher here\n");

    const uint32_t account = 900040, character = 5040;
    const uint32_t friend_account = 900041;

    const FriendCacheEntry one = { friend_account, "Faraway" };
    presence_friends_store(account, &one, 1);
    presence_requests_store(account, NULL, 0);

    bring_online(character, account);
    world_friends_player_entered(account, character, "Watcher");
    outbox_clear();

    /* Published as if from another world entirely -- which is the whole point:
     * this process learns about it from the shared channel, not from its own
     * player pool. */
    presence_set(friend_account, 9, 7001, "Faraway");

    SentPacket sent;
    CHECK(outbox_wait(PACKET_FRIEND_PRESENCE_UPDATE, &sent, 2000) == 1,
          "the watcher is told");

    FriendPresenceUpdatePacket* upd = (FriendPresenceUpdatePacket*)sent.bytes;
    CHECK(ntohl(upd->account_id) == friend_account, "about the right account");
    CHECK(upd->online == 1, "who is online");
    CHECK(ntohl(upd->world_id) == 9, "on the world they are actually on");
    CHECK(strcmp(upd->name, "Faraway") == 0, "under the name they are playing");
    CHECK(sent.fd == (int)(100 + character), "and it went to the watcher's socket");

    presence_clear(friend_account);
    world_friends_player_left(account, character);
    player_remove_active(character);
}

static void test_an_unwatched_change_reaches_nobody(void) {
    printf("redis: a stranger's presence change is dropped, not broadcast\n");

    const uint32_t account = 900050, character = 5050;
    const uint32_t friend_account = 900051;
    const uint32_t stranger = 900052;

    const FriendCacheEntry one = { friend_account, "Faraway" };
    presence_friends_store(account, &one, 1);
    presence_requests_store(account, NULL, 0);

    bring_online(character, account);
    world_friends_player_entered(account, character, "Watcher");
    outbox_clear();

    presence_set(stranger, 9, 7002, "Nobody");

    /* Then a change that IS watched, published second. Waiting for it is what
     * makes the negative check meaningful: by the time it arrives the bus has
     * demonstrably processed the stranger's change and chosen to drop it. */
    presence_set(friend_account, 9, 7003, "Friend");

    SentPacket sent;
    CHECK(outbox_wait(PACKET_FRIEND_PRESENCE_UPDATE, &sent, 2000) == 1,
          "the watched change arrives");

    int stranger_leaked = 0;
    pthread_mutex_lock(&g_outbox_lock);
    for (int i = 0; i < g_outbox_count; i++) {
        FriendPresenceUpdatePacket* u = (FriendPresenceUpdatePacket*)g_outbox[i].bytes;
        if (g_outbox[i].bytes[0] == PACKET_FRIEND_PRESENCE_UPDATE
            && ntohl(u->account_id) == stranger) {
            stranger_leaked = 1;
        }
    }
    pthread_mutex_unlock(&g_outbox_lock);
    CHECK(!stranger_leaked, "and the stranger's never did");

    presence_clear(stranger);
    presence_clear(friend_account);
    world_friends_player_left(account, character);
    player_remove_active(character);
}

static void test_the_heartbeat_keeps_local_presence_alive(void) {
    printf("redis: the heartbeat renews every local player's presence\n");

    const uint32_t account = 900060, character = 5060;

    presence_friends_store(account, NULL, 0);
    presence_requests_store(account, NULL, 0);

    bring_online(character, account);
    world_friends_player_entered(account, character, "Ticker");

    /* Expire it out from under the refresh, then confirm the refresh is what
     * puts the key back on a TTL rather than leaving it to die. */
    PresenceRecord rec;
    CHECK(presence_get(account, &rec) == 1, "presence exists after entering");

    world_friends_heartbeat();
    CHECK(presence_get(account, &rec) == 1, "and still exists after a heartbeat");
    CHECK(rec.character_id == character, "naming the right character");

    world_friends_player_left(account, character);
    CHECK(presence_get(account, &rec) == 0, "and is gone once they leave");

    player_remove_active(character);
}

int main(void) {
    log_init();

    /* Initializes the slot table and its identifier index -- without it every
     * index entry reads as slot 0 rather than as empty, and the first insert
     * reports the table full. The database calls it makes are stubbed above. */
    playerdata_init("stub");

    printf("=== packets ===\n");
    test_a_short_packet_is_ignored();
    test_a_packet_from_no_session_is_ignored();

    printf("\n=== redis and the bus ===\n");
    if (!session_init() || !redis_ready()) {
        printf("  SKIP no reachable Redis; the parsing checks above still ran\n");
    } else if (!world_friends_init(TEST_WORLD)) {
        printf("  SKIP the event bus did not start\n");
    } else {
        /* The packet cases all act as character 5001. The account they are
         * attributed to is read from this slot, not from the packet. */
        bring_online(5001, 900001);

        test_a_request_becomes_a_mutation();
        test_accept_and_decline_are_different_operations();
        test_block_and_unblock_are_different_operations();
        test_an_unterminated_wire_name_is_bounded();
        test_a_cold_cache_asks_rather_than_answering_empty();
        test_a_warm_cache_answers_without_the_realm();
        test_a_live_character_name_wins_over_the_cached_one();
        test_entering_inverts_the_friend_set();
        test_leaving_spares_another_players_watches();
        test_a_watched_friend_coming_online_reaches_the_watcher();
        test_an_unwatched_change_reaches_nobody();
        test_the_heartbeat_keeps_local_presence_alive();

        player_remove_active(5001);
        world_friends_shutdown();
    }

    presence_consumer_close();
    session_close();

    if (g_failures) {
        printf("\n%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("\nAll world friends checks passed\n");
    return 0;
}
