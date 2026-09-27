/**
 * @file
 * Walk the opening sequence against live servers, as a client would.
 *
 * The content test beside this one proves the data is self-consistent. This one
 * proves the sequence is *playable*: it logs in, creates a character of a given
 * race, enters the world, and then walks the courtyard talking to the Warden,
 * the race leader and the Gate Sentry, asserting at every step that the pages,
 * the offered choices, and the quest packets are the ones the design calls for.
 *
 * It is not part of `make test`, because it needs the three servers, PostgreSQL
 * and Redis actually running. Build and run it with:
 *
 *     make tests/bin/opening_flow_test
 *     ./tests/bin/opening_flow_test wolf
 *
 * Pass a race key from races.json; the account and character are named after it
 * so a re-run of the same race reuses the same character.
 */

#include "protocol.h"
#include "types.h"

#include <openssl/ssl.h>
#include <openssl/err.h>

#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* --- Reporting ----------------------------------------------------------- */

static int g_checks = 0;

static void fail(const char* fmt, ...) {
    va_list args;
    printf("\n  FAIL: ");
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
    exit(1);
}

static void check(int condition, const char* fmt, ...) {
    if (!condition) {
        va_list args;
        printf("\n  FAIL: ");
        va_start(args, fmt);
        vprintf(fmt, args);
        va_end(args);
        printf("\n");
        exit(1);
    }
    g_checks++;
}

static void step(const char* fmt, ...) {
    va_list args;
    printf("\n--- ");
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf(" ---\n");
}

/* --- Sockets ------------------------------------------------------------- */

static int tcp_connect(const char* ip, uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) { close(fd); return -1; }

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { close(fd); return -1; }
    return fd;
}

/** Read exactly `len` bytes, or fail the run. */
static void recv_exact(int fd, void* buf, size_t len, const char* what) {
    size_t total = 0;
    while (total < len) {
        struct pollfd p = { .fd = fd, .events = POLLIN };
        if (poll(&p, 1, 8000) <= 0) fail("timed out waiting for %s", what);
        ssize_t got = recv(fd, (char*)buf + total, len - total, 0);
        if (got <= 0) fail("connection closed while reading %s", what);
        total += (size_t)got;
    }
}

static void send_all(int fd, const void* buf, size_t len, const char* what) {
    if (send(fd, buf, len, 0) != (ssize_t)len) fail("failed to send %s", what);
}


/**
 * Read one realm packet, using the header's payload size.
 *
 * The realm truncates its variable-length lists to the entries actually used
 * and states the length in the header, so reading sizeof(struct) blocks
 * forever on a short list.
 *
 * @param out      Destination, zeroed first so unsent tail fields read as zero.
 * @param out_cap  Capacity of out; a longer packet is a protocol error.
 * @return         The number of bytes read into out.
 */
static int recv_packet(int fd, uint8_t expected_type, void* out, size_t out_cap,
                       const char* what) {
    memset(out, 0, out_cap);

    PacketHeader header;
    recv_exact(fd, &header, sizeof(header), what);

    /* Named rather than assumed: reading a packet into the wrong struct is the
     * one mistake here that produces plausible-looking garbage instead of an
     * error, and it is exactly what a missed unsolicited packet causes. */
    if (header.type != expected_type)
        fail("expected %s (type %u) but the realm sent type %u",
             what, expected_type, header.type);

    size_t payload = ntohs(header.payload_size);
    if (sizeof(header) + payload > out_cap)
        fail("%s claims %zu payload bytes, more than the %zu expected",
             what, payload, out_cap - sizeof(header));

    memcpy(out, &header, sizeof(header));
    if (payload > 0) recv_exact(fd, (char*)out + sizeof(header), payload, what);
    return (int)(sizeof(header) + payload);
}

/* --- Login server (TLS) -------------------------------------------------- */

static SSL_CTX* g_ssl_ctx;

/** Open a TLS session to the login server. */
static SSL* login_connect(int* out_fd) {
    int fd = tcp_connect("127.0.0.1", LOGIN_SERVER_PORT);
    if (fd < 0) fail("cannot reach the login server on port %d", LOGIN_SERVER_PORT);

    SSL* ssl = SSL_new(g_ssl_ctx);
    SSL_set_fd(ssl, fd);
    if (SSL_connect(ssl) != 1) fail("TLS handshake with the login server failed");

    *out_fd = fd;
    return ssl;
}

static void login_close(SSL* ssl, int fd) {
    SSL_shutdown(ssl);
    SSL_free(ssl);
    close(fd);
}

/** Read one whole response off a TLS session. */
static int tls_read_response(SSL* ssl, void* buf, int cap) {
    int got = SSL_read(ssl, buf, cap);
    if (got <= 0) fail("no response from the login server");
    return got;
}


/**
 * Pace realm requests the way a person clicking through menus would.
 *
 * The realm charges query-class packets against a token bucket, and firing the
 * world list, character list, creation and entry back to back in one
 * millisecond is over budget -- correctly so. This is not a workaround for the
 * limiter; it is the test behaving like the client it stands in for.
 */
static void pace(void) {
    struct timespec ts = { .tv_sec = 3, .tv_nsec = 0 };
    nanosleep(&ts, NULL);
}

/* --- The walk ------------------------------------------------------------ */

/** Identifier scheme, mirrored from Next_steps/gen_opening.py. */
#define NPC_TYPE_WARDEN      20
#define NPC_TYPE_GATE_SENTRY 21
#define NPC_TYPE_LEADER_BASE 30
#define QUEST_CALLED_BASE   100
#define QUEST_INTO_ENNARA   200

/** Track the last dialogue page the world sent. */
typedef struct {
    uint32_t npc_id;
    uint32_t dialogue_id;
    uint8_t  page_num;
    uint8_t  option_count;
    uint8_t  option_ids[MAX_DIALOGUE_OPTIONS];
    char     npc_name[32];
    char     text[MAX_DIALOGUE_TEXT];
    char     option_text[MAX_DIALOGUE_OPTIONS][MAX_OPTION_TEXT];
    int      open;
} PageView;

/** Track one NPC the world has told us about. */
typedef struct {
    uint32_t id;
    float    x, y;
    uint8_t  type_id;
} SeenNPC;

/** Track everything the world server has told us since we connected. */
typedef struct {
    SeenNPC  npcs[64];
    int      npc_count;

    PageView page;

    /** The most recent quest accept, progress, and completion. */
    uint32_t accepted_quest;
    char     accepted_title[48];
    uint8_t  accepted_obj_count;
    QuestObjectiveInfo accepted_objectives[MAX_QUEST_OBJECTIVES];

    uint32_t progress_quest;
    int32_t  progress_current;
    int32_t  progress_required;

    uint32_t completed_quest;
    uint32_t completed_xp;

    /** The most recent abandon the world confirmed. */
    uint32_t abandoned_quest;

    /** Where the world says we are. A character that logged out mid-courtyard
     *  is not at the spawn point, and assuming otherwise makes every move
     *  packet look like a teleport. */
    int   have_position;
    float pos_x, pos_y;
} WorldView;

static WorldView g_world;

/** Note an NPC, replacing an earlier report of the same one. */
static void remember_npc(uint32_t id, float x, float y, uint8_t type_id) {
    for (int i = 0; i < g_world.npc_count; i++) {
        if (g_world.npcs[i].id == id) {
            g_world.npcs[i].x = x;
            g_world.npcs[i].y = y;
            return;
        }
    }
    if (g_world.npc_count >= (int)(sizeof(g_world.npcs) / sizeof(g_world.npcs[0]))) return;

    SeenNPC* npc = &g_world.npcs[g_world.npc_count++];
    npc->id      = id;
    npc->x       = x;
    npc->y       = y;
    npc->type_id = type_id;
}

/** Find a remembered NPC by its type. */
static const SeenNPC* npc_of_type(uint8_t type_id) {
    for (int i = 0; i < g_world.npc_count; i++)
        if (g_world.npcs[i].type_id == type_id) return &g_world.npcs[i];
    return NULL;
}

/** Interpret one world packet. */
static void absorb_packet(const uint8_t* buf, int len) {
    if (len < (int)sizeof(PacketHeader)) return;
    uint8_t type = buf[0];

    switch (type) {
        case PACKET_NPC_POSITIONS: {
            const NPCPositionPacket* pkt = (const NPCPositionPacket*)buf;
            int count = pkt->npc_count;
            if (count > MAX_NPCS_PER_PACKET) count = MAX_NPCS_PER_PACKET;
            for (int i = 0; i < count; i++) {
                const NPCPositionData* d = &pkt->npcs[i];
                if (!d->is_alive) continue;
                remember_npc(ntohl(d->npc_id), d->pos_x, d->pos_y, d->npc_type_id);
            }
            break;
        }

        case PACKET_NPC_INTERACT_RESPONSE: {
            const NPCInteractResponsePacket* pkt = (const NPCInteractResponsePacket*)buf;
            PageView* p = &g_world.page;
            memset(p, 0, sizeof(*p));
            p->open         = 1;
            p->npc_id       = ntohl(pkt->npc_id);
            p->dialogue_id  = ntohl(pkt->dialogue_id);
            p->page_num     = pkt->page_num;
            p->option_count = pkt->option_count;
            memcpy(p->option_ids, pkt->option_ids, sizeof(p->option_ids));
            snprintf(p->npc_name, sizeof(p->npc_name), "%s", pkt->npc_name);
            snprintf(p->text, sizeof(p->text), "%s", pkt->text);
            for (int i = 0; i < p->option_count && i < MAX_DIALOGUE_OPTIONS; i++)
                snprintf(p->option_text[i], MAX_OPTION_TEXT, "%s", pkt->option_text[i]);
            break;
        }

        case PACKET_DIALOGUE_UPDATE: {
            const DialogueUpdatePacket* pkt = (const DialogueUpdatePacket*)buf;
            PageView* p = &g_world.page;
            p->open         = 1;
            p->npc_id       = ntohl(pkt->npc_id);
            p->dialogue_id  = ntohl(pkt->dialogue_id);
            p->page_num     = pkt->page_num;
            p->option_count = pkt->option_count;
            memcpy(p->option_ids, pkt->option_ids, sizeof(p->option_ids));
            snprintf(p->text, sizeof(p->text), "%s", pkt->text);
            memset(p->option_text, 0, sizeof(p->option_text));
            for (int i = 0; i < p->option_count && i < MAX_DIALOGUE_OPTIONS; i++)
                snprintf(p->option_text[i], MAX_OPTION_TEXT, "%s", pkt->option_text[i]);
            break;
        }

        case PACKET_DIALOGUE_CLOSE:
            g_world.page.open = 0;
            break;

        case PACKET_PLAYER_DATA_RESPONSE: {
            const CharacterInfo* info = (const CharacterInfo*)buf;
            g_world.have_position = 1;
            g_world.pos_x = info->pos_x;
            g_world.pos_y = info->pos_y;
            break;
        }

        case PACKET_QUEST_ACCEPT: {
            const QuestAcceptPacket* pkt = (const QuestAcceptPacket*)buf;
            g_world.accepted_quest = ntohl(pkt->quest_id);
            snprintf(g_world.accepted_title, sizeof(g_world.accepted_title), "%s", pkt->title);
            g_world.accepted_obj_count = pkt->obj_count;
            memcpy(g_world.accepted_objectives, pkt->objectives,
                   sizeof(g_world.accepted_objectives));
            break;
        }

        case PACKET_QUEST_PROGRESS: {
            const QuestProgressPacket* pkt = (const QuestProgressPacket*)buf;
            g_world.progress_quest    = ntohl(pkt->quest_id);
            g_world.progress_current  = (int32_t)ntohl((uint32_t)pkt->current);
            g_world.progress_required = (int32_t)ntohl((uint32_t)pkt->required);
            break;
        }

        case PACKET_QUEST_ABANDONED: {
            const QuestAbandonPacket* pkt = (const QuestAbandonPacket*)buf;
            g_world.abandoned_quest = ntohl(pkt->quest_id);
            break;
        }

        case PACKET_QUEST_COMPLETE: {
            const QuestCompletePacket* pkt = (const QuestCompletePacket*)buf;
            g_world.completed_quest = ntohl(pkt->quest_id);
            g_world.completed_xp    = ntohl(pkt->xp_reward);
            break;
        }

        default:
            break;
    }
}

static uint8_t g_stream[1 << 20];
static int     g_stream_len = 0;

/**
 * Drain the world socket for a while, interpreting whole packets.
 *
 * Framed by the header's payload size, exactly as the real client frames it,
 * so packet types this test does not care about are stepped over rather than
 * needing a size table that would go stale.
 */
static void pump_world(int fd, int millis) {
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (now.tv_sec - start.tv_sec) * 1000.0 +
                         (now.tv_nsec - start.tv_nsec) / 1e6;
        if (elapsed >= millis) break;

        struct pollfd p = { .fd = fd, .events = POLLIN };
        int ready = poll(&p, 1, (int)(millis - elapsed));
        if (ready <= 0) continue;

        ssize_t got = recv(fd, g_stream + g_stream_len,
                           sizeof(g_stream) - (size_t)g_stream_len, 0);
        if (got <= 0) fail("world server closed the connection");
        g_stream_len += (int)got;

        int offset = 0;
        while (offset + (int)sizeof(PacketHeader) <= g_stream_len) {
            const PacketHeader* hdr = (const PacketHeader*)(g_stream + offset);
            int size = (int)sizeof(PacketHeader) + (int)ntohs(hdr->payload_size);
            if (size < (int)sizeof(PacketHeader) || size > (int)sizeof(g_stream))
                fail("world sent a packet claiming %d bytes", size);
            if (offset + size > g_stream_len) break;
            absorb_packet(g_stream + offset, size);
            offset += size;
        }

        if (offset > 0) {
            memmove(g_stream, g_stream + offset, (size_t)(g_stream_len - offset));
            g_stream_len -= offset;
        }
    }
}

/** Declared movement speed, and how long each step waits, in world px/s and ms. */
#define WALK_SPEED_PX_S 220.0f
#define WALK_STEP_MS      50

/** Report where the character now stands, and tell the world about it. */
static void walk_to(int fd, uint32_t character_id, float x, float y) {
    PlayerMovePacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type         = PACKET_PLAYER_MOVE;
    pkt.header.player_id    = htonl(character_id);
    pkt.header.payload_size = htons(sizeof(pkt) - sizeof(PacketHeader));
    pkt.pos_x        = x;
    pkt.pos_y        = y;
    /* Numbered the way the client numbers them. The server does not act on it
     * -- it hands it back on a refusal so the client knows which proposal was
     * refused -- but sending zero for every move would leave this flow
     * exercising a shape no real client produces. */
    static uint32_t sequence = 0;
    pkt.sequence     = htonl(++sequence);
    /* No speed field: the client used to declare one here and the server
     * correctly ignored it, using its own record of the character's speed.
     * It was removed at the PROTOCOL_VERSION 5 bump. WALK_SPEED_PX_S is still
     * what this test paces itself by, because that is what the server's
     * movement budget will accept -- it is just no longer something the
     * client gets to say out loud. */
    send_all(fd, &pkt, sizeof(pkt), "player move");

    g_world.pos_x = x;
    g_world.pos_y = y;
}

/**
 * Move to an NPC at a speed the server's movement budget will accept, then talk.
 *
 * The step length is derived from the elapsed time and the declared speed, with
 * headroom, because that is the rule the server actually enforces: a single
 * teleporting move packet is exactly what a speed hack looks like, and the move
 * validator is right to reject it. Walking the courtyard therefore takes about
 * as long here as it would with a hand on the keys.
 */
static void approach_and_talk(int fd, uint32_t character_id,
                              float from_x, float from_y, const SeenNPC* npc) {
    float x = from_x, y = from_y;
    const float stride_max = WALK_SPEED_PX_S * (WALK_STEP_MS / 1000.0f) * 0.8f;

    for (int i = 0; i < 4000; i++) {
        float dx = npc->x - x, dy = npc->y - y;
        float dist = sqrtf(dx * dx + dy * dy);
        if (dist <= 60.0f) break;

        float stride = dist < stride_max ? dist : stride_max;
        x += dx / dist * stride;
        y += dy / dist * stride;

        walk_to(fd, character_id, x, y);
        pump_world(fd, WALK_STEP_MS);
    }

    NPCInteractRequestPacket req;
    memset(&req, 0, sizeof(req));
    req.header.type         = PACKET_NPC_INTERACT_REQUEST;
    req.header.player_id    = htonl(character_id);
    req.header.payload_size = htons(sizeof(req) - sizeof(PacketHeader));
    req.npc_id              = htonl(npc->id);
    send_all(fd, &req, sizeof(req), "npc interact request");

    g_world.page.open = 0;
    pump_world(fd, 900);
}

/** Choose an option by identifier and wait for what the world does about it. */
static void choose(int fd, uint32_t character_id, uint8_t option_id) {
    DialogueOptionSelectPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type         = PACKET_DIALOGUE_OPTION_SELECT;
    pkt.header.player_id    = htonl(character_id);
    pkt.header.payload_size = htons(sizeof(pkt) - sizeof(PacketHeader));
    pkt.npc_id       = htonl(g_world.page.npc_id);
    pkt.dialogue_id  = htonl(g_world.page.dialogue_id);
    pkt.current_page = g_world.page.page_num;
    pkt.option_id    = option_id;
    send_all(fd, &pkt, sizeof(pkt), "dialogue option");

    pump_world(fd, 900);
}

/** Print a page the way a player would read it. */
static void show_page(void) {
    const PageView* p = &g_world.page;
    printf("  [%s] page %u\n", p->npc_name[0] ? p->npc_name : "...", p->page_num);
    printf("  | %s\n", p->text);
    for (int i = 0; i < p->option_count; i++)
        printf("  > (%u) %s\n", p->option_ids[i], p->option_text[i]);
}

/** Find the option whose text starts with a given prefix. */
static int option_with_prefix(const char* prefix, uint8_t* out_id) {
    const PageView* p = &g_world.page;
    for (int i = 0; i < p->option_count; i++) {
        if (strncmp(p->option_text[i], prefix, strlen(prefix)) == 0) {
            *out_id = p->option_ids[i];
            return 1;
        }
    }
    return 0;
}

/** Ask the world to drop a quest, and wait for whatever it says back. */
static void abandon_quest(int fd, uint32_t character_id, uint32_t quest_id) {
    QuestAbandonPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type         = PACKET_QUEST_ABANDON;
    pkt.header.player_id    = htonl(character_id);
    pkt.header.payload_size = htons(sizeof(pkt) - sizeof(PacketHeader));
    pkt.quest_id            = htonl(quest_id);
    pace();
    send_all(fd, &pkt, sizeof(pkt), "quest abandon");
    pump_world(fd, 600);
}

/* --- Main ---------------------------------------------------------------- */

/** Map a race key to its identifier, mirroring races.json. */
static uint32_t race_id_for(const char* key) {
    static const struct { const char* key; uint32_t id; } RACES[] = {
        { "wolf", 1 }, { "bear", 2 }, { "fox", 3 }, { "crow", 4 }, { "hawk", 5 },
        { "deer", 6 }, { "rabbit", 8 }, { "hyena", 9 }, { "cat", 10 },
    };
    for (size_t i = 0; i < sizeof(RACES) / sizeof(RACES[0]); i++)
        if (strcmp(RACES[i].key, key) == 0) return RACES[i].id;
    return 0;
}

int main(int argc, char** argv) {
    const char* race_key = (argc > 1) ? argv[1] : "wolf";
    uint32_t race_id = race_id_for(race_key);
    if (race_id == 0) fail("unknown race key '%s'", race_key);

    char account[32], character[32];
    snprintf(account,   sizeof(account),   "flow_%s", race_key);
    snprintf(character, sizeof(character), "Flow%c%s",
             (char)(race_key[0] - 32), race_key + 1);

    printf("=== opening sequence, walked as a %s ===\n", race_key);
    printf("account '%s', character '%s'\n", account, character);

    SSL_library_init();
    g_ssl_ctx = SSL_CTX_new(TLS_client_method());
    if (!g_ssl_ctx) fail("cannot create a TLS context");
    /* The dev server uses a self-signed certificate; this test is talking to
     * 127.0.0.1 and is checking gameplay, not the PKI. */
    SSL_CTX_set_verify(g_ssl_ctx, SSL_VERIFY_NONE, NULL);

    /* --- Register (idempotent) and log in --- */
    step("login");
    {
        int fd; SSL* ssl = login_connect(&fd);
        AuthRegisterPacket reg;
        memset(&reg, 0, sizeof(reg));
        reg.header.type         = PACKET_AUTH_REGISTER;
        reg.header.payload_size = htons(sizeof(reg) - sizeof(PacketHeader));
        snprintf(reg.username, sizeof(reg.username), "%s", account);
        snprintf(reg.password, sizeof(reg.password), "FlowTest123");
        snprintf(reg.email,    sizeof(reg.email),    "%s@example.invalid", account);
        snprintf(reg.birthday, sizeof(reg.birthday), "1990-01-01");
        SSL_write(ssl, &reg, sizeof(reg));

        uint8_t buf[512];
        int got = tls_read_response(ssl, buf, sizeof(buf));
        printf("  registration replied %d bytes (an existing account is fine)\n", got);
        login_close(ssl, fd);
    }

    uint32_t player_id = 0;
    char auth_token[32];
    {
        int fd; SSL* ssl = login_connect(&fd);
        AuthLoginPacket login;
        memset(&login, 0, sizeof(login));
        login.header.type         = PACKET_AUTH_LOGIN;
        login.header.payload_size = htons(sizeof(login) - sizeof(PacketHeader));
        snprintf(login.username, sizeof(login.username), "%s", account);
        snprintf(login.password, sizeof(login.password), "FlowTest123");
        SSL_write(ssl, &login, sizeof(login));

        uint8_t buf[512];
        int got = tls_read_response(ssl, buf, sizeof(buf));
        check(got >= 44, "login response is at least header+success+id+token");

        player_id = ntohl(*(uint32_t*)(buf + 1));
        check(buf[7] == 1, "login was accepted");
        memcpy(auth_token, buf + 12, 32);
        login_close(ssl, fd);
        printf("  logged in as player %u\n", player_id);
    }

    char session_key[32];
    {
        int fd; SSL* ssl = login_connect(&fd);
        StartGameRequestPacket start;
        memset(&start, 0, sizeof(start));
        start.header.type         = PACKET_START_GAME_REQUEST;
        start.header.player_id    = htonl(player_id);
        start.header.payload_size = htons(sizeof(start) - sizeof(PacketHeader));
        start.player_id           = htonl(player_id);
        snprintf(start.username, sizeof(start.username), "%s", account);
        memcpy(start.auth_token, auth_token, 32);
        SSL_write(ssl, &start, sizeof(start));

        uint8_t buf[512];
        int got = tls_read_response(ssl, buf, sizeof(buf));
        check(got >= 40, "start-game response carries a session key");
        memcpy(session_key, buf + 7, 32);
        check(buf[39] == 1, "start-game was accepted");
        login_close(ssl, fd);
        printf("  session created\n");
    }

    /* --- Realm --- */
    step("realm");
    int realm_fd = tcp_connect("127.0.0.1", REALM_SERVER_PORT);
    if (realm_fd < 0) fail("cannot reach the realm server on port %d", REALM_SERVER_PORT);

    {
        RealmConnectPacket pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.header.type         = PACKET_REALM_CONNECT;
        pkt.header.player_id    = htonl(player_id);
        pkt.header.payload_size = 0;
        memcpy(pkt.header.session_key, session_key, 32);
        pkt.protocol_version    = htons(PROTOCOL_VERSION);
        send_all(realm_fd, &pkt, sizeof(pkt), "realm connect");

        RealmConnectAckPacket ack;
        recv_packet(realm_fd, PACKET_REALM_CONNECT_ACK, &ack, sizeof(ack), "realm ack");
        check(ack.success, "realm accepted the session: %s", ack.message);
        printf("  %s\n", ack.message);
    }

    uint32_t world_id = 0;
    {
        WorldListRequestPacket req;
        memset(&req, 0, sizeof(req));
        req.header.type         = PACKET_WORLD_LIST_REQUEST;
        req.header.player_id    = htonl(player_id);
        req.header.payload_size = htons(sizeof(req) - sizeof(PacketHeader));
        pace();
        send_all(realm_fd, &req, sizeof(req), "world list request");

        WorldListResponsePacket list;
        recv_packet(realm_fd, PACKET_WORLD_LIST_RESPONSE, &list, sizeof(list), "world list");
        check(list.count > 0, "the realm offers at least one world");

        for (int i = 0; i < list.count && i < MAX_WORLDS; i++) {
            if (list.worlds[i].status != 1) continue;
            world_id = ntohl(list.worlds[i].world_id);
            printf("  entering world '%s' (id %u)\n", list.worlds[i].name, world_id);
            break;
        }
        check(world_id != 0, "at least one world is online");
    }

    /* Delete any character left by an earlier run and start clean.
     *
     * The sequence under test is a *first* login: a character who already holds
     * "Called to ..." would be refused it a second time, correctly, and the run
     * would prove nothing. */
    uint32_t character_id = 0;
    {
        CharacterListRequestPacket req;
        memset(&req, 0, sizeof(req));
        req.header.type         = PACKET_CHARACTER_LIST_REQUEST;
        req.header.player_id    = htonl(player_id);
        req.header.payload_size = htons(sizeof(req) - sizeof(PacketHeader));
        req.world_id            = htonl(world_id);
        pace();
        send_all(realm_fd, &req, sizeof(req), "character list request");

        CharacterListResponsePacket list;
        recv_packet(realm_fd, PACKET_CHARACTER_LIST_RESPONSE, &list, sizeof(list), "character list");

        for (int i = 0; i < list.count && i < 10; i++) {
            if (strcmp(list.characters[i].name, character) != 0) continue;

            uint32_t stale = ntohl(list.characters[i].character_id);
            printf("  deleting character '%s' (id %u) from an earlier run\n",
                   list.characters[i].name, stale);

            CharacterDeleteRequestPacket del;
            memset(&del, 0, sizeof(del));
            del.header.type         = PACKET_CHARACTER_DELETE_REQUEST;
            del.header.player_id    = htonl(player_id);
            del.header.payload_size = htons(sizeof(del) - sizeof(PacketHeader));
            del.character_id        = htonl(stale);
            del.world_id            = htonl(world_id);
            pace();
            send_all(realm_fd, &del, sizeof(del), "character delete");

            CharacterDeleteResponsePacket del_resp;
            recv_packet(realm_fd, PACKET_CHARACTER_DELETE_RESPONSE, &del_resp,
                        sizeof(del_resp), "character delete response");
            check(del_resp.success, "the earlier character was deleted: %s", del_resp.message);

            CharacterListResponsePacket after;
            recv_packet(realm_fd, PACKET_CHARACTER_LIST_RESPONSE, &after,
                        sizeof(after), "character list after deletion");
            break;
        }
    }

    {
        CharacterCreateRequestPacket req;
        memset(&req, 0, sizeof(req));
        req.header.type         = PACKET_CHARACTER_CREATE_REQUEST;
        req.header.player_id    = htonl(player_id);
        req.header.payload_size = htons(sizeof(req) - sizeof(PacketHeader));
        req.world_id            = htonl(world_id);
        snprintf(req.name, sizeof(req.name), "%s", character);
        req.class_id = htonl(race_id);   /* race and class fuse into one id */
        req.race_id  = htonl(race_id);
        pace();
        send_all(realm_fd, &req, sizeof(req), "character create");

        CharacterCreateResponsePacket resp;
        recv_packet(realm_fd, PACKET_CHARACTER_CREATE_RESPONSE, &resp, sizeof(resp), "character create response");
        check(resp.success, "character created: %s", resp.message);
        character_id = ntohl(resp.character_id);
        printf("  created character '%s' (id %u)\n", resp.character_name, character_id);

        /* The realm pushes a refreshed list straight afterwards. */
        CharacterListResponsePacket refreshed;
        recv_packet(realm_fd, PACKET_CHARACTER_LIST_RESPONSE, &refreshed, sizeof(refreshed), "refreshed character list");
    }

    char game_ticket[64];
    char world_host[64];
    uint16_t world_port = 0;
    {
        EnterWorldPacket req;
        memset(&req, 0, sizeof(req));
        req.header.type         = PACKET_ENTER_WORLD;
        req.header.player_id    = htonl(player_id);
        req.header.payload_size = htons(sizeof(req) - sizeof(PacketHeader));
        req.character_id        = htonl(character_id);
        req.world_id            = htonl(world_id);
        pace();
        send_all(realm_fd, &req, sizeof(req), "enter world");

        EnterWorldResponsePacket resp;
        recv_packet(realm_fd, PACKET_ENTER_WORLD_RESPONSE, &resp, sizeof(resp), "enter world response");
        check(resp.success, "the realm admitted us: %s", resp.message);

        memcpy(game_ticket, resp.game_ticket, 64);
        snprintf(world_host, sizeof(world_host), "%s", resp.world_host);
        world_port = ntohs(resp.world_port);
        printf("  world at %s:%u\n", world_host, world_port);
    }
    close(realm_fd);

    /* --- World --- */
    step("world");
    int world_fd = tcp_connect(world_host, world_port);
    if (world_fd < 0) fail("cannot reach the world server at %s:%u", world_host, world_port);

    {
        WorldConnectPacket pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.header.type         = PACKET_WORLD_CONNECT;
        pkt.header.player_id    = htonl(player_id);
        pkt.header.payload_size = htons(sizeof(pkt) - sizeof(PacketHeader));
        memcpy(pkt.game_ticket, game_ticket, 64);
        pkt.character_id     = htonl(character_id);
        pkt.protocol_version = htons(PROTOCOL_VERSION);
        send_all(world_fd, &pkt, sizeof(pkt), "world connect");

        WorldConnectAckPacket ack;
        recv_packet(world_fd, PACKET_WORLD_CONNECT_ACK, &ack, sizeof(ack), "world ack");
        check(ack.success, "the world admitted us: %s", ack.welcome_message);
        printf("  %s\n", ack.welcome_message);
    }

    {
        WorldPlayerDataRequest req;
        memset(&req, 0, sizeof(req));
        req.header.type         = PACKET_REQUEST_PLAYER_DATA;
        req.header.player_id    = htonl(player_id);
        req.header.payload_size = htons(sizeof(req) - sizeof(PacketHeader));
        req.character_id        = htonl(character_id);
        req.world_id            = htonl(world_id);
        send_all(world_fd, &req, sizeof(req), "player data request");
    }

    /* Let a few NPC broadcasts and the player data land. */
    pump_world(world_fd, 1500);
    check(g_world.have_position, "the world reported where our character stands");
    printf("  standing at (%.0f, %.0f)\n", g_world.pos_x, g_world.pos_y);
    printf("  %d NPCs visible from the spawn point\n", g_world.npc_count);
    check(g_world.npc_count >= 11,
          "the whole courtyard is in view (saw %d NPCs, expected 11)", g_world.npc_count);

    const SeenNPC* warden = npc_of_type(NPC_TYPE_WARDEN);
    const SeenNPC* leader = npc_of_type((uint8_t)(NPC_TYPE_LEADER_BASE + race_id));
    const SeenNPC* sentry = npc_of_type(NPC_TYPE_GATE_SENTRY);
    check(warden != NULL, "the Courtyard Warden is spawned");
    check(leader != NULL, "the %s leader is spawned", race_key);
    check(sentry != NULL, "the Gate Sentry is spawned");

    /* --- The Warden --- */
    step("Courtyard Warden");
    approach_and_talk(world_fd, character_id, g_world.pos_x, g_world.pos_y, warden);
    check(g_world.page.open, "the Warden opened a conversation");
    check(strcmp(g_world.page.npc_name, "Courtyard Warden") == 0,
          "we are talking to the Warden, not '%s'", g_world.page.npc_name);
    check(g_world.page.text[0] != '\0', "the page carried its text on the wire");
    show_page();
    check(g_world.page.option_count == 3, "the opening page offers three questions");

    uint8_t option = 0;
    check(option_with_prefix("Then tell me what to do", &option),
          "the Warden offers to send us on");
    choose(world_fd, character_id, option);
    show_page();

    /* This is the race gate: nine options are authored, and only ours passes. */
    check(g_world.page.option_count == 2,
          "the race branch shows one leader and the way out, not %u options",
          g_world.page.option_count);
    check(option_with_prefix("Where do I find", &option),
          "our race's leader is named on the page");
    printf("  race-gated option: \"%s\"\n", g_world.page.option_text[0]);

    g_world.accepted_quest = 0;
    choose(world_fd, character_id, option);
    show_page();

    check(g_world.accepted_quest == QUEST_CALLED_BASE + race_id,
          "the Warden granted quest %u, not %u",
          g_world.accepted_quest, QUEST_CALLED_BASE + race_id);
    printf("  quest %u accepted: '%s'\n", g_world.accepted_quest, g_world.accepted_title);

    check(g_world.accepted_obj_count == 1, "the quest has one objective");
    {
        const QuestObjectiveInfo* obj = &g_world.accepted_objectives[0];
        check(obj->objective_type == QUEST_OBJECTIVE_TALK, "it is a conversation objective");
        check(ntohl(obj->target_id) == (uint32_t)(NPC_TYPE_LEADER_BASE + race_id),
              "it targets our leader's NPC type");
        check(obj->has_marker, "it carries a map marker");
        float dx = obj->marker_x - leader->x, dy = obj->marker_y - leader->y;
        check(sqrtf(dx * dx + dy * dy) < 1.0f,
              "the marker sits on the leader (off by %.1f)", sqrtf(dx * dx + dy * dy));
        printf("  objective: \"%s\" -> marker (%.0f, %.0f)\n",
               obj->description, obj->marker_x, obj->marker_y);
    }

    choose(world_fd, character_id, g_world.page.option_ids[0]);   /* "I will go." */

    /* --- Giving it up, and being offered it again ---
     *
     * The only quest packet a client starts, so this is the only place the
     * server acts on a quest identifier a player chose. Three things are worth
     * proving on the wire and not just in a unit test: the quest actually goes,
     * a second attempt at the same quest is refused rather than acted on, and
     * nothing about giving up was mistaken for finishing -- which is what makes
     * the Warden offer it a second time. */
    step("abandon and retake");
    uint32_t called_quest = QUEST_CALLED_BASE + race_id;

    g_world.abandoned_quest = 0;
    abandon_quest(world_fd, character_id, called_quest);
    check(g_world.abandoned_quest == called_quest,
          "the world confirmed quest %u was given up (got %u)",
          called_quest, g_world.abandoned_quest);
    printf("  quest %u abandoned\n", called_quest);

    g_world.abandoned_quest = 0;
    abandon_quest(world_fd, character_id, called_quest);
    check(g_world.abandoned_quest == 0,
          "abandoning it twice is refused rather than confirmed again");

    g_world.abandoned_quest = 0;
    abandon_quest(world_fd, character_id, 4242);
    check(g_world.abandoned_quest == 0, "and so is a quest that does not exist");
    printf("  a repeat and a fabricated abandon were both refused\n");

    /* Back to the Warden. A quest that had been recorded as finished would be
     * gone from this page forever. */
    approach_and_talk(world_fd, character_id, g_world.pos_x, g_world.pos_y, warden);
    check(g_world.page.open, "the Warden opened a conversation again");
    check(option_with_prefix("Then tell me what to do", &option),
          "the Warden still offers to send us on");
    choose(world_fd, character_id, option);
    check(option_with_prefix("Where do I find", &option),
          "and our leader is named on the page again");

    g_world.accepted_quest = 0;
    choose(world_fd, character_id, option);
    check(g_world.accepted_quest == called_quest,
          "the Warden granted quest %u a second time (got %u)",
          called_quest, g_world.accepted_quest);
    printf("  quest %u taken again after being given up\n", g_world.accepted_quest);

    choose(world_fd, character_id, g_world.page.option_ids[0]);   /* "I will go." */

    /* --- The race leader --- */
    step("race leader");
    g_world.progress_quest = 0;
    approach_and_talk(world_fd, character_id, g_world.pos_x, g_world.pos_y, leader);
    check(g_world.page.open, "the leader opened a conversation");
    show_page();

    check(g_world.progress_quest == QUEST_CALLED_BASE + race_id,
          "talking to the leader advanced the quest");
    check(g_world.progress_current >= g_world.progress_required,
          "the objective is satisfied (%d/%d)",
          g_world.progress_current, g_world.progress_required);

    check(g_world.page.option_count == 1,
          "exactly one option passes for a player who was sent here (saw %u)",
          g_world.page.option_count);

    g_world.completed_quest = 0;
    choose(world_fd, character_id, g_world.page.option_ids[0]);
    check(g_world.completed_quest == QUEST_CALLED_BASE + race_id,
          "the leader took the quest in");
    printf("  quest %u turned in for %u xp\n", g_world.completed_quest, g_world.completed_xp);
    show_page();

    g_world.accepted_quest = 0;
    choose(world_fd, character_id, g_world.page.option_ids[0]);
    check(g_world.accepted_quest == QUEST_INTO_ENNARA,
          "the leader granted 'Into Ennara' (got %u)", g_world.accepted_quest);
    show_page();
    {
        const QuestObjectiveInfo* obj = &g_world.accepted_objectives[0];
        check(ntohl(obj->target_id) == NPC_TYPE_GATE_SENTRY,
              "the sendoff points at the Gate Sentry");
        check(obj->has_marker, "the sendoff carries a marker");
        printf("  objective: \"%s\" -> marker (%.0f, %.0f)\n",
               obj->description, obj->marker_x, obj->marker_y);
    }

    choose(world_fd, character_id, g_world.page.option_ids[0]);   /* "I will go." */

    /* --- The gate --- */
    step("Gate Sentry");
    g_world.progress_quest = 0;
    approach_and_talk(world_fd, character_id, g_world.pos_x, g_world.pos_y, sentry);
    check(g_world.page.open, "the Sentry opened a conversation");
    show_page();

    check(g_world.progress_quest == QUEST_INTO_ENNARA,
          "reaching the gate advanced 'Into Ennara'");
    check(g_world.page.option_count == 1,
          "only the hand-in passes at the gate (saw %u)", g_world.page.option_count);

    g_world.completed_quest = 0;
    choose(world_fd, character_id, g_world.page.option_ids[0]);
    check(g_world.completed_quest == QUEST_INTO_ENNARA, "the Sentry took the quest in");
    show_page();
    printf("  released into Ennara for %u xp\n", g_world.completed_xp);

    choose(world_fd, character_id, g_world.page.option_ids[0]);   /* "Thank you." */
    check(!g_world.page.open, "the conversation closed");

    close(world_fd);
    SSL_CTX_free(g_ssl_ctx);

    printf("\n%d checks passed — the %s opening sequence is playable end to end\n",
           g_checks, race_key);
    return 0;
}
