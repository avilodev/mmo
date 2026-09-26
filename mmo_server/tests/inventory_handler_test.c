/**
 * @file
 * Check the five inventory packet handlers -- equip, unequip, use, drop, move
 * -- and the name-query route that shares packet_handler.c with them.
 *
 * These are the handlers that move a player's belongings, and they had no
 * coverage at all. Each one takes a slot index straight off the wire and
 * indexes a fixed array with it, and each one is the difference between an
 * item existing and not existing -- a use that does not consume, an equip that
 * loses what it displaced, a drop that removes the stack without creating the
 * ground item.
 *
 * The real handlers, the real inventory model and the real item registry are
 * linked. The player registry, the socket layer and the ground-item pool are
 * replaced, so what is measured is the handler.
 */

#define _POSIX_C_SOURCE 200809L

#include "packet_handler.h"
#include "npc_query.h"
#include "items_database.h"
#include "item_instance.h"
#include "server_types.h"
#include "config.h"
#include "move_validator.h"
#include "zone_system.h"
#include "party.h"
#include "log.h"

#include <arpa/inet.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond, what)                                                   \
    do {                                                                    \
        if (cond) { printf("  ok   %s\n", (what)); }                        \
        else { printf("  FAIL %s  (%s:%d)\n", (what), __FILE__, __LINE__);  \
               g_failures++; }                                              \
    } while (0)

/* --- One player ---------------------------------------------------------- */

ActivePlayer active_players[MAX_PLAYERS];
ServerConfig g_server;

#define CHAR_ID 42u
#define CLIENT_FD 7

static ActivePlayer* me(void) { return &active_players[0]; }

/* --- Registry stubs ------------------------------------------------------ */

ActivePlayer* player_acquire(uint32_t character_id) {
    if (character_id != CHAR_ID) return NULL;
    pthread_mutex_lock(&me()->lock);
    return me();
}
ActivePlayer* player_acquire_hint(uint32_t character_id, int slot) {
    (void)slot; return player_acquire(character_id);
}
void player_release(ActivePlayer* p) { if (p) pthread_mutex_unlock(&p->lock); }

static pthread_rwlock_t g_registry_lock = PTHREAD_RWLOCK_INITIALIZER;
void player_registry_rdlock(void) { pthread_rwlock_rdlock(&g_registry_lock); }
void player_registry_unlock(void) { pthread_rwlock_unlock(&g_registry_lock); }
static int g_slots[1] = { 0 };
const int* player_active_list_locked(int* n) { if (n) *n = 1; return g_slots; }

uint32_t player_find_by_name(const char* name) { (void)name; return 0; }

/* Movement validation asks two questions this test has no opinion about: is this
 * player afraid of something, and where is that something standing. Nobody here
 * is feared, so the answer is no and the lookup never runs. */
uint32_t player_fear_source(const ActivePlayer* p) { (void)p; return 0; }
int npc_query_lookup(uint32_t npc_id, NpcQueryHit* out) {
    (void)npc_id; (void)out; return 0;
}
void player_send_data_response(int fd, uint32_t c) { (void)fd; (void)c; }

/** The block index, which only the party-invite route in this file consults.
 *
 * Nothing here is blocked; friends.c pulls in the friend bus and Redis, and
 * whether an invite honours a block is world_friends_test's subject.
 */
int world_friends_is_blocked(uint32_t blocker, uint32_t subject) {
    (void)blocker; (void)subject; return 0;
}

/** Equipment bonuses and the derived-stat packet are their own subjects.
 *
 * player_level.c is not linked: it pulls in class tables, the ability hotbars
 * and the experience curve, none of which an inventory move touches. What
 * matters here is that the handler recalculates at all, and that it sends the
 * stats while it still owns the slot, so both are recorded. */
static int g_bonus_recalcs = 0;
void player_apply_equipment_bonuses(ActivePlayer* p) { (void)p; g_bonus_recalcs++; }

static int g_stats_sent = 0;
void player_send_stats_locked(int fd, ActivePlayer* p) {
    (void)fd; (void)p;
    g_stats_sent++;
}
void player_send_stats(int fd, ActivePlayer* p) { (void)fd; (void)p; g_stats_sent++; }

/** Which wire slots the handler asked the client to resynchronize. */
#define MAX_SYNCED 16
static uint16_t g_synced[MAX_SYNCED];
static int      g_synced_count = 0;
void player_send_slot_updates(int fd, uint32_t c, const uint16_t* ids, int count) {
    (void)fd; (void)c;
    for (int i = 0; i < count && g_synced_count < MAX_SYNCED; i++)
        g_synced[g_synced_count++] = ids[i];
}

/* --- Everything else the translation unit references --------------------- */

int      party_snapshot(uint32_t id, PartySnapshot* out) { (void)id; (void)out; return 0; }
int      party_snapshot_of_player(uint32_t c, PartySnapshot* o) { (void)c; (void)o; return 0; }
uint32_t party_id_of_player(uint32_t c) { (void)c; return 0; }
uint32_t party_create(uint32_t l) { (void)l; return 0; }
int      party_add_member(uint32_t p, uint32_t c) { (void)p; (void)c; return 0; }
void     party_remove_member(uint32_t c) { (void)c; }
void     party_broadcast_update(uint32_t p) { (void)p; }
int      party_invite_create(uint32_t f, uint32_t t, uint32_t p) { (void)f;(void)t;(void)p; return 0; }
int      party_invite_find_for_player(uint32_t t, PendingInvite* o) { (void)t;(void)o; return 0; }
void     party_invite_remove(uint32_t t) { (void)t; }

MoveVerdict move_validate(MoveBudget* b, float fx, float fy, float tx, float ty,
                          float speed, const struct timespec* now) {
    (void)b;(void)fx;(void)fy;(void)tx;(void)ty;(void)speed;(void)now;
    return MOVE_ACCEPT;
}
const char* move_verdict_name(MoveVerdict v) { (void)v; return "ok"; }
const WorldZone* zone_lookup(float x, float y) { (void)x; (void)y; return NULL; }

/** The ground-item pool, recorded rather than run. */
static int      g_drops = 0;
static uint32_t g_dropped_item = 0;
static uint8_t  g_dropped_qty = 0;
uint32_t loot_drop_item(uint32_t item_id, uint8_t quantity, float x, float y, uint32_t owner) {
    (void)x; (void)y; (void)owner;
    g_drops++;
    g_dropped_item = item_id;
    g_dropped_qty  = quantity;
    return 1;
}

/* --- The socket ---------------------------------------------------------- */

#define MAX_REPLIES 16
static uint8_t g_reply[MAX_REPLIES][512];
static size_t  g_reply_len[MAX_REPLIES];
static int     g_reply_count = 0;

ssize_t server_send(int fd, void* buf, size_t len) {
    (void)fd;
    if (g_reply_count < MAX_REPLIES) {
        size_t n = len < sizeof(g_reply[0]) ? len : sizeof(g_reply[0]);
        memcpy(g_reply[g_reply_count], buf, n);
        g_reply_len[g_reply_count] = n;
        g_reply_count++;
    }
    return (ssize_t)len;
}
ssize_t server_send_direct(int fd, void* buf, size_t len) { return server_send(fd, buf, len); }

/** The first reply of a given opcode, or NULL. */
static const void* reply_of(uint8_t type) {
    for (int i = 0; i < g_reply_count; i++)
        if (g_reply_len[i] >= sizeof(PacketHeader) && g_reply[i][0] == type)
            return g_reply[i];
    return NULL;
}

static void reset_io(void) {
    g_reply_count = 0;
    g_synced_count = 0;
    g_bonus_recalcs = 0;
    g_stats_sent = 0;
    g_drops = 0;
}

/* --- Fixtures ------------------------------------------------------------ */

/** Item definitions this test needs, written where items_database.c reads them. */
static const char* k_items_json =
    "{\"items\":["
    "{\"id\":10,\"name\":\"Iron Helm\",\"type\":\"armor\",\"slot\":\"helmet\",\"value\":100},"
    "{\"id\":11,\"name\":\"Iron Boots\",\"type\":\"armor\",\"slot\":\"boots\",\"value\":100},"
    /* is_two_handed is derived from the slot name, not from a separate key --
     * see parse_slot() in items_database.c. */
    "{\"id\":20,\"name\":\"Greatsword\",\"type\":\"weapon\",\"slot\":\"two_handed\","
    " \"value\":300},"
    "{\"id\":21,\"name\":\"Shield\",\"type\":\"armor\",\"slot\":\"off_hand\",\"value\":150},"
    "{\"id\":30,\"name\":\"Potion\",\"type\":\"consumable\",\"max_stack\":20,"
    " \"use_effect\":\"restore_health\",\"use_value\":25,\"value\":10},"
    "{\"id\":31,\"name\":\"Rock\",\"type\":\"misc\",\"max_stack\":10,\"value\":1}"
    "]}";

static void write_items(const char* path) {
    FILE* f = fopen(path, "w");
    assert(f);
    fputs(k_items_json, f);
    fclose(f);
}

/** Empty the player and give them a known starting state. */
static void reset_player(void) {
    ActivePlayer* p = me();
    memset(p, 0, sizeof(*p));
    pthread_mutex_init(&p->lock, NULL);
    p->character_id = CHAR_ID;
    p->client_fd    = CLIENT_FD;
    p->is_loaded    = 1;
    p->health       = 50;
    p->max_health   = 100;
    p->resource     = 0;
    p->max_resource = 100;
    p->pos_x = p->pos_y = 500.0f;
    reset_io();
}

/* --- Sending one packet -------------------------------------------------- */

static void send_equip(uint32_t item_id, uint8_t inv_slot, uint8_t equip_slot) {
    EquipItemPacket pkt = {0};
    pkt.header.type = PACKET_EQUIP_ITEM;
    pkt.item_id        = htonl(item_id);
    pkt.inventory_slot = inv_slot;
    pkt.equip_slot     = equip_slot;
    handle_equip_item(CLIENT_FD, CHAR_ID, (uint8_t*)&pkt, (ssize_t)sizeof(pkt));
}

static void send_unequip(uint8_t equip_slot) {
    UnequipItemPacket pkt = {0};
    pkt.header.type = PACKET_UNEQUIP_ITEM;
    pkt.equip_slot  = equip_slot;
    handle_unequip_item(CLIENT_FD, CHAR_ID, (uint8_t*)&pkt, (ssize_t)sizeof(pkt));
}

static void send_use(uint8_t inv_slot) {
    UseItemPacket pkt = {0};
    pkt.header.type    = PACKET_USE_ITEM;
    pkt.inventory_slot = inv_slot;
    handle_use_item(CLIENT_FD, CHAR_ID, (uint8_t*)&pkt, (ssize_t)sizeof(pkt));
}

static void send_drop(uint8_t inv_slot) {
    DropItemPacket pkt = {0};
    pkt.header.type    = PACKET_DROP_ITEM;
    pkt.inventory_slot = inv_slot;
    handle_drop_item(CLIENT_FD, CHAR_ID, (uint8_t*)&pkt, (ssize_t)sizeof(pkt));
}

static void send_move(uint8_t from, uint8_t to) {
    MoveItemPacket pkt = {0};
    pkt.header.type = PACKET_MOVE_ITEM;
    pkt.from_slot   = from;
    pkt.to_slot     = to;
    handle_move_item(CLIENT_FD, CHAR_ID, (uint8_t*)&pkt, (ssize_t)sizeof(pkt));
}

/** Count occupied inventory slots. */
static int occupied(void) {
    int n = 0;
    for (int i = 0; i < INVENTORY_SLOTS; i++)
        if (me()->inventory[i].instance_id != 0) n++;
    return n;
}

int main(void) {
    printf("=== inventory handlers ===\n");
    log_init();

    const char* items_path = "/tmp/mmo_inventory_handler_items.json";
    write_items(items_path);
    assert(items_init(items_path) == 1);

    /* ---------------------------------------------------------------- */
    printf("\nTEST 1: equipping moves the item and returns what it displaced\n");
    reset_player();
    assert(inventory_add(me()->inventory, 10, 1, 1, 0) == 0);
    uint64_t helm_instance = me()->inventory[0].instance_id;

    send_equip(10, 0, SLOT_HELMET);

    CHECK(me()->equipment[EQUIP_HELMET].item_id == 10, "the helm is worn");
    CHECK(me()->equipment[EQUIP_HELMET].instance_id == helm_instance,
          "and it is the same instance, not a copy");
    CHECK(me()->inventory[0].instance_id == 0, "its inventory slot is now empty");
    CHECK(g_bonus_recalcs == 1, "equipment bonuses were recalculated");
    CHECK(g_synced_count == 2, "both changed slots were resynchronized");
    {
        const EquipItemResponsePacket* r = reply_of(PACKET_EQUIP_ITEM_RESPONSE);
        CHECK(r && r->success == 1, "the client was told it succeeded");
    }

    printf("\nTEST 2: equipping over a worn item returns the old one to the bag\n");
    reset_io();
    assert(inventory_add(me()->inventory, 10, 1, 1, 0) == 0);   /* a second helm */
    uint64_t second_helm = me()->inventory[0].instance_id;
    send_equip(10, 0, SLOT_HELMET);
    CHECK(me()->equipment[EQUIP_HELMET].instance_id == second_helm, "the new helm is worn");
    CHECK(me()->inventory[0].instance_id == helm_instance,
          "and the old one came back to the slot the new one left");
    CHECK(occupied() == 1, "nothing was created and nothing was destroyed");

    /* ---------------------------------------------------------------- */
    printf("\nTEST 3: an item cannot be equipped in the wrong slot\n");
    reset_player();
    assert(inventory_add(me()->inventory, 11, 1, 1, 0) == 0);   /* boots */
    send_equip(11, 0, SLOT_HELMET);
    CHECK(me()->equipment[EQUIP_HELMET].instance_id == 0, "boots are not a helmet");
    CHECK(me()->inventory[0].item_id == 11, "and they stayed in the bag");
    {
        const EquipItemResponsePacket* r = reply_of(PACKET_EQUIP_ITEM_RESPONSE);
        CHECK(r && r->success == 0, "the refusal was reported");
    }

    printf("\nTEST 4: a slot index past the bag is refused, not indexed\n");
    reset_player();
    assert(inventory_add(me()->inventory, 10, 1, 1, 0) == 0);
    send_equip(10, 200, SLOT_HELMET);      /* INVENTORY_SLOTS is 150 */
    send_equip(10, 255, SLOT_HELMET);
    CHECK(me()->equipment[EQUIP_HELMET].instance_id == 0, "nothing was equipped");
    CHECK(me()->inventory[0].item_id == 10, "and the real item is untouched");

    printf("\nTEST 5: equipping an item that is not in the named slot is refused\n");
    /* The slot and the item id must agree, or a client could name any slot and
     * have whatever is there equipped as whatever it claims. */
    reset_player();
    assert(inventory_add(me()->inventory, 10, 1, 1, 0) == 0);
    send_equip(11, 0, SLOT_BOOTS);         /* slot 0 holds a helm, not boots */
    CHECK(me()->equipment[EQUIP_BOOTS].instance_id == 0, "nothing was equipped");
    CHECK(me()->inventory[0].item_id == 10, "the helm is still in slot 0");

    /* ---------------------------------------------------------------- */
    printf("\nTEST 6: an off-hand cannot be worn alongside a two-handed weapon\n");
    reset_player();
    assert(inventory_add(me()->inventory, 20, 1, 1, 0) == 0);   /* greatsword */
    send_equip(20, 0, SLOT_MAIN_HAND);
    CHECK(me()->equipment[EQUIP_MAIN_HAND].item_id == 20, "the greatsword is worn");

    reset_io();
    assert(inventory_add(me()->inventory, 21, 1, 1, 0) == 0);   /* shield */
    send_equip(21, 0, SLOT_OFF_HAND);
    CHECK(me()->equipment[EQUIP_SECOND_HAND].instance_id == 0, "the shield was refused");
    CHECK(me()->inventory[0].item_id == 21, "and stayed in the bag");

    /* ---------------------------------------------------------------- */
    printf("\nTEST 6b: a two-hander displaces the worn off-hand, and says so\n");
    /* The displacement touches two slots beyond the two an equip normally
     * changes: the bag slot the off-hand lands in, and the off-hand equipment
     * slot it left. Those were not in the update, so the client went on drawing
     * the shield as worn and the bag slot as empty -- and nothing corrected it,
     * because a successful equip is answered with a stats request and stats
     * carry no inventory. */
    reset_player();
    assert(inventory_add(me()->inventory, 21, 1, 1, 0) == 0);   /* shield  -> slot 0 */
    send_equip(21, 0, SLOT_OFF_HAND);
    CHECK(me()->equipment[EQUIP_SECOND_HAND].item_id == 21, "the shield is worn");

    reset_io();
    assert(inventory_add(me()->inventory, 20, 1, 1, 0) == 0);   /* greatsword -> slot 0 */
    uint64_t shield_instance = me()->equipment[EQUIP_SECOND_HAND].instance_id;
    send_equip(20, 0, SLOT_MAIN_HAND);

    CHECK(me()->equipment[EQUIP_MAIN_HAND].item_id == 20, "the greatsword went on");
    CHECK(me()->equipment[EQUIP_SECOND_HAND].instance_id == 0, "the off-hand was emptied");
    {
        int shield_slot = -1;
        for (int i = 0; i < INVENTORY_SLOTS; i++)
            if (me()->inventory[i].instance_id == shield_instance) shield_slot = i;
        CHECK(shield_slot >= 0, "the same shield instance is back in the bag");

        /* Four slots, in any order: the bag slot the sword came from, the main
         * hand it went to, the bag slot the shield landed in, and the off-hand
         * it vacated. */
        int saw_main = 0, saw_off = 0, saw_shield = 0;
        for (int i = 0; i < g_synced_count; i++) {
            if (g_synced[i] == EQUIP_SLOT_BASE + EQUIP_MAIN_HAND)   saw_main = 1;
            if (g_synced[i] == EQUIP_SLOT_BASE + EQUIP_SECOND_HAND) saw_off = 1;
            if (shield_slot >= 0 && g_synced[i] == (uint16_t)shield_slot) saw_shield = 1;
        }
        CHECK(g_synced_count == 4, "four slots were resynchronized, not two");
        CHECK(saw_main,   "the main hand is among them");
        CHECK(saw_off,    "the vacated off-hand is among them");
        CHECK(saw_shield, "and the bag slot the shield moved into");
    }

    /* ---------------------------------------------------------------- */
    printf("\nTEST 7: unequipping returns the same instance to the first free slot\n");
    reset_player();
    assert(inventory_add(me()->inventory, 10, 1, 1, 0) == 0);
    uint64_t worn = me()->inventory[0].instance_id;
    send_equip(10, 0, SLOT_HELMET);
    reset_io();
    send_unequip(SLOT_HELMET);
    CHECK(me()->equipment[EQUIP_HELMET].instance_id == 0, "nothing is worn");
    CHECK(me()->inventory[0].instance_id == worn, "and the same instance is back in the bag");
    CHECK(g_bonus_recalcs == 1, "equipment bonuses were recalculated");
    {
        const UnequipItemResponsePacket* r = reply_of(PACKET_UNEQUIP_ITEM_RESPONSE);
        CHECK(r && r->success == 1, "the client was told it succeeded");
    }

    printf("\nTEST 8: unequipping an empty slot changes nothing\n");
    reset_player();
    reset_io();
    send_unequip(SLOT_HELMET);
    CHECK(occupied() == 0, "no item appeared from nowhere");

    printf("\nTEST 9: unequipping into a full bag is refused, not dropped\n");
    /* The item must not vanish because there was nowhere to put it. */
    reset_player();
    assert(inventory_add(me()->inventory, 10, 1, 1, 0) == 0);
    send_equip(10, 0, SLOT_HELMET);
    /* Filled slot by slot rather than through inventory_add(), which merges
     * into an existing stack and would fill fifteen slots with a hundred and
     * fifty units of a ten-stack item. */
    for (int i = 0; i < INVENTORY_SLOTS; i++) {
        me()->inventory[i].item_id     = 31;
        me()->inventory[i].quantity    = 1;
        me()->inventory[i].instance_id = 5000 + (uint64_t)i;
    }
    CHECK(inventory_first_free(me()->inventory) == -1, "the bag is genuinely full");

    reset_io();
    send_unequip(SLOT_HELMET);
    CHECK(me()->equipment[EQUIP_HELMET].item_id == 10, "the helm is still worn");
    {
        const UnequipItemResponsePacket* r = reply_of(PACKET_UNEQUIP_ITEM_RESPONSE);
        CHECK(r && r->success == 0, "and the refusal was reported");
    }

    /* ---------------------------------------------------------------- */
    printf("\nTEST 10: using a potion heals, consumes one, and names the slot\n");
    reset_player();
    assert(inventory_add(me()->inventory, 30, 5, 20, 0) == 0);
    reset_io();
    send_use(0);

    CHECK(me()->health == 75, "health went from 50 to 75");
    CHECK(me()->inventory[0].quantity == 4, "one unit of five was consumed");
    {
        const UseItemResponsePacket* r = reply_of(PACKET_USE_ITEM_RESPONSE);
        CHECK(r && r->success == 1, "the client was told it succeeded");
        /* The slot the request named, not a search for the item id: the same
         * item in two stacks used to make the client decrement the wrong one. */
        CHECK(r && r->inventory_slot == 0, "the response names the slot that was used");
        CHECK(r && ntohl(r->health_changed) == 25, "and how much it healed");
    }

    printf("\nTEST 11: healing is capped at the missing amount\n");
    reset_player();
    me()->health = 95;
    assert(inventory_add(me()->inventory, 30, 1, 20, 0) == 0);
    reset_io();
    send_use(0);
    CHECK(me()->health == 100, "health stops at max");
    {
        const UseItemResponsePacket* r = reply_of(PACKET_USE_ITEM_RESPONSE);
        CHECK(r && ntohl(r->health_changed) == 5, "and only the missing 5 is reported");
    }

    printf("\nTEST 12: the last unit of a stack empties the slot\n");
    reset_player();
    assert(inventory_add(me()->inventory, 30, 1, 20, 0) == 0);
    reset_io();
    send_use(0);
    CHECK(me()->inventory[0].instance_id == 0, "the slot is empty, not a zero-quantity stack");

    printf("\nTEST 13: a non-consumable cannot be used\n");
    reset_player();
    assert(inventory_add(me()->inventory, 31, 1, 10, 0) == 0);   /* a rock */
    reset_io();
    send_use(0);
    CHECK(me()->inventory[0].quantity == 1, "the rock was not consumed");
    {
        const UseItemResponsePacket* r = reply_of(PACKET_USE_ITEM_RESPONSE);
        CHECK(r && r->success == 0, "and the refusal was reported");
    }

    printf("\nTEST 14: using an empty slot consumes nothing\n");
    reset_player();
    reset_io();
    send_use(0);
    send_use(200);
    CHECK(occupied() == 0, "nothing happened");

    /* ---------------------------------------------------------------- */
    printf("\nTEST 15: dropping removes one unit and creates the ground item\n");
    reset_player();
    assert(inventory_add(me()->inventory, 30, 3, 20, 0) == 0);
    reset_io();
    send_drop(0);
    CHECK(me()->inventory[0].quantity == 2, "one unit left the bag");
    CHECK(g_drops == 1, "exactly one ground item was created");
    CHECK(g_dropped_item == 30 && g_dropped_qty == 1,
          "and it is the item and quantity that left");

    printf("\nTEST 16: dropping an empty or out-of-range slot creates nothing\n");
    reset_player();
    reset_io();
    send_drop(0);
    send_drop(200);
    CHECK(g_drops == 0, "no ground item was created out of nothing");

    /* ---------------------------------------------------------------- */
    printf("\nTEST 17: moving an item keeps its instance and empties the source\n");
    reset_player();
    assert(inventory_add(me()->inventory, 10, 1, 1, 0) == 0);
    uint64_t moved = me()->inventory[0].instance_id;
    reset_io();
    send_move(0, 40);
    CHECK(me()->inventory[40].instance_id == moved, "the same instance is in slot 40");
    CHECK(me()->inventory[0].instance_id == 0, "and slot 0 is empty");
    CHECK(g_synced_count == 2, "both endpoints were resynchronized");
    {
        const MoveItemResponsePacket* r = reply_of(PACKET_MOVE_ITEM_RESPONSE);
        CHECK(r && r->from_slot == 0 && r->to_slot == 40,
              "the response names both slots, so a client can undo its own guess");
    }

    printf("\nTEST 18: moving onto a partial stack merges up to max_stack\n");
    reset_player();
    /* Two stacks of the same potion, one nearly full. */
    me()->inventory[0].item_id = 30; me()->inventory[0].quantity = 15;
    me()->inventory[0].instance_id = 900;
    me()->inventory[1].item_id = 30; me()->inventory[1].quantity = 10;
    me()->inventory[1].instance_id = 901;
    reset_io();
    send_move(1, 0);
    CHECK(me()->inventory[0].quantity == 20, "the destination filled to max_stack of 20");
    CHECK(me()->inventory[1].quantity == 5,  "and the overflow stayed in the source");
    CHECK(me()->inventory[0].quantity + me()->inventory[1].quantity == 25,
          "no units were created or lost");

    printf("\nTEST 19: an out-of-range move slot is refused, not indexed\n");
    reset_player();
    assert(inventory_add(me()->inventory, 10, 1, 1, 0) == 0);
    reset_io();
    send_move(0, 200);
    send_move(200, 0);
    send_move(255, 255);
    CHECK(me()->inventory[0].item_id == 10, "the item did not move");
    CHECK(g_reply_count == 0, "and no response claimed it had");

    /* ---------------------------------------------------------------- */
    printf("\nTEST 20: every handler refuses a packet one byte short\n");
    reset_player();
    assert(inventory_add(me()->inventory, 30, 5, 20, 0) == 0);
    reset_io();
    {
        EquipItemPacket   e = {0}; e.header.type = PACKET_EQUIP_ITEM;
        UnequipItemPacket u = {0}; u.header.type = PACKET_UNEQUIP_ITEM;
        UseItemPacket     s = {0}; s.header.type = PACKET_USE_ITEM;
        DropItemPacket    d = {0}; d.header.type = PACKET_DROP_ITEM;
        MoveItemPacket    m = {0}; m.header.type = PACKET_MOVE_ITEM;

        handle_equip_item  (CLIENT_FD, CHAR_ID, (uint8_t*)&e, (ssize_t)sizeof(e) - 1);
        handle_unequip_item(CLIENT_FD, CHAR_ID, (uint8_t*)&u, (ssize_t)sizeof(u) - 1);
        handle_use_item    (CLIENT_FD, CHAR_ID, (uint8_t*)&s, (ssize_t)sizeof(s) - 1);
        handle_drop_item   (CLIENT_FD, CHAR_ID, (uint8_t*)&d, (ssize_t)sizeof(d) - 1);
        handle_move_item   (CLIENT_FD, CHAR_ID, (uint8_t*)&m, (ssize_t)sizeof(m) - 1);
    }
    CHECK(g_reply_count == 0, "no handler answered a short packet");
    CHECK(me()->inventory[0].quantity == 5, "and none of them touched the bag");
    CHECK(g_drops == 0, "nor the ground");

    printf("\nTEST 21: an unloaded player is not served\n");
    /* A character whose slot exists but whose data has not arrived yet has an
     * inventory of zeroes; acting on it would be acting on nothing. */
    reset_player();
    assert(inventory_add(me()->inventory, 30, 5, 20, 0) == 0);
    me()->is_loaded = 0;
    reset_io();
    send_use(0);
    send_drop(0);
    send_move(0, 1);
    send_equip(30, 0, SLOT_HELMET);
    send_unequip(SLOT_HELMET);
    CHECK(me()->inventory[0].quantity == 5, "the bag is untouched");
    CHECK(g_drops == 0, "nothing reached the ground");
    CHECK(g_reply_count == 0, "and nothing was sent back");

    /* ---------------------------------------------------------------- */
    printf("\nTEST 22: a name query trimmed to the ids asked about is answered\n");
    /* The client sends offsetof(character_ids) + count * 4, not the whole
     * 32-entry array -- the same trimming the response uses. The handler
     * demanded sizeof(NameQueryRequestPacket) and returned without a word for
     * anything smaller, so unless a client happened to have exactly 32 names
     * pending, no player ever learned another player's name. */
    reset_player();
    snprintf(me()->username, sizeof(me()->username), "%s", "Named");
    reset_io();
    {
        NameQueryRequestPacket req;
        memset(&req, 0, sizeof(req));
        req.header.type = PACKET_NAME_QUERY_REQUEST;
        req.count = 1;
        req.character_ids[0] = htonl(CHAR_ID);

        size_t trimmed = offsetof(NameQueryRequestPacket, character_ids)
                       + sizeof(uint32_t);
        handle_name_query_request(1, (uint8_t*)&req, (ssize_t)trimmed);

        const NameQueryResponsePacket* r = reply_of(PACKET_NAME_QUERY_RESPONSE);
        CHECK(r != NULL, "a one-name query is answered");
        if (r) {
            CHECK(r->count == 1, "with exactly one entry");
            CHECK(ntohl(r->entries[0].character_id) == CHAR_ID, "naming the id asked about");
            CHECK(strcmp(r->entries[0].name, "Named") == 0, "and carrying the name");
        }
    }

    printf("\nTEST 23: a name query claiming more ids than it carries is clamped\n");
    /* The count is believed only as far as the bytes that arrived, so a short
     * packet claiming a full array cannot walk off the end of it. */
    reset_io();
    {
        NameQueryRequestPacket req;
        memset(&req, 0, sizeof(req));
        req.header.type = PACKET_NAME_QUERY_REQUEST;
        req.count = MAX_NAME_QUERY;          /* claims 32 */
        req.character_ids[0] = htonl(CHAR_ID);

        size_t trimmed = offsetof(NameQueryRequestPacket, character_ids)
                       + sizeof(uint32_t);   /* carries 1 */
        handle_name_query_request(1, (uint8_t*)&req, (ssize_t)trimmed);

        const NameQueryResponsePacket* r = reply_of(PACKET_NAME_QUERY_RESPONSE);
        CHECK(r && r->count == 1, "only the id that actually arrived is read");
    }

    printf("\nTEST 24: a name query with no room for even one id is dropped\n");
    reset_io();
    {
        NameQueryRequestPacket req;
        memset(&req, 0, sizeof(req));
        req.header.type = PACKET_NAME_QUERY_REQUEST;
        req.count = 4;
        handle_name_query_request(1, (uint8_t*)&req,
                                  (ssize_t)offsetof(NameQueryRequestPacket, character_ids));
        CHECK(reply_of(PACKET_NAME_QUERY_RESPONSE) == NULL,
              "a request carrying no identifiers is answered with nothing");
    }

    remove(items_path);
    items_cleanup();
    pthread_mutex_destroy(&me()->lock);

    if (g_failures) {
        printf("\n=== inventory handlers: %d check(s) FAILED ===\n", g_failures);
        return 1;
    }
    printf("\n=== inventory handlers: all checks passed ===\n");
    return 0;
}
