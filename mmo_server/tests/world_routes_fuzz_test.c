/**
 * @file
 * Fuzz the world server's packet router.
 *
 * The largest of the three, and the one that runs on the hot path: thirty-odd
 * opcodes, a rate limiter, a dead-player gate, and a mix of handlers -- some
 * handed a typed packet after the router checked its length, some handed the
 * raw (buffer, bytes) pair to check for themselves. The bug this looks for is
 * one of the first kind missing its check.
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
#include "npc_world.h"
#include "player_data.h"

#include <fcntl.h>
#include <sys/socket.h>

/* --- Stubs ---------------------------------------------------------------
 *
 * The world's subsystems -- the player registry, the NPC pool, the database,
 * the broadcast threads -- are not linked. Every handler is replaced by a
 * recorder. What is under test is the router's arithmetic.
 */

NPCWorld g_npc_world;

/* No player is ever live, so the dead-player gate falls through to dispatch
 * and every opcode reaches its case. A registry that answered would make the
 * gate the thing under test instead of the router. */
ActivePlayer* player_acquire(uint32_t character_id) { (void)character_id; return NULL; }
ActivePlayer* player_acquire_hint(uint32_t character_id, int slot) {
    (void)character_id; (void)slot; return NULL;
}
void player_release(ActivePlayer* p) { (void)p; }
void player_send_stats_locked(int fd, ActivePlayer* p) { (void)fd; (void)p; }
void ability_send_data(int fd, ActivePlayer* p) { (void)fd; (void)p; }

ssize_t connection_io_send(int fd, const void* data, size_t len) {
    return send(fd, data, len, MSG_NOSIGNAL);
}

/* Handlers the router hands a typed packet to, after checking its length.
 * These are the ones the floor table is about. */
void handle_ping(int fd, uint8_t* buffer, ssize_t bytes, uint32_t c, int slot) {
    (void)fd; (void)c; (void)slot;
    fz_entered("handle_ping", PACKET_PING, (size_t)bytes);
    /* The real one reads the latency field and echoes `bytes` back. It reads
     * it with memcpy, because PacketHeader is 7 bytes and a uint16_t* cast at
     * that offset claims an alignment that is not there; this mirrors it. */
    if (bytes >= (ssize_t)(sizeof(PacketHeader) + sizeof(uint16_t))) {
        uint16_t raw = 0;
        memcpy(&raw, buffer + sizeof(PacketHeader), sizeof(raw));
        volatile uint16_t s = raw;
        (void)s;
    }
    if (bytes > 0) { volatile uint8_t s = buffer[bytes - 1]; (void)s; }
}

void handle_player_move(int fd, uint32_t c, int slot, PlayerMovePacket* pkt) {
    (void)fd; (void)c; (void)slot;
    fz_entered("handle_player_move", PACKET_PLAYER_MOVE, sizeof(PlayerMovePacket));
    volatile float s = pkt->vel_y; (void)s;
}

void combat_handle_attack_intent(NPCWorld* w, int fd, uint32_t c, AttackIntentPacket* pkt) {
    (void)w; (void)fd; (void)c;
    fz_entered("combat_handle_attack_intent", PACKET_ATTACK_INTENT, sizeof(AttackIntentPacket));
    volatile float s = pkt->aim_y; (void)s;
}

void ability_handle_cast_intent(NPCWorld* w, int fd, uint32_t c, AbilityCastIntentPacket* pkt) {
    (void)w; (void)fd; (void)c;
    fz_entered("ability_handle_cast_intent", PACKET_ABILITY_CAST_INTENT,
               sizeof(AbilityCastIntentPacket));
    volatile float s = pkt->aim_y; (void)s;
}

void ability_handle_form_swap(int fd, uint32_t c, uint8_t form) {
    (void)fd; (void)c; (void)form;
    fz_entered("ability_handle_form_swap", PACKET_FORM_SWAP, sizeof(FormSwapPacket));
}

int quest_player_abandon(uint32_t c, int fd, uint32_t quest_id) {
    (void)c; (void)fd; (void)quest_id;
    fz_entered("quest_player_abandon", PACKET_QUEST_ABANDON, sizeof(QuestAbandonPacket));
    return 1;
}

/* Handlers with no payload to check at all. Their floor is a bare header. */
void handle_request_player_data(int fd, uint32_t c) {
    (void)fd; (void)c;
    fz_entered("handle_request_player_data", PACKET_REQUEST_PLAYER_DATA, sizeof(PacketHeader));
}
void combat_handle_cast_cancel(int fd, uint32_t c) {
    (void)fd; (void)c;
    fz_entered("combat_handle_cast_cancel", PACKET_CAST_CANCEL, sizeof(PacketHeader));
}
void ability_handle_cast_cancel(int fd, uint32_t c) {
    (void)fd; (void)c;
    fz_entered("ability_handle_cast_cancel", PACKET_ABILITY_CAST_CANCEL, sizeof(PacketHeader));
}
void handle_dialogue_close(uint32_t c) {
    (void)c;
    fz_entered("handle_dialogue_close", PACKET_DIALOGUE_CLOSE, sizeof(PacketHeader));
}
void handle_party_accept(int fd, uint32_t c) {
    (void)fd; (void)c;
    fz_entered("handle_party_accept", PACKET_PARTY_ACCEPT, sizeof(PacketHeader));
}
void handle_party_decline(int fd, uint32_t c) {
    (void)fd; (void)c;
    fz_entered("handle_party_decline", PACKET_PARTY_DECLINE, sizeof(PacketHeader));
}
void handle_party_leave(int fd, uint32_t c) {
    (void)fd; (void)c;
    fz_entered("handle_party_leave", PACKET_PARTY_LEAVE, sizeof(PacketHeader));
}

/* Handlers given the raw pair, which check for themselves. The router's only
 * promise is a header -- but it must also tell them the truth about the
 * length, so each of these reads both ends of what it was told it has. */
static void raw_handler(const char* name, uint8_t opcode,
                        const uint8_t* buffer, ssize_t bytes) {
    fz_entered(name, opcode, (size_t)bytes);
    if (bytes > 0) {
        volatile uint8_t s = buffer[0];
        s = buffer[bytes - 1];
        (void)s;
    }
}

#define RAW(fn, opcode)                                                        \
    void fn(int fd, uint32_t c, uint8_t* buffer, ssize_t bytes) {              \
        (void)fd; (void)c;                                                     \
        raw_handler(#fn, (opcode), buffer, bytes);                             \
    }

RAW(handle_equip_item,             PACKET_EQUIP_ITEM)
RAW(handle_unequip_item,           PACKET_UNEQUIP_ITEM)
RAW(handle_use_item,               PACKET_USE_ITEM)
RAW(handle_drop_item,              PACKET_DROP_ITEM)
RAW(handle_move_item,              PACKET_MOVE_ITEM)
RAW(handle_party_invite,           PACKET_PARTY_INVITE)
RAW(handle_party_kick,             PACKET_PARTY_KICK)
RAW(handle_npc_interact_request,   PACKET_NPC_INTERACT_REQUEST)
RAW(handle_dialogue_option_select, PACKET_DIALOGUE_OPTION_SELECT)
RAW(chat_handle_send,              PACKET_CHAT_SEND)

/* The remaining raw handlers have their own argument orders. */
void loot_handle_pickup_request(uint32_t c, int fd, const uint8_t* buffer, ssize_t bytes) {
    (void)c; (void)fd;
    raw_handler("loot_handle_pickup_request", PACKET_LOOT_PICKUP_REQUEST, buffer, bytes);
}
void shop_handle_buy(uint32_t c, int fd, uint8_t* buffer, int bytes) {
    (void)c; (void)fd;
    raw_handler("shop_handle_buy", PACKET_SHOP_BUY, buffer, bytes);
}
void shop_handle_sell(uint32_t c, int fd, uint8_t* buffer, int bytes) {
    (void)c; (void)fd;
    raw_handler("shop_handle_sell", PACKET_SHOP_SELL, buffer, bytes);
}
void handle_session_list_request(int fd, uint8_t* buffer, ssize_t bytes) {
    (void)fd;
    raw_handler("handle_session_list_request", PACKET_SESSION_LIST_REQUEST, buffer, bytes);
}
void handle_name_query_request(int fd, uint8_t* buffer, ssize_t bytes) {
    (void)fd;
    raw_handler("handle_name_query_request", PACKET_NAME_QUERY_REQUEST, buffer, bytes);
}

/* --- The corpus ---------------------------------------------------------- */

/** A non-blocking socket pair; see the note in the realm fuzzer. */
static int g_fd = -1;

static void open_sink(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { perror("socketpair"); exit(1); }
    g_fd = sv[0];
    fcntl(g_fd,  F_SETFL, fcntl(g_fd,  F_GETFL, 0) | O_NONBLOCK);
    fcntl(sv[1], F_SETFL, fcntl(sv[1], F_GETFL, 0) | O_NONBLOCK);
}

/** The session identity every fixture is dispatched under. */
#define FUZZ_CHARACTER_ID 777u

static int feed(uint8_t opcode, size_t len, int fill) {
    uint8_t* buf = fz_fixture(opcode, len, fill);
    int r = process_packet(g_fd, FUZZ_CHARACTER_ID, -1, (ssize_t)len, buf);
    free(buf);
    return r;
}

/**
 * Feed a packet that also claims the session's own character id.
 *
 * Three opcodes -- attack, ability cast and form swap -- carry the character
 * id in the header and refuse the packet when it does not match the session's.
 * That is the router doing its job, so a fixture that wants to reach those
 * handlers has to say who it is.
 */
static int feed_as_self(uint8_t opcode, size_t len) {
    uint8_t* buf = fz_fixture(opcode, len, 0);
    if (len >= sizeof(PacketHeader))
        ((PacketHeader*)buf)->player_id = htonl(FUZZ_CHARACTER_ID);
    int r = process_packet(g_fd, FUZZ_CHARACTER_ID, -1, (ssize_t)len, buf);
    free(buf);
    return r;
}

static void refresh_budget(void) { packet_limiter_reset(g_fd); }

int main(void) {
    printf("=== world packet router ===\n");
    log_init();

    open_sink();
    packet_limiter_init(limit_profile_world());
    refresh_budget();

    /* Written from protocol.h. Every opcode the router checks a length for
     * gets its packet's size; every opcode handed the raw pair gets a bare
     * header, because that is all the router promises those. */
    fz_declare(PACKET_PING,                  sizeof(PacketHeader) + sizeof(uint16_t));
    fz_declare(PACKET_REQUEST_PLAYER_DATA,   sizeof(PacketHeader));
    fz_declare(PACKET_PLAYER_MOVE,           sizeof(PlayerMovePacket));
    fz_declare(PACKET_ATTACK_INTENT,         sizeof(AttackIntentPacket));
    fz_declare(PACKET_CAST_CANCEL,           sizeof(PacketHeader));
    fz_declare(PACKET_ABILITY_CAST_INTENT,   sizeof(AbilityCastIntentPacket));
    fz_declare(PACKET_ABILITY_CAST_CANCEL,   sizeof(PacketHeader));
    fz_declare(PACKET_FORM_SWAP,             sizeof(FormSwapPacket));
    fz_declare(PACKET_QUEST_ABANDON,         sizeof(QuestAbandonPacket));
    fz_declare(PACKET_DIALOGUE_CLOSE,        sizeof(PacketHeader));
    fz_declare(PACKET_PARTY_ACCEPT,          sizeof(PacketHeader));
    fz_declare(PACKET_PARTY_DECLINE,         sizeof(PacketHeader));
    fz_declare(PACKET_PARTY_LEAVE,           sizeof(PacketHeader));

    fz_declare(PACKET_EQUIP_ITEM,            sizeof(PacketHeader));
    fz_declare(PACKET_UNEQUIP_ITEM,          sizeof(PacketHeader));
    fz_declare(PACKET_USE_ITEM,              sizeof(PacketHeader));
    fz_declare(PACKET_DROP_ITEM,             sizeof(PacketHeader));
    fz_declare(PACKET_MOVE_ITEM,             sizeof(PacketHeader));
    fz_declare(PACKET_PARTY_INVITE,          sizeof(PacketHeader));
    fz_declare(PACKET_PARTY_KICK,            sizeof(PacketHeader));
    fz_declare(PACKET_NPC_INTERACT_REQUEST,  sizeof(PacketHeader));
    fz_declare(PACKET_DIALOGUE_OPTION_SELECT, sizeof(PacketHeader));
    fz_declare(PACKET_CHAT_SEND,             sizeof(PacketHeader));
    fz_declare(PACKET_LOOT_PICKUP_REQUEST,   sizeof(PacketHeader));
    fz_declare(PACKET_SHOP_BUY,              sizeof(PacketHeader));
    fz_declare(PACKET_SHOP_SELL,             sizeof(PacketHeader));
    fz_declare(PACKET_SESSION_LIST_REQUEST,  sizeof(PacketHeader));
    fz_declare(PACKET_NAME_QUERY_REQUEST,    sizeof(PacketHeader));

    printf("\nTEST 1: every opcode, truncated at every length up to 256 bytes\n");
    for (int opcode = 0; opcode <= 255; opcode++) {
        refresh_budget();
        for (size_t len = 0; len <= 256; len++) {
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
            for (size_t len = 0; len <= 96; len++) {
                if ((len & 7) == 0) refresh_budget();
                feed((uint8_t)opcode, len, 1);
            }
        }
    FZ_CHECK(fz_violations == before, "arbitrary payloads change nothing");

    printf("\nTEST 3: a full-size packet of every declared opcode reaches a handler\n");
    /* Without this the corpus above would pass against a router that dispatches
     * nothing. MAX_PACKET_SIZE is comfortably past every packet's size, so one
     * length covers all of them. */
    memset(fz_reached, 0, sizeof(fz_reached));
    for (int opcode = 0; opcode <= 255; opcode++) {
        if (!fz_floor[opcode]) continue;
        refresh_budget();
        feed_as_self((uint8_t)opcode, 512);
    }
    int declared = 0, arrived = 0;
    for (int opcode = 0; opcode <= 255; opcode++) {
        if (!fz_floor[opcode]) continue;
        declared++;
        if (fz_reached[opcode]) arrived++;
        else printf("  opcode %d has a floor but never reached a handler\n", opcode);
    }
    printf("  %d of %d declared opcodes were dispatched\n", arrived, declared);
    FZ_CHECK(arrived == declared, "every declared opcode is dispatched when whole");

    printf("\nTEST 4: a packet claiming another character is refused\n");
    /* The three opcodes that carry an identity check it against the session's.
     * A client that could act as another character by writing their id into a
     * header would be a complete authority failure, so the check is worth an
     * assertion of its own rather than being implied by the pass above. */
    {
        static const uint8_t identity_checked[] = {
            PACKET_ATTACK_INTENT, PACKET_ABILITY_CAST_INTENT, PACKET_FORM_SWAP,
        };
        for (size_t i = 0; i < sizeof(identity_checked) / sizeof(identity_checked[0]); i++) {
            uint8_t opcode = identity_checked[i];
            memset(fz_reached, 0, sizeof(fz_reached));

            refresh_budget();
            uint8_t* buf = fz_fixture(opcode, 512, 0);
            ((PacketHeader*)buf)->player_id = htonl(FUZZ_CHARACTER_ID + 1);
            process_packet(g_fd, FUZZ_CHARACTER_ID, -1, 512, buf);
            free(buf);

            char what[96];
            snprintf(what, sizeof(what),
                     "opcode %u from another character's id is dropped", opcode);
            FZ_CHECK(!fz_reached[opcode], what);

            refresh_budget();
            feed_as_self(opcode, 512);
            snprintf(what, sizeof(what), "opcode %u under its own id is dispatched", opcode);
            FZ_CHECK(fz_reached[opcode], what);
        }
    }

    printf("\nTEST 5: LOGOUT ends the connection and reads nothing\n");
    refresh_budget();
    FZ_CHECK(feed(PACKET_LOGOUT, sizeof(PacketHeader), 0) == -1,
             "a logout asks the caller to close");

    printf("\nTEST 6: a sustained flood is answered with a disconnect, not a crash\n");
    {
        refresh_budget();
        int kicked = 0;
        for (int i = 0; i < 20000 && !kicked; i++)
            if (feed(PACKET_CHAT_SEND, 64, 0) == -1) kicked = 1;
        printf("  the router asked for a disconnect: %s\n", kicked ? "yes" : "no");
        FZ_CHECK(kicked, "sustained abuse ends the connection");
    }

    if (fz_violations)
        printf("\n%d floor violation(s)\n", fz_violations);

    if (fz_failures || fz_violations) {
        printf("\n=== world packet router: FAILED ===\n");
        return 1;
    }
    printf("\n=== world packet router: all checks passed ===\n");
    return 0;
}
