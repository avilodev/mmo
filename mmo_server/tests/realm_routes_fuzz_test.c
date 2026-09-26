/**
 * @file
 * Fuzz the realm server's packet router.
 *
 * Every opcode this router dispatches is a PostgreSQL round trip against a
 * character table, and three of them read fields straight out of the buffer
 * after a hand-written length check -- including one that terminates a
 * character name in place before passing it to strlen().
 *
 * See routes_fuzz.h for what is being asserted and why the fixtures are
 * exactly-sized heap blocks.
 */

#include "routes_fuzz.h"
#include "log.h"
#include "types.h"

#include "routes.h"
#include "packet_limiter.h"
#include "limit_profiles.h"

#include <fcntl.h>
#include <sys/socket.h>

/* --- Stubs --------------------------------------------------------------- */

void handle_race_list_request(int client_fd, uint32_t account_id) {
    (void)client_fd; (void)account_id;
    fz_entered("handle_race_list_request", PACKET_RACE_LIST_REQUEST, sizeof(PacketHeader));
}

void handle_character_list_request(int client_fd, uint32_t account_id, uint32_t world_id) {
    (void)client_fd; (void)account_id; (void)world_id;
    fz_entered("handle_character_list_request", PACKET_CHARACTER_LIST_REQUEST,
               sizeof(CharacterListRequestPacket));
}

/* The name arrives already terminated by the router, which wrote the
 * terminator into the buffer. strlen() here is the same read the real handler
 * makes, and it is what runs off the end if the router admitted a short one. */
static size_t g_last_name_len = 0;
void handle_character_create_request(int client_fd, uint32_t account_id,
                                     uint32_t world_id, const char* name,
                                     int class_id, int race_id) {
    (void)client_fd; (void)account_id; (void)world_id; (void)class_id; (void)race_id;
    fz_entered("handle_character_create_request", PACKET_CHARACTER_CREATE_REQUEST,
               sizeof(CharacterCreateRequestPacket));
    g_last_name_len = strlen(name);
}

void handle_character_delete_request(int client_fd, uint32_t account_id,
                                     uint32_t character_id, uint32_t world_id) {
    (void)client_fd; (void)account_id; (void)character_id; (void)world_id;
    fz_entered("handle_character_delete_request", PACKET_CHARACTER_DELETE_REQUEST,
               sizeof(CharacterDeleteRequestPacket));
}

void world_send_list(int client_fd, uint32_t account_id) {
    (void)client_fd; (void)account_id;
    fz_entered("world_send_list", PACKET_WORLD_LIST_REQUEST, sizeof(PacketHeader));
}

/* world_enter() is handed the raw pair and self-checks, so the router's only
 * promise is a header. What must hold is that the length it is told is the
 * length that exists. */
static size_t g_enter_bytes = 0;
void world_enter(int client_fd, uint32_t account_id, uint8_t* buffer, ssize_t bytes) {
    (void)client_fd; (void)account_id;
    g_enter_bytes = (size_t)bytes;
    fz_entered("world_enter", PACKET_ENTER_WORLD, (size_t)bytes);
    /* Touch both ends of what it was told it has. */
    if (bytes > 0) { volatile uint8_t s = buffer[0]; s = buffer[bytes - 1]; (void)s; }
}

/* --- The corpus ---------------------------------------------------------- */

/** A socket pair, because the ping case and the limiter replies call send().
 *
 * Non-blocking, and the far end is never read: a corpus this size fills the
 * peer's receive buffer within the first few hundred packets, and a blocking
 * send() then parks the test forever waiting for a reader that does not exist.
 * What is under test is the router, not delivery, so EAGAIN is the right
 * answer -- and it is the same answer the router gets in production from a
 * client that has stopped reading.
 */
static int g_fd = -1;

/** Stand in for the realm's TLS write.
 *
 * The realm answers over TLS now, and its route table reaches the session
 * through a thread-local the reactor binds -- neither of which exists here, and
 * neither of which is what a router fuzzer is about. What matters is that a
 * reply still goes somewhere that can refuse it, so the EAGAIN path stays
 * reachable, which is what the socketpair above provides.
 */
ssize_t tls_send(int fd, const void* buf, size_t len, int flags);
ssize_t tls_send(int fd, const void* buf, size_t len, int flags) {
    return send(fd, buf, len, flags | MSG_NOSIGNAL);
}

static void open_sink(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { perror("socketpair"); exit(1); }
    g_fd = sv[0];
    fcntl(g_fd,  F_SETFL, fcntl(g_fd,  F_GETFL, 0) | O_NONBLOCK);
    fcntl(sv[1], F_SETFL, fcntl(sv[1], F_GETFL, 0) | O_NONBLOCK);
}

static int feed(uint8_t opcode, size_t len, int fill) {
    uint8_t* buf = fz_fixture(opcode, len, fill);
    int r = process_packet(g_fd, 4242, buf, (ssize_t)len);
    free(buf);
    return r;
}

/** Re-arm the limiter so a long corpus is not entirely rate-limited away. */
static void refresh_budget(void) { packet_limiter_reset(g_fd); }

int main(void) {
    printf("=== realm packet router ===\n");

    /* Without this the level stays at INFO and the routers narrate every one
     * of the several hundred thousand packets below, which is most of the
     * runtime. The Makefile also sets MMO_LOG_LEVEL=error; this is what makes
     * it take effect. */
    log_init();

    open_sink();
    packet_limiter_init(limit_profile_realm());
    packet_limiter_reset(g_fd);

    fz_declare(PACKET_RACE_LIST_REQUEST,       sizeof(PacketHeader));
    fz_declare(PACKET_CHARACTER_LIST_REQUEST,  sizeof(CharacterListRequestPacket));
    fz_declare(PACKET_CHARACTER_CREATE_REQUEST, sizeof(CharacterCreateRequestPacket));
    fz_declare(PACKET_CHARACTER_DELETE_REQUEST, sizeof(CharacterDeleteRequestPacket));
    fz_declare(PACKET_WORLD_LIST_REQUEST,      sizeof(PacketHeader));
    fz_declare(PACKET_ENTER_WORLD,             sizeof(PacketHeader));

    printf("\nTEST 1: every opcode, truncated at every length up to 256 bytes\n");
    for (int opcode = 0; opcode <= 255; opcode++) {
        refresh_budget();
        for (size_t len = 0; len <= 256; len++) {
            /* The limiter would otherwise swallow most of the corpus; what is
             * under test here is the router past it. */
            if ((len & 7) == 0) refresh_budget();
            feed((uint8_t)opcode, len, 0);
        }
    }
    FZ_CHECK(fz_violations == 0, "no handler was entered below its floor");

    printf("\nTEST 2: the same lengths filled with arbitrary bytes\n");
    int before = fz_violations;
    for (int round = 0; round < 20; round++)
        for (int opcode = 0; opcode <= 255; opcode++) {
            refresh_budget();
            for (size_t len = 0; len <= 64; len++) {
                if ((len & 7) == 0) refresh_budget();
                feed((uint8_t)opcode, len, 1);
            }
        }
    FZ_CHECK(fz_violations == before, "arbitrary payloads change nothing");

    printf("\nTEST 3: a well-formed packet of each kind does reach its handler\n");
    memset(fz_reached, 0, sizeof(fz_reached));
    refresh_budget(); feed(PACKET_RACE_LIST_REQUEST,        sizeof(PacketHeader), 0);
    refresh_budget(); feed(PACKET_CHARACTER_LIST_REQUEST,   sizeof(CharacterListRequestPacket), 0);
    refresh_budget(); feed(PACKET_CHARACTER_CREATE_REQUEST, sizeof(CharacterCreateRequestPacket), 0);
    refresh_budget(); feed(PACKET_CHARACTER_DELETE_REQUEST, sizeof(CharacterDeleteRequestPacket), 0);
    refresh_budget(); feed(PACKET_WORLD_LIST_REQUEST,       sizeof(PacketHeader), 0);
    refresh_budget(); feed(PACKET_ENTER_WORLD,              sizeof(EnterWorldPacket), 0);

    FZ_CHECK(fz_reached[PACKET_RACE_LIST_REQUEST],        "a race list request is dispatched");
    FZ_CHECK(fz_reached[PACKET_CHARACTER_LIST_REQUEST],   "a character list request is dispatched");
    FZ_CHECK(fz_reached[PACKET_CHARACTER_CREATE_REQUEST], "a character create request is dispatched");
    FZ_CHECK(fz_reached[PACKET_CHARACTER_DELETE_REQUEST], "a character delete request is dispatched");
    FZ_CHECK(fz_reached[PACKET_WORLD_LIST_REQUEST],       "a world list request is dispatched");
    FZ_CHECK(fz_reached[PACKET_ENTER_WORLD],              "an enter-world request is dispatched");

    printf("\nTEST 4: a name filling its whole field is still terminated\n");
    /* The router writes the terminator into the last byte of the field, so a
     * name that fills the field is the case where strlen() would otherwise run
     * into class_id and beyond. */
    {
        size_t n = sizeof(CharacterCreateRequestPacket);
        uint8_t* buf = fz_fixture(PACKET_CHARACTER_CREATE_REQUEST, n, 0);
        CharacterCreateRequestPacket* req = (CharacterCreateRequestPacket*)buf;
        memset(req->name, 'A', sizeof(req->name));
        g_last_name_len = SIZE_MAX;
        refresh_budget();
        process_packet(g_fd, 4242, buf, (ssize_t)n);
        free(buf);
        printf("  name length seen by the handler: %zu (field is %zu)\n",
               g_last_name_len, sizeof(req->name));
        FZ_CHECK(g_last_name_len == sizeof(((CharacterCreateRequestPacket*)0)->name) - 1,
                 "an unterminated name is truncated to fit its field");
    }

    printf("\nTEST 5: enter-world is told the length it actually has\n");
    g_enter_bytes = 0;
    refresh_budget();
    feed(PACKET_ENTER_WORLD, 33, 0);
    printf("  handler was told %zu bytes for a 33-byte packet\n", g_enter_bytes);
    FZ_CHECK(g_enter_bytes == 33, "the length passed through is the length received");

    printf("\nTEST 6: a sustained flood is answered with a disconnect, not a crash\n");
    {
        packet_limiter_reset(g_fd);
        int kicked = 0;
        for (int i = 0; i < 5000 && !kicked; i++)
            if (feed(PACKET_CHARACTER_CREATE_REQUEST,
                     sizeof(CharacterCreateRequestPacket), 0) == -1) kicked = 1;
        printf("  the router asked for a disconnect: %s\n", kicked ? "yes" : "no");
        FZ_CHECK(kicked, "sustained abuse ends the connection");
    }

    if (fz_violations)
        printf("\n%d floor violation(s)\n", fz_violations);

    if (fz_failures || fz_violations) {
        printf("\n=== realm packet router: FAILED ===\n");
        return 1;
    }
    printf("\n=== realm packet router: all checks passed ===\n");
    return 0;
}
