/**
 * @file
 * Fuzz the login server's packet router.
 *
 * The login router is the only one of the three that reaches a stranger: every
 * other server dispatcher runs behind an authenticated session, and this one
 * runs on bytes from whoever opened the socket. It is also the router with the
 * most string handling -- it terminates a username, a password, an email and a
 * birthday in place, in the packet, after a length check written by hand next
 * to each case.
 *
 * See routes_fuzz.h for what is being asserted and why the fixtures are
 * exactly-sized heap blocks.
 */

#include "routes_fuzz.h"
#include "log.h"
#include "types.h"

#include "routes.h"

/* --- Stubs ---------------------------------------------------------------
 *
 * Each one records the buffer length the router was willing to hand it. None
 * of them touches a socket or a database; what is under test is the router's
 * arithmetic, not what the handlers do afterwards.
 */

void auth_handle_login(int client_fd, AuthLoginPacket* packet) {
    (void)client_fd;
    fz_entered("auth_handle_login", PACKET_AUTH_LOGIN, sizeof(AuthLoginPacket));
    /* Read the fields the real handler reads, so a router that admitted a
     * short packet is a sanitizer report rather than a silent pass. */
    volatile char sink = packet->username[0];
    sink = packet->password[sizeof(packet->password) - 1];
    (void)sink;
}

void auth_handle_register(int client_fd, AuthRegisterPacket* packet) {
    (void)client_fd;
    fz_entered("auth_handle_register", PACKET_AUTH_REGISTER, sizeof(AuthRegisterPacket));
    volatile char sink = packet->username[0];
    sink = packet->birthday[sizeof(packet->birthday) - 1];
    (void)sink;
}

void auth_handle_start_game(int client_fd, StartGameRequestPacket* packet) {
    (void)client_fd;
    fz_entered("auth_handle_start_game", PACKET_START_GAME_REQUEST,
               sizeof(StartGameRequestPacket));
    volatile uint32_t sink = packet->player_id;
    (void)sink;
}

/* The only handler the router hands a raw (buffer, length) pair to, so it is
 * the only one that can be told a length. It self-checks past the header. */
static size_t g_patch_notes_bytes = 0;
void handle_patch_notes_request(int client_fd, void* packet_data, ssize_t bytes) {
    (void)client_fd; (void)packet_data;
    g_patch_notes_bytes = (size_t)bytes;
    fz_entered("handle_patch_notes_request", PATCH_NOTES_REQUEST, (size_t)bytes);
}

/* --- The corpus ---------------------------------------------------------- */

/** Hand the router one exactly-sized buffer. */
static void feed(uint8_t opcode, size_t len, int fill) {
    uint8_t* buf = fz_fixture(opcode, len, fill);
    route_packet(3, buf, (ssize_t)len);
    free(buf);
}

int main(void) {
    printf("=== login packet router ===\n");

    /* Without this the level stays at INFO and the routers narrate every one
     * of the several hundred thousand packets below, which is most of the
     * runtime. The Makefile also sets MMO_LOG_LEVEL=error; this is what makes
     * it take effect. */
    log_init();

    /* The floor for each opcode, written from protocol.h rather than from the
     * router. A router that drops a length check does not get to drop the
     * expectation with it. */
    fz_declare(PACKET_AUTH_LOGIN,          sizeof(AuthLoginPacket));
    fz_declare(PACKET_AUTH_REGISTER,       AUTH_REGISTER_SIZE);
    fz_declare(PACKET_START_GAME_REQUEST,  sizeof(StartGameRequestPacket));
    fz_declare(PATCH_NOTES_REQUEST,        MIN_HEADER_SIZE);

    /* AUTH_REGISTER_SIZE is a hand-written constant standing in for a struct
     * size, and the router terminates four strings inside that struct straight
     * after comparing against it. If the two ever disagree in the wrong
     * direction, those four writes go past the buffer. */
    printf("\nTEST 1: the hand-written register size still matches the struct\n");
    printf("  AUTH_REGISTER_SIZE=%d sizeof(AuthRegisterPacket)=%zu\n",
           (int)AUTH_REGISTER_SIZE, sizeof(AuthRegisterPacket));
    FZ_CHECK((size_t)AUTH_REGISTER_SIZE >= sizeof(AuthRegisterPacket),
             "the register length check covers the whole struct");

    printf("\nTEST 2: every opcode, truncated at every length up to 256 bytes\n");
    for (int opcode = 0; opcode <= 255; opcode++)
        for (size_t len = 0; len <= 256; len++)
            feed((uint8_t)opcode, len, 0);
    FZ_CHECK(fz_violations == 0, "no handler was entered below its floor");

    printf("\nTEST 3: the same lengths filled with arbitrary bytes\n");
    /* The zeroed pass above always presents a self-consistent payload_size.
     * This one does not, which is the case a router that trusts the declared
     * length over the received length gets wrong. */
    int before = fz_violations;
    for (int round = 0; round < 40; round++)
        for (int opcode = 0; opcode <= 255; opcode++)
            for (size_t len = 0; len <= 64; len++)
                feed((uint8_t)opcode, len, 1);
    FZ_CHECK(fz_violations == before, "arbitrary payloads change nothing");

    printf("\nTEST 4: a well-formed packet of each kind does reach its handler\n");
    /* Without this the run above would pass just as well against a router that
     * dispatches nothing at all. */
    memset(fz_reached, 0, sizeof(fz_reached));
    feed(PACKET_AUTH_LOGIN,         sizeof(AuthLoginPacket), 0);
    feed(PACKET_AUTH_REGISTER,      sizeof(AuthRegisterPacket), 0);
    feed(PACKET_START_GAME_REQUEST, sizeof(StartGameRequestPacket), 0);
    feed(PATCH_NOTES_REQUEST,       sizeof(PacketHeader), 0);

    FZ_CHECK(fz_reached[PACKET_AUTH_LOGIN],         "a full login reaches auth_handle_login");
    FZ_CHECK(fz_reached[PACKET_AUTH_REGISTER],      "a full register reaches auth_handle_register");
    FZ_CHECK(fz_reached[PACKET_START_GAME_REQUEST], "a full start-game reaches auth_handle_start_game");
    FZ_CHECK(fz_reached[PATCH_NOTES_REQUEST],       "a header-sized patch-notes request reaches its handler");

    printf("\nTEST 5: the patch-notes handler is told the real length\n");
    /* It is the one handler that receives a length rather than a typed packet,
     * so it is the one that can be lied to. */
    g_patch_notes_bytes = 0;
    feed(PATCH_NOTES_REQUEST, 41, 0);
    printf("  handler was told %zu bytes for a 41-byte packet\n", g_patch_notes_bytes);
    FZ_CHECK(g_patch_notes_bytes == 41, "the length passed through is the length received");

    if (fz_violations)
        printf("\n%d floor violation(s)\n", fz_violations);

    if (fz_failures || fz_violations) {
        printf("\n=== login packet router: FAILED ===\n");
        return 1;
    }
    printf("\n=== login packet router: all checks passed ===\n");
    return 0;
}
