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
#include "network/heading_wire.h"

#include <assert.h>
#include "states/join_data.h"
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
		PACKET_QUEST_STEP_DONE,
        PACKET_NPC_QUEST_MARKS,
		PACKET_AVAILABLE_QUESTS_RESET,
		PACKET_AVAILABLE_QUEST,
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
    printf("  ok   18 counted opcodes x 256 counts x 7 sizes survived\n");
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
        PACKET_NPC_QUEST_MARKS,
		PACKET_AVAILABLE_QUEST,
		PACKET_QUEST_HISTORY,
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

// Send one available-quest packet to the world dispatcher.
static void send_available(uint8_t type, int length) {
	memset(g_buf, 0, (size_t)length);
	PacketHeader* h = (PacketHeader*)g_buf;

	h->type = type;
	h->player_id = htonl(1);
	h->payload_size = htons((uint16_t)(length - sizeof(PacketHeader)));
	assert(net_dispatch_world(type, (const char*)g_buf, length) == 1);
}

// Quest packets from the world connection arrive during character select.
static void test_early_quest_packets_wait_for_gameplay(void) {
	printf("quest packets that arrive before gameplay are held, then delivered in order\n");

	reset_game();
	g_game.playing = NULL;
	g_stub_available_resets = 0;
	g_stub_available_adds = 0;
	g_stub_available_add_before_reset = 0;

	send_available(PACKET_AVAILABLE_QUESTS_RESET, (int)sizeof(AvailableQuestsResetPacket));
	send_available(PACKET_AVAILABLE_QUEST, (int)sizeof(AvailableQuestPacket));
	send_available(PACKET_AVAILABLE_QUEST, (int)sizeof(AvailableQuestPacket));
	assert(g_stub_available_resets == 0 && g_stub_available_adds == 0);
	printf("  ok   nothing is delivered while there is no gameplay state\n");

	g_game.playing = &g_playing;
	network_replay_early_quest_packets();
	assert(g_stub_available_resets == 1 && g_stub_available_adds == 2);
	assert(!g_stub_available_add_before_reset);
	printf("  ok   all three arrive once it exists, the reset first\n");

	network_replay_early_quest_packets();
	assert(g_stub_available_resets == 1 && g_stub_available_adds == 2);
	printf("  ok   a second replay delivers nothing more\n");

	send_available(PACKET_AVAILABLE_QUESTS_RESET, (int)sizeof(AvailableQuestsResetPacket));
	assert(g_stub_available_resets == 2);
	printf("  ok   with gameplay running, packets go straight through\n");

	g_game.playing = NULL;
	send_available(PACKET_AVAILABLE_QUEST, (int)sizeof(AvailableQuestPacket));
	net_world_forget_early_quest();
	g_game.playing = &g_playing;
	network_replay_early_quest_packets();
	assert(g_stub_available_adds == 2);
	printf("  ok   a new world connection forgets what an old one left\n");
}

// Hand one zeroed packet of a given type and length to the whole chain.
static void send_blank(uint8_t type, int length) {
	memset(g_buf, 0, (size_t)length);
	PacketHeader* h = (PacketHeader*)g_buf;

	h->type = type;
	h->player_id = htonl(1);
	h->payload_size = htons((uint16_t)(length - sizeof(PacketHeader)));
	assert(net_dispatch_packet(type, (const char*)g_buf, length) == 1);
}

// Stats and both ability bars are pushed at join, possibly before gameplay exists.
static void test_early_stats_and_abilities_wait_for_gameplay(void) {
	printf("stats, abilities and the first nearby lists that arrive before gameplay are held, then delivered\n");

	reset_game();
	g_game.playing = NULL;
	join_data_reset();

	send_blank(PACKET_PLAYER_STATS, (int)sizeof(PlayerStatsPacket));
	send_blank(PACKET_NPC_POSITIONS, (int)offsetof(NPCPositionPacket, npcs));
	send_blank(PACKET_PLAYER_POSITIONS, (int)offsetof(PlayerPositionBroadcastPacket, players));
	send_blank(PACKET_ABILITY_DATA, (int)sizeof(AbilityDataPacket));
	((AbilityDataPacket*)g_buf)->form = FORM_ANIMAL;
	assert(net_dispatch_packet(PACKET_ABILITY_DATA, (const char*)g_buf,
	                           (int)sizeof(AbilityDataPacket)) == 1);
	assert(!join_data_seen(JOIN_STATS) && !join_data_seen(JOIN_ABILITIES_HUMAN));
	assert(!join_data_seen(JOIN_NPCS) && !join_data_seen(JOIN_PLAYERS));
	printf("  ok   nothing is noted while there is no gameplay state\n");

	g_game.playing = &g_playing;
	network_replay_early_quest_packets();
	assert(join_data_seen(JOIN_STATS));
	assert(join_data_seen(JOIN_ABILITIES_HUMAN));
	assert(join_data_seen(JOIN_ABILITIES_ANIMAL));
	assert(join_data_seen(JOIN_NPCS) && join_data_seen(JOIN_PLAYERS));
	printf("  ok   once it exists the stats, both forms and the first nearby lists are delivered\n");
}

// The loading screen waits on these arrivals, so each one has to be noted.
static void test_join_arrivals_are_noted(void) {
	printf("each join packet notes its arrival\n");

	reset_game();
	join_data_reset();

	send_blank(PACKET_FRIEND_LIST_RESPONSE, (int)offsetof(FriendListResponsePacket, friends));
	assert(join_data_seen(JOIN_FRIENDS) && !join_data_seen(JOIN_FRIEND_REQUESTS));
	send_blank(PACKET_FRIEND_REQUESTS_LIST, (int)offsetof(FriendRequestsListPacket, requests));
	assert(join_data_seen(JOIN_FRIEND_REQUESTS));
	printf("  ok   the friend list and the request list\n");

	send_blank(PACKET_SESSION_LIST_RESPONSE, (int)offsetof(SessionListResponsePacket, entries));
	assert(join_data_seen(JOIN_ONLINE_LIST));
	printf("  ok   the online players list\n");

	send_blank(PACKET_NPC_POSITIONS, (int)offsetof(NPCPositionPacket, npcs));
	assert(join_data_seen(JOIN_NPCS) && !join_data_seen(JOIN_PLAYERS));
	send_blank(PACKET_PLAYER_POSITIONS, (int)offsetof(PlayerPositionBroadcastPacket, players));
	assert(join_data_seen(JOIN_PLAYERS));
	printf("  ok   the nearby NPCs and players\n");

	// The quest list is whole only when every entry it announced has come
	send_available(PACKET_AVAILABLE_QUESTS_RESET, (int)sizeof(AvailableQuestsResetPacket));
	assert(join_data_seen(JOIN_QUESTS));
	join_data_reset();
	((AvailableQuestsResetPacket*)g_buf)->count = htonl(2);
	assert(net_dispatch_packet(PACKET_AVAILABLE_QUESTS_RESET, (const char*)g_buf,
	                           (int)sizeof(AvailableQuestsResetPacket)) == 1);
	assert(!join_data_seen(JOIN_QUESTS));
	send_available(PACKET_AVAILABLE_QUEST, (int)sizeof(AvailableQuestPacket));
	assert(!join_data_seen(JOIN_QUESTS));
	send_available(PACKET_AVAILABLE_QUEST, (int)sizeof(AvailableQuestPacket));
	assert(join_data_seen(JOIN_QUESTS));
	printf("  ok   the available quests, once the announced count is in\n");
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

// --- Emotes ---

static int g_emote_failures = 0;
#define EMOTE_CHECK(cond, what)                                             \
    do { if (cond) printf("  ok   %s\n", (what));                          \
         else { printf("  FAIL %s  (%s:%d)\n", (what), __FILE__, __LINE__);  \
                g_emote_failures++; } } while (0)

static void send_emote_state(uint8_t emote, uint8_t arg, uint8_t result,
                             uint8_t requested) {
	EmoteStatePacket pkt;

	memset(&pkt, 0, sizeof(pkt));
	pkt.header.type = PACKET_EMOTE_STATE;
	pkt.header.player_id = htonl(1);
	pkt.header.payload_size = htons(sizeof(pkt) - sizeof(PacketHeader));
	pkt.emote = emote;
	pkt.arg = arg;
	pkt.result = result;
	pkt.requested = requested;
	dispatch_all(PACKET_EMOTE_STATE, (const char*)&pkt, sizeof(pkt));
}

// One positions broadcast carrying n nearby players.
static void send_positions(const uint32_t* ids, const uint8_t* emotes,
                           const uint8_t* args, int n) {
	static PlayerPositionBroadcastPacket pkt;
	size_t size;

	memset(&pkt, 0, sizeof(pkt));
	pkt.header.type = PACKET_PLAYER_POSITIONS;
	pkt.header.player_id = htonl(1);
	pkt.count = (uint8_t)n;
	for(int i = 0; i < n; i++) {
		pkt.players[i].player_id = htonl(ids[i]);
		pkt.players[i].health = htonl(10);
		pkt.players[i].max_health = htonl(10);
		pkt.players[i].emote = emotes[i];
		pkt.players[i].emote_arg = args[i];
	}

	size = offsetof(PlayerPositionBroadcastPacket, players) +
	       (size_t)n * sizeof(NearbyPlayerData);
	pkt.header.payload_size = htons((uint16_t)(size - sizeof(PacketHeader)));
	dispatch_all(PACKET_PLAYER_POSITIONS, (const char*)&pkt, (int)size);
}

static void test_emote_states_arrive_in_order(void) {
	uint8_t emote, arg, result, requested;
	int got = 0;
	uint8_t last_arg = 0;

	printf("the server's word on the player's emote is queued, oldest first\n");
	reset_game();

	// the fuzz tests above sprayed this queue with whatever bytes they chose
	while(network_get_emote_state(&emote, &arg, &result, &requested)) {
	}

	EMOTE_CHECK(!network_get_emote_state(&emote, &arg, &result, &requested),
	            "nothing queued at first");
	send_emote_state(EMOTE_SIT, 1, EMOTE_RESULT_STARTED, EMOTE_SIT);
	send_emote_state(EMOTE_NONE, 0, EMOTE_ENDED_MOVED, EMOTE_SIT);
	EMOTE_CHECK(network_get_emote_state(&emote, &arg, &result, &requested) &&
	            emote == EMOTE_SIT && arg == 1 && result == EMOTE_RESULT_STARTED &&
	            requested == EMOTE_SIT, "the start comes out first, intact");
	EMOTE_CHECK(network_get_emote_state(&emote, &arg, &result, &requested) &&
	            emote == EMOTE_NONE && result == EMOTE_ENDED_MOVED, "then the ending");
	EMOTE_CHECK(!network_get_emote_state(&emote, &arg, &result, &requested),
	            "then nothing");

	// a flood drops the oldest and keeps the newest, never overrunning
	for(int i = 0; i < 20; i++)
		send_emote_state(EMOTE_DANCE, (uint8_t)i, EMOTE_RESULT_STARTED, EMOTE_DANCE);

	while(network_get_emote_state(&emote, &arg, &result, &requested)) {
		got++;
		last_arg = arg;
	}
	EMOTE_CHECK(got == 8 && last_arg == 19, "a flood keeps the 8 newest");
}

static void send_world_time(uint32_t time_of_day_ms, uint32_t day_length_ms, int length) {
	WorldTimePacket pkt;

	memset(&pkt, 0, sizeof(pkt));
	pkt.header.type = PACKET_WORLD_TIME;
	pkt.header.payload_size = htons(sizeof(pkt) - sizeof(PacketHeader));
	pkt.time_of_day_ms = htonl(time_of_day_ms);
	pkt.day_length_ms = htonl(day_length_ms);
	assert(net_dispatch_packet(PACKET_WORLD_TIME, (const char*)&pkt, length) == 1);
}

static void test_the_world_time_is_read_once_in_network_order(void) {
	uint32_t time_of_day_ms = 0;
	uint32_t day_length_ms = 0;

	printf("the world time is read in network order and handed over once\n");
	reset_game();

	// the fuzz tests above may have left one waiting
	while(network_get_world_time(&time_of_day_ms, &day_length_ms)) {
	}

	assert(!network_get_world_time(&time_of_day_ms, &day_length_ms));
	send_world_time(675000, 2700000, (int)sizeof(WorldTimePacket));
	assert(network_get_world_time(&time_of_day_ms, &day_length_ms));
	assert(time_of_day_ms == 675000 && day_length_ms == 2700000);
	assert(!network_get_world_time(&time_of_day_ms, &day_length_ms));

	// a second one before the first is read replaces it
	send_world_time(1, 2700000, (int)sizeof(WorldTimePacket));
	send_world_time(2, 2700000, (int)sizeof(WorldTimePacket));
	assert(network_get_world_time(&time_of_day_ms, &day_length_ms));
	assert(time_of_day_ms == 2);

	// a truncated packet is claimed and ignored
	send_world_time(3, 2700000, (int)sizeof(WorldTimePacket) - 1);
	assert(!network_get_world_time(&time_of_day_ms, &day_length_ms));
	printf("  ok   read once, newest wins, truncated ignored\n");
}

static void send_weather(uint32_t day_index, int region_count, int length) {
	WeatherForecastPacket pkt;

	memset(&pkt, 0, sizeof(pkt));
	pkt.header.type = PACKET_WEATHER_FORECAST;
	pkt.header.payload_size = htons(sizeof(pkt) - sizeof(PacketHeader));
	pkt.day_index = htonl(day_index);
	pkt.day_length_ms = htonl(7200000);
	pkt.ms_to_next_day = htonl(123456);
	pkt.region_count = (uint8_t)region_count;
	pkt.days[1][2].condition = WEATHER_SNOW;
	pkt.days[1][2].temp_hi = -3;
	pkt.days[1][2].precip_pct = 80;
	pkt.days[1][2].revised = WEATHER_REVISED_WEST;
	assert(net_dispatch_packet(PACKET_WEATHER_FORECAST, (const char*)&pkt, length) == 1);
}

static void test_the_forecast_is_read_once_in_host_order(void) {
	WeatherForecast forecast;

	printf("the forecast is read in host order and handed over once\n");
	reset_game();

	// the fuzz tests above may have left one waiting
	while(network_get_weather(&forecast)) {
	}

	assert(!network_get_weather(&forecast));
	send_weather(41, 4, (int)sizeof(WeatherForecastPacket));
	assert(network_get_weather(&forecast));
	assert(forecast.day_index == 41 && forecast.day_length_ms == 7200000);
	assert(forecast.ms_to_next_day == 123456 && forecast.region_count == 4);
	assert(forecast.days[1][2].condition == WEATHER_SNOW);
	assert(forecast.days[1][2].temp_hi == -3 && forecast.days[1][2].precip_pct == 80);
	assert(forecast.days[1][2].revised == WEATHER_REVISED_WEST);
	assert(!network_get_weather(&forecast));

	// a second one before the first is read replaces it
	send_weather(1, 4, (int)sizeof(WeatherForecastPacket));
	send_weather(2, 4, (int)sizeof(WeatherForecastPacket));
	assert(network_get_weather(&forecast));
	assert(forecast.day_index == 2);

	// a truncated packet is claimed and ignored
	send_weather(3, 4, (int)sizeof(WeatherForecastPacket) - 1);
	assert(!network_get_weather(&forecast));
	printf("  ok   read once, newest wins, truncated ignored\n");
}

static void test_another_players_emote_follows_them(void) {
	NearbyPlayer* np;
	uint32_t ids[2] = { 7, 9 };
	uint8_t emotes[2] = { EMOTE_SIT, EMOTE_NONE };
	uint8_t args[2] = { 1, 0 };
	uint32_t swapped[2] = { 9, 7 };
	uint8_t same[2] = { EMOTE_NONE, EMOTE_SIT };
	uint8_t same_args[2] = { 0, 1 };
	uint8_t stopped[2] = { EMOTE_NONE, EMOTE_NONE };
	uint8_t dance[2] = { EMOTE_DANCE, EMOTE_NONE };
	uint32_t only9[1] = { 9 };

	printf("another player's emote is carried across broadcasts by their id\n");
	reset_game();
	np = g_playing.nearby_players;

	// first seen already sitting: seated at once
	send_positions(ids, emotes, args, 2);
	EMOTE_CHECK(g_playing.nearby_player_count == 2, "two players in view");
	EMOTE_CHECK(np[0].emote.phase == EMOTE_PHASE_HELD && np[0].emote.id == EMOTE_SIT &&
	            np[0].emote.arg == 1,
	            "one seen mid-sit is seated, with no sit-down to play");
	EMOTE_CHECK(np[1].emote.phase == EMOTE_PHASE_NONE, "the other is not emoting");

	// the next broadcast lists them in the other order: the state follows the id
	send_positions(swapped, same, same_args, 2);
	EMOTE_CHECK(np[0].player_id == 9 && np[0].emote.phase == EMOTE_PHASE_NONE,
	            "the slot that changed hands does not inherit a sit");
	EMOTE_CHECK(np[1].player_id == 7 && np[1].emote.phase == EMOTE_PHASE_HELD,
	            "the sitter is still sitting in their new slot");

	// the server says they stopped: they get up
	send_positions(swapped, stopped, same_args, 2);
	EMOTE_CHECK(np[1].emote.phase == EMOTE_PHASE_EXITING, "a stop plays the get-up");

	// a player already known who starts one plays it from the beginning
	send_positions(swapped, dance, same_args, 2);
	EMOTE_CHECK(np[0].emote.id == EMOTE_DANCE && np[0].emote.phase == EMOTE_PHASE_HELD,
	            "a dance starts (it has no enter clip)");

	// someone who leaves view and returns is a first sighting again
	send_positions(only9, dance, same_args, 1);
	send_positions(swapped, stopped, same_args, 2);
	EMOTE_CHECK(np[1].emote.phase == EMOTE_PHASE_NONE,
	            "a returning player starts from nothing");
}

// The inspect answer is held for the window to read, once, and ended.
static void test_inspect_answer_is_held_once(void) {
	InspectResponsePacket pkt;
	InspectResponsePacket got;

	printf("an inspect answer is held until read, then gone\n");
	reset_game();
	while(network_get_inspect_response(&got)) {
	}

	memset(&pkt, 0, sizeof(pkt));
	pkt.header.type = PACKET_INSPECT_RESPONSE;
	pkt.header.player_id = htonl(1);
	pkt.header.payload_size = htons(sizeof(pkt) - sizeof(PacketHeader));
	pkt.target_id = htonl(9);
	pkt.result = INSPECT_OK;
	pkt.equipment[EQUIP_HELMET] = htonl(1001);

	// the name arrives with no terminator at all
	memset(pkt.name, 'x', sizeof(pkt.name));
	dispatch_all(PACKET_INSPECT_RESPONSE, (const char*)&pkt, sizeof(pkt));
	EMOTE_CHECK(network_get_inspect_response(&got), "the answer comes out");
	EMOTE_CHECK(ntohl(got.target_id) == 9 && got.result == INSPECT_OK &&
	            ntohl(got.equipment[EQUIP_HELMET]) == 1001, "intact");
	EMOTE_CHECK(got.name[sizeof(got.name) - 1] == '\0', "its name is ended");
	EMOTE_CHECK(!network_get_inspect_response(&got), "and is not read twice");

	// a truncated answer is not an answer
	dispatch_all(PACKET_INSPECT_RESPONSE, (const char*)&pkt, sizeof(pkt) - 1);
	EMOTE_CHECK(!network_get_inspect_response(&got), "a short answer is dropped");
}

// A broadcast naming one player with a ground speed, or none at all.
static void send_positions_with_speed(uint32_t id, uint16_t speed, int n) {
	static PlayerPositionBroadcastPacket pkt;
	size_t size;

	memset(&pkt, 0, sizeof(pkt));
	pkt.header.type = PACKET_PLAYER_POSITIONS;
	pkt.header.player_id = htonl(1);
	pkt.count = (uint8_t)n;
	if(n > 0) {
		pkt.players[0].player_id = htonl(id);
		pkt.players[0].health = htonl(10);
		pkt.players[0].max_health = htonl(10);
		pkt.players[0].speed = htons(speed);
	}
	size = offsetof(PlayerPositionBroadcastPacket, players) +
	       (size_t)n * sizeof(NearbyPlayerData);
	pkt.header.payload_size = htons((uint16_t)(size - sizeof(PacketHeader)));
	dispatch_all(PACKET_PLAYER_POSITIONS, (const char*)&pkt, (int)size);
}

static void test_facing_and_idle_variation_arrive(void) {
	static PlayerPositionBroadcastPacket pkt;
	size_t size;

	printf("a player's facing and idle variation are carried across\n");
	reset_game();
	memset(&pkt, 0, sizeof(pkt));
	pkt.header.type = PACKET_PLAYER_POSITIONS;
	pkt.count = 1;
	pkt.players[0].player_id = htonl(7);
	pkt.players[0].health = htonl(10);
	pkt.players[0].max_health = htonl(10);
	pkt.players[0].heading = heading_to_wire(1.5f);
	pkt.players[0].idle_fidget = 1;
	size = offsetof(PlayerPositionBroadcastPacket, players) + sizeof(NearbyPlayerData);
	pkt.header.payload_size = htons((uint16_t)(size - sizeof(PacketHeader)));
	dispatch_all(PACKET_PLAYER_POSITIONS, (const char*)&pkt, (int)size);

	EMOTE_CHECK(g_playing.nearby_players[0].idle_fidget == 1, "the idle variation flag arrives");
	EMOTE_CHECK(fabsf(heading_from_wire(g_playing.nearby_players[0].heading) - 1.5f) < 0.02f,
	            "the facing arrives, to the wire's precision");
	EMOTE_CHECK(heading_to_wire(heading_from_wire(200)) == 200, "a wire facing survives a round trip");
	EMOTE_CHECK(heading_wire_steps(250, 4) == 10, "facings are compared the short way round");
}

static void test_an_empty_list_clears_who_is_in_view(void) {
	printf("the last player leaving view is told, and the speed they moved at is kept\n");
	reset_game();

	send_positions_with_speed(7, 150, 1);
	EMOTE_CHECK(g_playing.nearby_player_count == 1, "one player in view");
	EMOTE_CHECK(g_playing.nearby_players[0].speed == 150.0f, "their ground speed arrives");

	send_positions_with_speed(0, 0, 0);
	EMOTE_CHECK(g_playing.nearby_player_count == 0, "an empty broadcast empties the list");
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
    test_early_quest_packets_wait_for_gameplay();
	test_early_stats_and_abilities_wait_for_gameplay();
	test_join_arrivals_are_noted();
    test_a_refused_move_is_reverted();
    test_emote_states_arrive_in_order();
    test_the_world_time_is_read_once_in_network_order();
    test_the_forecast_is_read_once_in_host_order();
    test_another_players_emote_follows_them();
    test_inspect_answer_is_held_once();
    test_an_empty_list_clears_who_is_in_view();
    test_facing_and_idle_variation_arrive();

    test_the_friends_list_is_read_from_what_arrived();
    test_an_unterminated_friend_name_is_bounded();
    test_presence_updates_only_a_listed_friend();
    test_a_result_name_is_not_a_format_string();
    test_opening_the_panel_queues_a_refresh();
    test_loading_and_empty_are_distinguishable();

    DeleteCriticalSection(&g_net.response_lock);

    int failures = g_move_failures + g_friend_failures + g_emote_failures;
    if (failures) {
        printf("=== client packet dispatch: %d check(s) FAILED ===\n", failures);
        return 1;
    }

    printf("=== client packet dispatch: all checks passed ===\n");
    return 0;
}
