/**
 * @file
 * Check who each chat channel reaches, and who it does not.
 *
 * Chat is the one gameplay feature whose cost is decided entirely by
 * recipient selection: a local message that goes global is a thousand sends
 * instead of five, and a party message that leaks is a privacy failure rather
 * than a performance one. Both are silent -- the sender sees their own line
 * either way -- so neither shows up without a test that inspects the recipient
 * list.
 *
 * The real chat.c is linked, including its dispatch thread. The player
 * registry and the socket layer are replaced by a small in-test roster and a
 * recorder, so what is measured is the routing and nothing else.
 */

#define _POSIX_C_SOURCE 200809L

#include "chat.h"
#include "player_data.h"
#include "server_types.h"
#include "types.h"
#include "log.h"

#include <arpa/inet.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_failures = 0;

#define CHECK(cond, what)                                                   \
    do {                                                                    \
        if (cond) { printf("  ok   %s\n", (what)); }                        \
        else { printf("  FAIL %s  (%s:%d)\n", (what), __FILE__, __LINE__);  \
               g_failures++; }                                              \
    } while (0)

/* --- The roster ----------------------------------------------------------
 *
 * Five players. Their positions and parties are what the routing decisions
 * below are made from:
 *
 *   slot 0  "Sender"    at (0,0)      party 7    fd 100
 *   slot 1  "Near"      at (100,0)    party 7    fd 101   -- inside 800px
 *   slot 2  "Far"       at (5000,0)   party 7    fd 102   -- outside 800px
 *   slot 3  "Stranger"  at (50,0)     party 0    fd 103   -- near, no party
 *   slot 4  "OtherParty" at (60,0)    party 9    fd 104   -- near, other party
 */
ActivePlayer active_players[MAX_PLAYERS];

#define ROSTER 5
static int g_slots[ROSTER] = { 0, 1, 2, 3, 4 };

static const int FD_SENDER = 100, FD_NEAR = 101, FD_FAR = 102,
                 FD_STRANGER = 103, FD_OTHER_PARTY = 104;

static void seat(int slot, uint32_t id, const char* name, float x, float y,
                 uint32_t party, int fd) {
    ActivePlayer* p = &active_players[slot];
    memset(p, 0, sizeof(*p));
    pthread_mutex_init(&p->lock, NULL);
    p->character_id = id;
    /* One account per character here. The blocks the delivery path consults
     * are held against accounts, so the roster needs them to be distinct and
     * nonzero. */
    p->account_id   = 100 + id;
    p->client_fd    = fd;
    p->pos_x        = x;
    p->pos_y        = y;
    p->party_id     = party;
    p->is_loaded    = 1;
    snprintf(p->username, sizeof(p->username), "%s", name);
}

static void build_roster(void) {
    seat(0, 1, "Sender",     0.0f,    0.0f, 7, FD_SENDER);
    seat(1, 2, "Near",     100.0f,    0.0f, 7, FD_NEAR);
    seat(2, 3, "Far",     5000.0f,    0.0f, 7, FD_FAR);
    seat(3, 4, "Stranger",  50.0f,    0.0f, 0, FD_STRANGER);
    seat(4, 5, "OtherParty", 60.0f,   0.0f, 9, FD_OTHER_PARTY);
}

/* --- Registry stubs ------------------------------------------------------ */

static pthread_rwlock_t g_registry_lock = PTHREAD_RWLOCK_INITIALIZER;

void player_registry_rdlock(void) { pthread_rwlock_rdlock(&g_registry_lock); }
void player_registry_unlock(void) { pthread_rwlock_unlock(&g_registry_lock); }

const int* player_active_list_locked(int* out_count) {
    if (out_count) *out_count = ROSTER;
    return g_slots;
}

int player_slot_of(uint32_t character_id) {
    for (int i = 0; i < ROSTER; i++)
        if (active_players[i].character_id == character_id) return i;
    return -1;
}

ActivePlayer* player_acquire(uint32_t character_id) {
    int slot = player_slot_of(character_id);
    if (slot < 0) return NULL;
    pthread_mutex_lock(&active_players[slot].lock);
    return &active_players[slot];
}

void player_release(ActivePlayer* p) { if (p) pthread_mutex_unlock(&p->lock); }

int player_fd_by_name(const char* name) {
    for (int i = 0; i < ROSTER; i++)
        if (strcmp(active_players[i].username, name) == 0)
            return active_players[i].client_fd;
    return -1;
}

uint32_t player_find_by_name(const char* name) {
    for (int i = 0; i < ROSTER; i++)
        if (strcmp(active_players[i].username, name) == 0)
            return active_players[i].character_id;
    return 0;
}

/* --- The block index, stubbed -------------------------------------------
 *
 * The real one is world_server/src/friends.c, which pulls in the friend bus
 * and Redis. What this file has to prove is that delivery consults it and
 * honours the answer, so one settable edge is the whole stub.
 */
static uint32_t g_block_holder = 0;   /**< Account doing the blocking. */
static uint32_t g_block_target = 0;   /**< Account they refuse. */

int world_friends_is_blocked(uint32_t blocker_account, uint32_t subject_account) {
    return g_block_holder && blocker_account == g_block_holder &&
           subject_account == g_block_target;
}

/** Make `blocker` refuse `subject`, by character id. */
static void set_block(uint32_t blocker, uint32_t subject) {
    g_block_holder = blocker ? 100 + blocker : 0;
    g_block_target = subject ? 100 + subject : 0;
}

/* --- The recorder -------------------------------------------------------- */

#define MAX_SENDS 64

static pthread_mutex_t g_sent_lock = PTHREAD_MUTEX_INITIALIZER;
static int             g_sent_fd[MAX_SENDS];
static char            g_sent_name[MAX_SENDS][32];
static char            g_sent_body[MAX_SENDS][MAX_CHAT_MESSAGE];
static int             g_sent_count = 0;

ssize_t server_send(int fd, void* buf, size_t len) {
    pthread_mutex_lock(&g_sent_lock);
    if (g_sent_count < MAX_SENDS && len >= sizeof(ChatMessagePacket)) {
        const ChatMessagePacket* m = (const ChatMessagePacket*)buf;
        g_sent_fd[g_sent_count] = fd;
        snprintf(g_sent_name[g_sent_count], sizeof(g_sent_name[0]), "%s", m->sender_name);
        snprintf(g_sent_body[g_sent_count], sizeof(g_sent_body[0]), "%s", m->message);
        g_sent_count++;
    }
    pthread_mutex_unlock(&g_sent_lock);
    return (ssize_t)len;
}

static void clear_sends(void) {
    pthread_mutex_lock(&g_sent_lock);
    g_sent_count = 0;
    pthread_mutex_unlock(&g_sent_lock);
}

/** Wait for the dispatch thread to drain, then report how many sends it made. */
static int settle(void) {
    for (int i = 0; i < 500; i++) {
        if (chat_queue_depth() == 0) break;
        struct timespec ts = { 0, 2 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    /* The queue empties before the last deliver() finishes; give it a moment. */
    struct timespec ts = { 0, 30 * 1000 * 1000 };
    nanosleep(&ts, NULL);

    pthread_mutex_lock(&g_sent_lock);
    int n = g_sent_count;
    pthread_mutex_unlock(&g_sent_lock);
    return n;
}

/** Did anything go to this descriptor? */
static int reached(int fd) {
    int found = 0;
    pthread_mutex_lock(&g_sent_lock);
    for (int i = 0; i < g_sent_count; i++) if (g_sent_fd[i] == fd) found = 1;
    pthread_mutex_unlock(&g_sent_lock);
    return found;
}

/* --- Driving one message ------------------------------------------------- */

static void say(uint8_t channel, const char* text) {
    ChatSendPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type         = PACKET_CHAT_SEND;
    pkt.header.player_id    = htonl(1);
    pkt.header.payload_size = htons(sizeof(pkt) - sizeof(PacketHeader));
    pkt.channel             = channel;
    snprintf(pkt.message, sizeof(pkt.message), "%s", text);

    chat_handle_send(FD_SENDER, 1, (uint8_t*)&pkt, (ssize_t)sizeof(pkt));
}

int main(void) {
    printf("=== chat routing ===\n");
    log_init();

    /* No cooldown: several of the cases below are consecutive global messages
     * from the same character, and the cooldown is tested on its own. */
    setenv("MMO_CHAT_GLOBAL_COOLDOWN", "0", 1);
    build_roster();
    assert(chat_init());

    printf("\nTEST 1: local chat reaches the nearby and not the distant\n");
    clear_sends();
    say(CHAT_CHANNEL_LOCAL, "hello nearby");
    int n = settle();
    printf("  %d recipient(s)\n", n);
    CHECK(reached(FD_SENDER),      "the sender hears their own local message");
    CHECK(reached(FD_NEAR),        "a player 100px away hears it");
    CHECK(reached(FD_STRANGER),    "a nearby player in no party hears it");
    CHECK(reached(FD_OTHER_PARTY), "a nearby player in another party hears it");
    CHECK(!reached(FD_FAR),        "a player 5000px away does NOT");

    printf("\nTEST 2: global chat reaches everyone, including the distant\n");
    clear_sends();
    say(CHAT_CHANNEL_GLOBAL, "hello world");
    n = settle();
    printf("  %d recipient(s)\n", n);
    CHECK(n == ROSTER,      "every player in the roster is reached");
    CHECK(reached(FD_FAR),  "distance does not matter on the global channel");

    printf("\nTEST 3: party chat reaches the party and nobody else\n");
    clear_sends();
    say(CHAT_CHANNEL_PARTY, "party up");
    n = settle();
    printf("  %d recipient(s)\n", n);
    CHECK(reached(FD_SENDER),       "the sender hears it");
    CHECK(reached(FD_NEAR),         "a party member nearby hears it");
    CHECK(reached(FD_FAR),          "a party member far away hears it — party is not local");
    CHECK(!reached(FD_STRANGER),    "a player in no party does NOT");
    CHECK(!reached(FD_OTHER_PARTY), "a player in a different party does NOT");

    printf("\nTEST 4: a partyless sender's party message reaches nobody\n");
    /* party_id 0 means "no party". Matching on it would deliver every
     * partyless player's party chat to every other partyless player. */
    clear_sends();
    pthread_mutex_lock(&active_players[0].lock);
    active_players[0].party_id = 0;
    pthread_mutex_unlock(&active_players[0].lock);
    say(CHAT_CHANNEL_PARTY, "anyone there");
    n = settle();
    printf("  %d recipient(s)\n", n);
    CHECK(n == 0, "party 0 is not a party");
    pthread_mutex_lock(&active_players[0].lock);
    active_players[0].party_id = 7;
    pthread_mutex_unlock(&active_players[0].lock);

    printf("\nTEST 5: a whisper reaches its target and its sender, and no one else\n");
    clear_sends();
    say(CHAT_CHANNEL_WHISPER, "Far psst over here");
    n = settle();
    printf("  %d recipient(s)\n", n);
    CHECK(n == 2,                   "exactly two sends: the target and the echo");
    CHECK(reached(FD_FAR),          "the named target receives it");
    CHECK(reached(FD_SENDER),       "the sender receives their own echo");
    CHECK(!reached(FD_NEAR),        "a player standing next to the sender does NOT");
    CHECK(!reached(FD_STRANGER),    "nor does anyone else");

    printf("\nTEST 6: the whisper body has the target name stripped from it\n");
    {
        int ok_body = 0, ok_echo = 0;
        pthread_mutex_lock(&g_sent_lock);
        for (int i = 0; i < g_sent_count; i++) {
            if (g_sent_fd[i] == FD_FAR && strcmp(g_sent_body[i], "psst over here") == 0)
                ok_body = 1;
            /* The echo is labelled with where it went, not who sent it. */
            if (g_sent_fd[i] == FD_SENDER && strncmp(g_sent_name[i], "-> Far", 6) == 0)
                ok_echo = 1;
        }
        pthread_mutex_unlock(&g_sent_lock);
        CHECK(ok_body, "the target sees the body without their own name in front");
        CHECK(ok_echo, "the sender's echo is labelled with the target");
    }

    printf("\nTEST 7: a whisper to nobody reaches nobody\n");
    clear_sends();
    say(CHAT_CHANNEL_WHISPER, "Nonexistent are you there");
    n = settle();
    printf("  %d recipient(s)\n", n);
    CHECK(n == 0, "an offline target is not an excuse to broadcast");

    printf("\nTEST 8: malformed whispers are dropped, not routed\n");
    /* A whisper is "Target body". Without a space there is no target, and the
     * old inline implementation's split is the kind of parse that turns a
     * missing separator into a name of the whole message -- or into a read
     * past it. */
    const char* malformed[] = { "Alone", " leadingspace", "Target ", "" };
    for (size_t i = 0; i < sizeof(malformed) / sizeof(malformed[0]); i++) {
        clear_sends();
        say(CHAT_CHANNEL_WHISPER, malformed[i]);
        n = settle();
        char what[96];
        snprintf(what, sizeof(what), "whisper \"%s\" reaches nobody", malformed[i]);
        CHECK(n == 0, what);
    }

    printf("\nTEST 9: an unknown channel routes to nobody\n");
    clear_sends();
    say(200, "who am I talking to");
    n = settle();
    printf("  %d recipient(s)\n", n);
    CHECK(n == 0, "an unrecognised channel is not treated as global");

    printf("\nTEST 10: an empty message is dropped before it is queued\n");
    clear_sends();
    say(CHAT_CHANNEL_GLOBAL, "");
    n = settle();
    CHECK(n == 0, "an empty message reaches nobody");

    printf("\nTEST 11: a short packet is refused\n");
    clear_sends();
    {
        ChatSendPacket pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.header.type = PACKET_CHAT_SEND;
        pkt.channel     = CHAT_CHANNEL_GLOBAL;
        snprintf(pkt.message, sizeof(pkt.message), "truncated");
        /* One byte short of the packet the handler is written against. */
        chat_handle_send(FD_SENDER, 1, (uint8_t*)&pkt, (ssize_t)sizeof(pkt) - 1);
    }
    n = settle();
    CHECK(n == 0, "a packet one byte short is refused");

    printf("\nTEST 12: an unterminated message is terminated, not read past\n");
    clear_sends();
    {
        ChatSendPacket pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.header.type = PACKET_CHAT_SEND;
        pkt.channel     = CHAT_CHANNEL_GLOBAL;
        memset(pkt.message, 'A', sizeof(pkt.message));   /* no terminator at all */
        chat_handle_send(FD_SENDER, 1, (uint8_t*)&pkt, (ssize_t)sizeof(pkt));
    }
    n = settle();
    printf("  %d recipient(s)\n", n);
    CHECK(n == ROSTER, "it is still delivered");
    {
        int len_ok = 0;
        pthread_mutex_lock(&g_sent_lock);
        if (g_sent_count > 0)
            len_ok = (strlen(g_sent_body[0]) == MAX_CHAT_MESSAGE - 1);
        pthread_mutex_unlock(&g_sent_lock);
        CHECK(len_ok, "truncated to the field, with a terminator inside it");
    }

    printf("\nTEST 13: a block keeps the blocker out of every fan-out channel\n");
    /* The blocker is not removed from the world and the sender is not stopped
     * from speaking; the line simply is not delivered to whoever blocked
     * them. Everybody else still hears it, which is the half that a filter
     * applied in the wrong place gets wrong. */
    set_block(3, 1);            /* Far refuses Sender */

    clear_sends();
    say(CHAT_CHANNEL_GLOBAL, "everyone but one");
    n = settle();
    printf("  %d recipient(s)\n", n);
    CHECK(n == ROSTER - 1,  "one fewer recipient than the roster");
    CHECK(!reached(FD_FAR), "the blocker does not receive it");
    CHECK(reached(FD_NEAR), "everybody else still does");

    clear_sends();
    say(CHAT_CHANNEL_PARTY, "party line");
    n = settle();
    CHECK(!reached(FD_FAR),   "a block outranks shared party membership");
    CHECK(reached(FD_SENDER), "and does not silence the sender's own copy");

    printf("\nTEST 14: a whisper to somebody who blocked you tells you nothing\n");
    /* The sender's echo still prints, so their client shows the line exactly
     * as it would have. A distinguishable failure would make the block a
     * detector, and a detector is what turns one blocked account into two. */
    clear_sends();
    say(CHAT_CHANNEL_WHISPER, "Far let me in");
    n = settle();
    printf("  %d recipient(s)\n", n);
    CHECK(n == 1,             "exactly one send: the sender's own echo");
    CHECK(!reached(FD_FAR),   "the target does not receive it");
    CHECK(reached(FD_SENDER), "the sender sees the same echo as a delivered whisper");

    printf("\nTEST 15: the block is directional\n");
    set_block(1, 3);            /* Sender refuses Far -- the other way round */
    clear_sends();
    say(CHAT_CHANNEL_GLOBAL, "still audible");
    n = settle();
    CHECK(reached(FD_FAR), "blocking somebody does not stop you being heard by them");

    set_block(0, 0);            /* clear, so the cases below are unaffected */

    printf("\nTEST 16: the global cooldown applies per character\n");
    chat_shutdown();
    setenv("MMO_CHAT_GLOBAL_COOLDOWN", "600", 1);
    assert(chat_init());

    clear_sends();
    say(CHAT_CHANNEL_GLOBAL, "first");
    int first = settle();
    say(CHAT_CHANNEL_GLOBAL, "second, immediately");
    int both = settle();
    printf("  first message: %d sends; after a second: %d sends\n", first, both);
    CHECK(first == ROSTER, "the first global message goes out");
    CHECK(both == first,   "a second inside the cooldown adds nothing");

    printf("\nTEST 17: the cooldown does not gag the other channels\n");
    clear_sends();
    say(CHAT_CHANNEL_LOCAL, "still talking");
    n = settle();
    printf("  %d recipient(s)\n", n);
    CHECK(n > 0, "local chat is unaffected by the global cooldown");

    chat_shutdown();

    for (int i = 0; i < ROSTER; i++) pthread_mutex_destroy(&active_players[i].lock);

    if (g_failures) {
        printf("\n=== chat routing: %d check(s) FAILED ===\n", g_failures);
        return 1;
    }
    printf("\n=== chat routing: all checks passed ===\n");
    return 0;
}
