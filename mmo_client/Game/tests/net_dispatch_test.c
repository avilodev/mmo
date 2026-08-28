/**
 * @file
 * Feed malformed packets to the client's four packet dispatchers.
 *
 * net_session.c, net_world.c, net_combat.c and net_inventory.c are the only
 * client code that parses bytes chosen by someone else. Nothing tested them:
 * they were reachable only through a Winsock build that ran on one developer's
 * machine, and a length check missed in one of the seventy-odd cases would be
 * found by a hostile server rather than by a build.
 *
 * What this pins down is the property that actually matters for untrusted
 * input: no packet, at any length, with any declared count, may read or write
 * outside a buffer. Run under AddressSanitizer, which is what turns "it did not
 * crash" into a real answer.
 *
 * It deliberately does NOT assert on what the handlers do with a well-formed
 * packet -- that is gameplay, and it changes. It asserts that they survive.
 */

#include "net_internal.h"
#include "inventory.h"
#include "ability_bar.h"
#include "combat_system.h"
#include "ui/quest_log.h"
#include "ui/friends_panel.h"
#include "state_handler.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- The state the dispatchers write into ------------------------------- */

static GameState     g_game;
static PlayingState  g_playing;
static InventoryState g_inventory;

/** Build the minimum live game a dispatcher expects to find.
 *
 * A NULL g_current_game would make most handlers return early, which would
 * test nothing: the interesting paths are the ones that copy packet contents
 * into these arrays.
 */
static void reset_game(void) {
    memset(&g_game, 0, sizeof(g_game));
    memset(&g_playing, 0, sizeof(g_playing));
    memset(&g_inventory, 0, sizeof(g_inventory));

    g_game.playing   = &g_playing;
    g_game.inventory = &g_inventory;
    g_game.mode      = GAME_MODE_PLAYING;
    g_current_game   = &g_game;
}

/* The stubs the dispatchers call out to live in hostcompat/net_stubs.c, so the
 * connection test can share them rather than keeping a second copy that drifts.
 * This test does not link network.c, so it owns the context and the clock. */
#define NET_STUBS_OWN_CONTEXT
/* The real inventory model is linked here, so its stub must not be. */
#define NET_STUBS_REAL_INVENTORY
#include "hostcompat/net_stubs.c"

/* inventory.c holds the bag and draws it in the same file, so linking the
 * model brings the renderer with it. These are the four drawing calls that
 * costs, doing nothing. */
#include "hostcompat/render_stubs.c"

/* --- The harness --------------------------------------------------------- */

/** Hand one buffer to every dispatcher, whichever one claims it. */
static void dispatch_all(uint8_t type, const char* data, int length) {
    if (net_dispatch_session(type, data, length))   return;
    if (net_dispatch_world(type, data, length))     return;
    if (net_dispatch_combat(type, data, length))    return;
    if (net_dispatch_inventory(type, data, length)) return;
    if (net_dispatch_friends(type, data, length))   return;
    /* An unclaimed opcode is fine: the real dispatcher logs and drops it. */
}

/** The largest packet any handler reads, so fixtures are never short by accident. */
#define FIXTURE_MAX 65536

static uint8_t g_buf[FIXTURE_MAX];

/**
 * Every opcode, at every length from nothing to a little past its header.
 *
 * The bug class this is looking for is a handler that reads a field before
 * checking that the field arrived.
 */
static void test_every_opcode_at_every_short_length(void) {
    printf("every opcode, truncated at every length up to 96 bytes\n");

    for (int opcode = 0; opcode <= 255; opcode++) {
        for (int len = 0; len <= 96; len++) {
            reset_game();

            /* Fill with a recognisable pattern rather than zeroes: a zero
             * count or a zero length hides an unchecked multiply. */
            for (int i = 0; i < len; i++) g_buf[i] = (uint8_t)(0xA5 ^ (i * 31));

            PacketHeader* h = (PacketHeader*)g_buf;
            if (len >= (int)sizeof(PacketHeader)) {
                h->type = (uint8_t)opcode;
                h->player_id = htonl(1);
                h->payload_size = htons((uint16_t)(len - sizeof(PacketHeader)));
            }

            dispatch_all((uint8_t)opcode, (const char*)g_buf, len);
        }
    }
    printf("  ok   256 opcodes x 97 lengths dispatched without a fault\n");
}

/**
 * Count-bearing packets whose declared count is far past what they carry.
 *
 * These are the packets whose payload length is computed from a byte the
 * sender chose. A handler that trusts it walks off the end of the packet; one
 * that clamps it without re-checking the length walks off the end of its own
 * array.
 */
static void test_declared_counts_are_not_trusted(void) {
    printf("count-bearing packets with impossible counts\n");

    const uint8_t counted[] = {
        PACKET_NPC_POSITIONS,
        PACKET_PLAYER_POSITIONS,
        PACKET_PARTY_UPDATE,
        PACKET_PROJECTILE_UPDATE,
        PACKET_ABILITY_DATA,
        PACKET_ATTACK_RESULT,
        PACKET_WORLD_LIST_RESPONSE,
        PACKET_CHARACTER_LIST_RESPONSE,
        PACKET_RACE_LIST_RESPONSE,
        PACKET_INVENTORY_UPDATE,
        PACKET_QUEST_ACCEPT,
        PACKET_SHOP_OPEN,
        /* Both trimmed by the server to the entries it filled, so the count is
         * the only thing that says how many are there -- and it arrives from
         * the network like everything else. */
        PACKET_FRIEND_LIST_RESPONSE,
        PACKET_FRIEND_REQUESTS_LIST,
    };

    /* Every count byte value, at a handful of buffer sizes: a size that could
     * plausibly hold the entries, and sizes that plainly cannot. */
    const int sizes[] = { 8, 16, 32, 64, 256, 1024, 8192 };

    for (size_t o = 0; o < sizeof(counted) / sizeof(counted[0]); o++) {
        for (int count = 0; count <= 255; count++) {
            for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
                int len = sizes[s];
                reset_game();

                memset(g_buf, 0, (size_t)len);
                PacketHeader* h = (PacketHeader*)g_buf;
                h->type = counted[o];
                h->player_id = htonl(1);
                h->payload_size = htons((uint16_t)(len - sizeof(PacketHeader)));

                /* The count byte sits at a different offset per packet, so set
                 * every byte of the first word after the header. Crude on
                 * purpose: it covers whichever field the handler reads. */
                for (int b = (int)sizeof(PacketHeader);
                     b < (int)sizeof(PacketHeader) + 8 && b < len; b++) {
                    g_buf[b] = (uint8_t)count;
                }

                dispatch_all(counted[o], (const char*)g_buf, len);
            }
        }
    }
    printf("  ok   14 counted opcodes x 256 counts x 7 sizes survived\n");
}

/** Strings that fill their field with no terminator anywhere. */
static void test_unterminated_strings(void) {
    printf("packets whose text fields have no terminator\n");

    const uint8_t texty[] = {
        PACKET_CHAT_MESSAGE,
        PACKET_WORLD_LIST_RESPONSE,
        PACKET_CHARACTER_LIST_RESPONSE,
        PACKET_USE_ITEM_RESPONSE,
        PACKET_EQUIP_ITEM_RESPONSE,
        PACKET_UNEQUIP_ITEM_RESPONSE,
        PACKET_LOOT_PICKUP_RESPONSE,
        PACKET_SHOP_BUY_RESPONSE,
        PACKET_SHOP_SELL_RESPONSE,
        PACKET_DISCONNECT,
        PACKET_DIALOGUE_UPDATE,
        PACKET_QUEST_ACCEPT,
        PACKET_FRIEND_LIST_RESPONSE,
        PACKET_FRIEND_REQUESTS_LIST,
        PACKET_FRIEND_REQUEST_NOTIFY,
        PACKET_FRIEND_PRESENCE_UPDATE,
        PACKET_FRIEND_OP_RESULT,
    };

    for (size_t o = 0; o < sizeof(texty) / sizeof(texty[0]); o++) {
        for (int len = 8; len <= 2048; len += 7) {
            reset_game();

            /* Every byte non-zero: no field can contain a NUL, so any handler
             * that treats one as a C string reads past it unless it terminates
             * it first. */
            memset(g_buf, 0x41, (size_t)len);
            PacketHeader* h = (PacketHeader*)g_buf;
            h->type = texty[o];
            h->player_id = htonl(1);
            h->payload_size = htons((uint16_t)(len - sizeof(PacketHeader)));

            dispatch_all(texty[o], (const char*)g_buf, len);
        }
    }
    printf("  ok   17 text-bearing opcodes survived unterminated fields\n");
}

/** Random bytes, at random lengths, for every opcode. */
static void test_random_fuzz(void) {
    printf("random bytes\n");

    /* Fixed seed: a test that finds a different bug every run is a test whose
     * failures cannot be reproduced. */
    srand(20240617);

    const int iterations = 40000;
    for (int i = 0; i < iterations; i++) {
        int len = 1 + rand() % 1200;
        reset_game();

        for (int b = 0; b < len; b++) g_buf[b] = (uint8_t)(rand() & 0xFF);

        uint8_t type = (uint8_t)(rand() & 0xFF);
        if (len >= (int)sizeof(PacketHeader)) ((PacketHeader*)g_buf)->type = type;

        dispatch_all(type, (const char*)g_buf, len);
    }
    printf("  ok   %d random packets dispatched without a fault\n", iterations);
}

/** A negative length must be refused rather than sign-extended into a size. */
static void test_negative_lengths(void) {
    printf("negative lengths\n");

    for (int opcode = 0; opcode <= 255; opcode++) {
        reset_game();
        memset(g_buf, 0, 64);
        ((PacketHeader*)g_buf)->type = (uint8_t)opcode;
        dispatch_all((uint8_t)opcode, (const char*)g_buf, -1);
        dispatch_all((uint8_t)opcode, (const char*)g_buf, -4096);
    }
    printf("  ok   256 opcodes refused negative lengths\n");
}

/* --- Optimistic moves and their rollback --------------------------------- */

static int g_move_failures = 0;

#define MOVE_CHECK(cond, what)                                              \
    do {                                                                    \
        if (cond) { printf("  ok   %s\n", (what)); }                        \
        else { printf("  FAIL %s  (%s:%d)\n", (what), __FILE__, __LINE__);   \
               g_move_failures++; }                                         \
    } while (0)

/**
 * A refused move puts the bag back the way it was.
 *
 * The click path swaps two slots locally and sends MOVE_ITEM, so the bag
 * responds at once. The refusal used to be logged and otherwise ignored, which
 * left the player looking at an arrangement the server did not have -- correct
 * only until the next INVENTORY_UPDATE happened to arrive, and on the refusal
 * path one might not.
 */
static void test_a_refused_move_is_reverted(void) {
    printf("a refused move restores the slots it swapped\n");
    reset_game();

    /* Two distinguishable items, in slots 3 and 7. */
    g_inventory.slots[3].template_id = 101;
    g_inventory.slots[3].quantity    = 5;
    g_inventory.slots[7].template_id = 202;
    g_inventory.slots[7].quantity    = 1;

    /* What the click already did, before the server answered. */
    ItemSlot temp            = g_inventory.slots[7];
    g_inventory.slots[7]     = g_inventory.slots[3];
    g_inventory.slots[3]     = temp;

    MOVE_CHECK(g_inventory.slots[7].template_id == 101,
               "the optimistic swap moved the item");

    MoveItemResponsePacket refusal;
    memset(&refusal, 0, sizeof(refusal));
    refusal.header.type         = PACKET_MOVE_ITEM_RESPONSE;
    refusal.header.player_id    = htonl(1);
    refusal.header.payload_size = htons(sizeof(refusal) - sizeof(PacketHeader));
    refusal.success   = 0;
    refusal.from_slot = 3;
    refusal.to_slot   = 7;

    dispatch_all(PACKET_MOVE_ITEM_RESPONSE, (const char*)&refusal, sizeof(refusal));

    MOVE_CHECK(g_inventory.slots[3].template_id == 101,
               "the refusal put the first item back");
    MOVE_CHECK(g_inventory.slots[3].quantity == 5,
               "with its quantity intact");
    MOVE_CHECK(g_inventory.slots[7].template_id == 202,
               "and the second item back");

    /* A success must change nothing: the client already applied the move. */
    reset_game();
    g_inventory.slots[3].template_id = 101;
    g_inventory.slots[7].template_id = 202;

    MoveItemResponsePacket ok = refusal;
    ok.success = 1;
    dispatch_all(PACKET_MOVE_ITEM_RESPONSE, (const char*)&ok, sizeof(ok));

    MOVE_CHECK(g_inventory.slots[3].template_id == 101 &&
               g_inventory.slots[7].template_id == 202,
               "an accepted move leaves the bag alone");

    /* Slot numbers past the bag must be refused rather than indexed. They come
     * off the wire, so a hostile server can send any byte at all. */
    reset_game();
    g_inventory.slots[0].template_id = 999;

    MoveItemResponsePacket wild = refusal;
    wild.from_slot = 250;
    wild.to_slot   = 251;
    dispatch_all(PACKET_MOVE_ITEM_RESPONSE, (const char*)&wild, sizeof(wild));

    MOVE_CHECK(g_inventory.slots[0].template_id == 999,
               "out-of-range slots in a refusal change nothing");
}

/* --- The friends panel --------------------------------------------------- */

static int g_friend_failures = 0;

#define FRIEND_CHECK(cond, what)                                            \
    do {                                                                    \
        if (cond) { printf("  ok   %s\n", (what)); }                        \
        else { printf("  FAIL %s  (%s:%d)\n", (what), __FILE__, __LINE__);   \
               g_friend_failures++; }                                       \
    } while (0)

/** Build a friends-list packet trimmed to `count` entries, as the server sends it. */
static size_t build_friend_list(uint8_t count) {
    memset(g_buf, 0, FIXTURE_MAX);

    FriendListResponsePacket* pkt = (FriendListResponsePacket*)g_buf;
    pkt->header.type = PACKET_FRIEND_LIST_RESPONSE;
    pkt->count = count;

    for (uint8_t i = 0; i < count; i++) {
        pkt->friends[i].account_id = htonl(1000u + i);
        pkt->friends[i].online     = (i % 2 == 0) ? 1 : 0;
        /* The server sends a world only for an online friend, and a name for
         * both -- offline rows still need something to label them with. */
        pkt->friends[i].world_id   = (i % 2 == 0) ? htonl(3) : 0;
        snprintf(pkt->friends[i].name, sizeof(pkt->friends[i].name), "Friend%u", i);
    }

    size_t size = offsetof(FriendListResponsePacket, friends)
                + (size_t)count * sizeof(FriendWireEntry);
    pkt->header.payload_size = htons((uint16_t)(size - sizeof(PacketHeader)));
    return size;
}

/**
 * A trimmed list is read as the server sent it, and a lying count is not.
 *
 * The server sends only the entries it filled, so the count byte is the only
 * thing that says how many are there -- and it arrives over the network like
 * everything else. Reading it without checking the bytes that came with it is
 * a read past the end of the packet.
 */
static void test_the_friends_list_is_read_from_what_arrived(void) {
    printf("a friends list is read from the bytes that actually arrived\n");
    reset_game();

    size_t size = build_friend_list(3);
    dispatch_all(PACKET_FRIEND_LIST_RESPONSE, (const char*)g_buf, (int)size);

    FriendsState* fs = &g_playing.friends;
    FRIEND_CHECK(fs->friend_count == 3, "three friends are stored");
    FRIEND_CHECK(fs->have_list == 1, "and the list is marked as arrived");
    FRIEND_CHECK(fs->friends[0].account_id == 1000, "the first has its account");
    FRIEND_CHECK(fs->friends[0].online == 1, "and its presence");
    FRIEND_CHECK(fs->friends[0].world_id == 3, "and its world");
    FRIEND_CHECK(strcmp(fs->friends[0].name, "Friend0") == 0, "and its name");
    FRIEND_CHECK(fs->friends[1].online == 0, "the second is offline");
    FRIEND_CHECK(fs->friends[1].world_id == 0, "with no world");
    FRIEND_CHECK(strcmp(fs->friends[1].name, "Friend1") == 0,
                 "but still named, so the row can be acted on");

    /* Now the same packet claiming far more entries than it carries. */
    reset_game();
    size = build_friend_list(3);
    ((FriendListResponsePacket*)g_buf)->count = 100;
    dispatch_all(PACKET_FRIEND_LIST_RESPONSE, (const char*)g_buf, (int)size);

    FRIEND_CHECK(g_playing.friends.friend_count == 3,
                 "a count past the bytes is cut to what arrived");
}

/** An unterminated name from the wire must not be printed as a C string. */
static void test_an_unterminated_friend_name_is_bounded(void) {
    printf("a friend name with no terminator is bounded\n");
    reset_game();

    size_t size = build_friend_list(1);
    FriendListResponsePacket* pkt = (FriendListResponsePacket*)g_buf;
    memset(pkt->friends[0].name, 'Z', sizeof(pkt->friends[0].name));

    dispatch_all(PACKET_FRIEND_LIST_RESPONSE, (const char*)g_buf, (int)size);

    FriendsState* fs = &g_playing.friends;
    FRIEND_CHECK(fs->friend_count == 1, "the entry is stored");
    FRIEND_CHECK(strlen(fs->friends[0].name) == sizeof(fs->friends[0].name) - 1,
                 "the name is terminated at the buffer's end");
}

/**
 * A presence change updates a friend already listed and invents nobody.
 *
 * A change for a stranger means the list on screen is behind. Adding a row for
 * them would show a friend with no name and no history, which is worse than
 * waiting for the refreshed list that fixes it.
 */
static void test_presence_updates_only_a_listed_friend(void) {
    printf("a presence change updates a listed friend and invents nobody\n");
    reset_game();

    size_t size = build_friend_list(2);
    dispatch_all(PACKET_FRIEND_LIST_RESPONSE, (const char*)g_buf, (int)size);

    FriendsState* fs = &g_playing.friends;
    FRIEND_CHECK(fs->friends[1].online == 0, "the second friend starts offline");

    FriendPresenceUpdatePacket upd;
    memset(&upd, 0, sizeof(upd));
    upd.header.type = PACKET_FRIEND_PRESENCE_UPDATE;
    upd.account_id  = htonl(1001);
    upd.world_id    = htonl(9);
    upd.online      = 1;
    snprintf(upd.name, sizeof(upd.name), "%s", "Elsewhere");

    dispatch_all(PACKET_FRIEND_PRESENCE_UPDATE, (const char*)&upd, sizeof(upd));

    FRIEND_CHECK(fs->friends[1].online == 1, "the update brings them online");
    FRIEND_CHECK(fs->friends[1].world_id == 9, "on the world they are on");
    FRIEND_CHECK(strcmp(fs->friends[1].name, "Elsewhere") == 0,
                 "under the character they are playing");

    /* Going offline drops the world and keeps the name: there is no last world
     * to show, and a row that loses its label the moment somebody logs out is
     * a row nobody can act on. */
    upd.account_id = htonl(1001);
    upd.online     = 0;
    upd.world_id   = 0;
    memset(upd.name, 0, sizeof(upd.name));
    dispatch_all(PACKET_FRIEND_PRESENCE_UPDATE, (const char*)&upd, sizeof(upd));

    FRIEND_CHECK(fs->friends[1].online == 0, "going offline is applied");
    FRIEND_CHECK(fs->friends[1].world_id == 0, "with no world left behind");
    FRIEND_CHECK(strcmp(fs->friends[1].name, "Elsewhere") == 0,
                 "and the name kept, so the row stays identifiable");

    /* Now for somebody who is not a friend at all. */
    upd.account_id = htonl(4242);
    upd.online     = 1;
    dispatch_all(PACKET_FRIEND_PRESENCE_UPDATE, (const char*)&upd, sizeof(upd));

    FRIEND_CHECK(fs->friend_count == 2, "a stranger's change adds no row");
}

/**
 * A result carrying percent signs is data, not a format string.
 *
 * The subject name is chosen by whoever the player typed at, and it reaches an
 * snprintf. If it were ever used as the format, a name of "%s%s%s" would read
 * arbitrary stack.
 */
static void test_a_result_name_is_not_a_format_string(void) {
    printf("a result's subject name is data, never a format\n");
    reset_game();

    FriendOpResultPacket res;
    memset(&res, 0, sizeof(res));
    res.header.type = PACKET_FRIEND_OP_RESULT;
    res.action = FRIEND_ACTION_REQUEST;
    res.result = FRIEND_WIRE_NOT_FOUND;
    memset(res.subject_name, '%', sizeof(res.subject_name));
    res.subject_name[0] = '%';
    res.subject_name[1] = 'n';

    dispatch_all(PACKET_FRIEND_OP_RESULT, (const char*)&res, sizeof(res));

    FriendsState* fs = &g_playing.friends;
    FRIEND_CHECK(fs->notice_timer > 0.0f, "a notice was recorded");
    FRIEND_CHECK(strstr(fs->notice, "No player named") != NULL,
                 "phrased from the table, not from the packet");
}

/** Opening the panel asks the server; the panel never answers from memory. */
static void test_opening_the_panel_queues_a_refresh(void) {
    printf("opening the panel asks the server for the list\n");
    reset_game();

    FriendsState* fs = &g_playing.friends;
    friends_panel_init(fs);

    char name[32];
    FRIEND_CHECK(friends_panel_take_pending(fs, name, sizeof(name)) == FRIEND_PENDING_NONE,
                 "a closed panel has nothing queued");

    friends_panel_toggle(fs);
    FRIEND_CHECK(fs->is_open == 1, "the panel opens");
    FRIEND_CHECK(friends_panel_take_pending(fs, name, sizeof(name)) == FRIEND_PENDING_REFRESH,
                 "and queues a refresh");
    FRIEND_CHECK(friends_panel_take_pending(fs, name, sizeof(name)) == FRIEND_PENDING_NONE,
                 "which is taken exactly once");

    friends_panel_toggle(fs);
    FRIEND_CHECK(fs->is_open == 0, "and toggles shut again");
    FRIEND_CHECK(friends_panel_take_pending(fs, name, sizeof(name)) == FRIEND_PENDING_NONE,
                 "queueing nothing on the way out");
}

/**
 * An empty list before it arrives and after it arrives are different states.
 *
 * They render as "Loading..." and "No friends yet", and saying the second when
 * the first is true tells a player something they act on.
 */
static void test_loading_and_empty_are_distinguishable(void) {
    printf("an unarrived list and an empty one are told apart\n");
    reset_game();

    FriendsState* fs = &g_playing.friends;
    FRIEND_CHECK(fs->have_list == 0 && fs->friend_count == 0,
                 "before the list arrives, nothing is known");

    size_t size = build_friend_list(0);
    dispatch_all(PACKET_FRIEND_LIST_RESPONSE, (const char*)g_buf, (int)size);

    FRIEND_CHECK(fs->have_list == 1 && fs->friend_count == 0,
                 "an empty list that arrived is known to be empty");
}

int main(void) {
    printf("=== client packet dispatch ===\n");

    memset(&g_net, 0, sizeof(g_net));
    InitializeCriticalSection(&g_net.response_lock);
    g_net.connected = TRUE;

    test_every_opcode_at_every_short_length();
    test_declared_counts_are_not_trusted();
    test_unterminated_strings();
    test_negative_lengths();
    test_random_fuzz();
    test_a_refused_move_is_reverted();

    test_the_friends_list_is_read_from_what_arrived();
    test_an_unterminated_friend_name_is_bounded();
    test_presence_updates_only_a_listed_friend();
    test_a_result_name_is_not_a_format_string();
    test_opening_the_panel_queues_a_refresh();
    test_loading_and_empty_are_distinguishable();

    DeleteCriticalSection(&g_net.response_lock);

    int failures = g_move_failures + g_friend_failures;
    if (failures) {
        printf("=== client packet dispatch: %d check(s) FAILED ===\n", failures);
        return 1;
    }

    printf("=== client packet dispatch: all checks passed ===\n");
    return 0;
}
