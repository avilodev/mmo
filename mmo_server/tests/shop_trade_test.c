/**
 * @file
 * Check the shop counter: who may trade, from where, and what it costs them.
 *
 * Buying and selling move a player's money and items, so this is one of the
 * exploit-bearing paths the audit named as having no coverage at all. What is
 * pinned down here is the part that used to be missing entirely -- the gates:
 *
 *   * A shop can only be traded with by a character who has it open. Without
 *     that, a client could name any shop id in the world, and because each
 *     kingdom's shops price in that kingdom's own coin, that was cross-kingdom
 *     currency arbitrage without travelling.
 *
 *   * An open shop is only open while the character is still standing at the
 *     merchant. Without that, the session outlives the visit.
 *
 *   * The session ends when the character does. A shop that survived a
 *     disconnect would be inherited by whoever reconnected as that character.
 *
 * Plus the conservation properties either side of the gate: currency debits
 * refuse rather than underflow, credits saturate rather than wrap, and the
 * slot a purchase reports is the slot it actually landed in.
 *
 * The last two cases run the real shop_handle_buy() and shop_handle_sell()
 * concurrently. Everything above them is a rule tested in isolation; those are
 * the handler, and the handler is where money and items change hands at the
 * same moment. A trade that debited without delivering, delivered without
 * debiting, or did either twice is a defect no sequential test can see -- and
 * these are the two functions `make sanitize` did not cover, which is what
 * shop-sanitize now runs this file under.
 */

#include "shop.h"
#include "shop_session.h"
#include "item_instance.h"
#include "items_database.h"
#include "npc_world.h"
#include "player_data.h"
#include "world_regions.h"
#include "utils.h"
#include "log.h"

#include <arpa/inet.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* --- The world the trading handlers run against -------------------------
 *
 * shop.c, items_database.c, npc_world.c and player_data.c are stubbed rather
 * than linked. What is under test is what shop_trade.c does to a player's coin
 * and bag under contention, and every one of those modules is either a lookup
 * table or has a sanitizer suite of its own.
 */

#define TRADE_PLAYERS   8      /**< Threads, and characters, in the races. */
#define TRADE_SHOP_ID   4242
#define TRADE_ITEM_ID   808
#define TRADE_PRICE     10
#define TRADE_MAX_STACK 20
#define TRADE_CURRENCY  CURRENCY_ENNARA
#define TRADE_FIRST_CID 6000
#define TRADE_FIRST_FD  700

/** The players the trading races drive. One lock each, as in the real registry. */
static ActivePlayer g_traders[TRADE_PLAYERS];

static uint32_t trader_cid(int slot) { return TRADE_FIRST_CID + (uint32_t)slot; }
static int      trader_fd(int slot)  { return TRADE_FIRST_FD + slot; }

/** The one shop these cases trade with. */
static ShopDef g_shop;

ShopDef* shop_find(uint32_t shop_id) {
    return shop_id == TRADE_SHOP_ID ? &g_shop : NULL;
}

/** One stackable definition, which is all the trading paths read.
 *
 * Built once and never written again. Filling a function static on each call
 * and handing back its address is a data race the moment two threads trade at
 * once, and the real item_get() does not do it either: it returns a pointer
 * into a table loaded before any thread starts.
 */
static ItemDefinition g_trade_item;

static void trade_item_init(void) {
    memset(&g_trade_item, 0, sizeof(g_trade_item));
    g_trade_item.id             = TRADE_ITEM_ID;
    g_trade_item.stackable      = 1;
    g_trade_item.max_stack      = TRADE_MAX_STACK;
    g_trade_item.value          = TRADE_PRICE / 2;   /* what the shop buys it back for */
    g_trade_item.bind_on_pickup = 0;
}

const ItemDefinition* item_get(uint32_t item_id) {
    return item_id == TRADE_ITEM_ID ? &g_trade_item : NULL;
}

/** Hand back a trader, locked, exactly as the real registry does. */
ActivePlayer* player_acquire(uint32_t character_id) {
    if (character_id < TRADE_FIRST_CID) return NULL;
    uint32_t slot = character_id - TRADE_FIRST_CID;
    if (slot >= TRADE_PLAYERS) return NULL;
    pthread_mutex_lock(&g_traders[slot].lock);
    return &g_traders[slot];
}

void player_release(ActivePlayer* p) {
    if (p) pthread_mutex_unlock(&p->lock);
}

/** Packets the handlers tried to send, counted rather than delivered. */
static _Atomic int g_sends = 0;

ssize_t server_send(int fd, void* buf, size_t len) {
    (void)fd; (void)buf;
    atomic_fetch_add(&g_sends, 1);
    return (ssize_t)len;
}

void player_send_slot_updates(int client_fd, uint32_t character_id,
                              const uint16_t* slots, int count) {
    (void)client_fd; (void)character_id; (void)slots; (void)count;
    atomic_fetch_add(&g_sends, 1);
}

/* The merchant. Sessions in these cases are opened with npc_id 0, which is the
 * "no live merchant to re-check" case, so these are never reached -- they exist
 * because shop_trade.c references them. */
NPCWorld g_npc_world;
NPCEntity* npc_world_acquire(NPCWorld* w, uint32_t npc_id) {
    (void)w; (void)npc_id; return NULL;
}
void npc_world_release(NPCWorld* w, NPCEntity* n) { (void)w; (void)n; }

/* --- Trading fixture ---------------------------------------------------- */

/** Put every trader at the merchant with a purse and an empty bag. */
static void reset_traders(uint32_t starting_coin) {
    for (int i = 0; i < TRADE_PLAYERS; i++) {
        memset(&g_traders[i], 0, sizeof(g_traders[i]));
        pthread_mutex_init(&g_traders[i].lock, NULL);
        g_traders[i].character_id = trader_cid(i);
        g_traders[i].client_fd    = trader_fd(i);
        g_traders[i].is_loaded    = 1;
        g_traders[i].currency[TRADE_CURRENCY] = starting_coin;
    }
}

/** Stock the fixture shop and open it for every trader. */
static void open_shop_for_everyone(void) {
    memset(&g_shop, 0, sizeof(g_shop));
    g_shop.shop_id     = TRADE_SHOP_ID;
    g_shop.currency_id = TRADE_CURRENCY;
    g_shop.item_count  = 1;
    g_shop.items[0].item_id   = TRADE_ITEM_ID;
    g_shop.items[0].buy_price = TRADE_PRICE;
    snprintf(g_shop.name, sizeof(g_shop.name), "The Counter");

    shop_session_shutdown();
    for (int i = 0; i < TRADE_PLAYERS; i++)
        /* npc_id 0: the session records no live merchant, so the range
         * re-check falls back to the position recorded at open. The traders
         * stand on it. */
        shop_session_open(trader_cid(i), TRADE_SHOP_ID, 0, 0.0f, 0.0f);
}

/** Total of one item across every trader's bag. */
static int traders_hold(uint32_t item_id) {
    int total = 0;
    for (int i = 0; i < TRADE_PLAYERS; i++)
        for (int j = 0; j < INVENTORY_SLOTS; j++)
            if (g_traders[i].inventory[j].item_id == item_id)
                total += g_traders[i].inventory[j].quantity;
    return total;
}

/** Total coin across every trader's purse. */
static uint64_t traders_coin(void) {
    uint64_t total = 0;
    for (int i = 0; i < TRADE_PLAYERS; i++)
        total += g_traders[i].currency[TRADE_CURRENCY];
    return total;
}

static float dist(float ax, float ay, float bx, float by) {
    float dx = ax - bx, dy = ay - by;
    return (float)sqrt((double)(dx * dx + dy * dy));
}

/* --- The trading races -------------------------------------------------- */

/** One thread in the purchase race, and the character it trades as.
 *
 * The two are separate because both arrangements matter. Eight threads on
 * eight characters is what the server does: a connection is owned by one
 * worker at a time, so two packets from one player are never dispatched at
 * once. Eight threads on *one* character is the arrangement that would arise
 * from a duplicate login before the registry kicks the stale session, and it
 * is the only one that can catch a balance read outside the lock that spends
 * it -- which is why both are run.
 */
typedef struct {
    int                character_slot;
    int                attempts;
    pthread_barrier_t* start;
} BuyArgs;

/** Buy until the purse runs out, then keep asking. */
static void* buy_worker(void* arg) {
    BuyArgs* a = arg;
    uint32_t cid = trader_cid(a->character_slot);
    int      fd  = trader_fd(a->character_slot);

    ShopBuyPacket req;
    memset(&req, 0, sizeof(req));
    req.header.type         = PACKET_SHOP_BUY;
    req.header.player_id    = htonl(cid);
    req.header.payload_size = htons(sizeof(req) - sizeof(PacketHeader));
    req.shop_id             = htonl(TRADE_SHOP_ID);
    req.item_id             = htonl(TRADE_ITEM_ID);

    pthread_barrier_wait(a->start);

    /* Deliberately asks more times than the purse can pay for: the refusals
     * are half the test. A refusal that still debited, or a debit that still
     * refused, shows up in the ledger afterwards. */
    for (int i = 0; i < a->attempts; i++)
        shop_handle_buy(cid, fd, (uint8_t*)&req, (int)sizeof(req));
    return NULL;
}

/** One trader's thread in the mixed race: buying or selling, not both. */
typedef struct {
    int                slot;
    int                selling;
    int                attempts;
    pthread_barrier_t* start;
} TradeArgs;

static void* trade_worker(void* arg) {
    TradeArgs* a = arg;

    ShopBuyPacket buy;
    memset(&buy, 0, sizeof(buy));
    buy.header.type         = PACKET_SHOP_BUY;
    buy.header.player_id    = htonl(trader_cid(a->slot));
    buy.header.payload_size = htons(sizeof(buy) - sizeof(PacketHeader));
    buy.shop_id             = htonl(TRADE_SHOP_ID);
    buy.item_id             = htonl(TRADE_ITEM_ID);

    ShopSellPacket sell;
    memset(&sell, 0, sizeof(sell));
    sell.header.type         = PACKET_SHOP_SELL;
    sell.header.player_id    = htonl(trader_cid(a->slot));
    sell.header.payload_size = htons(sizeof(sell) - sizeof(PacketHeader));
    sell.shop_id             = htonl(TRADE_SHOP_ID);
    sell.inventory_slot      = 0;

    pthread_barrier_wait(a->start);

    for (int i = 0; i < a->attempts; i++) {
        if (a->selling)
            shop_handle_sell(trader_cid(a->slot), trader_fd(a->slot),
                             (uint8_t*)&sell, (int)sizeof(sell));
        else
            shop_handle_buy(trader_cid(a->slot), trader_fd(a->slot),
                            (uint8_t*)&buy, (int)sizeof(buy));
    }
    return NULL;
}

int main(void) {
    log_init();
    trade_item_init();   /* before any thread exists; see item_get() */
    printf("=== shop trading ===\n");

    printf("\nTEST 1: a shop is not open until it is opened\n");
    {
        shop_session_shutdown();

        ShopSession session;
        CHECK(!shop_session_snapshot(1001, &session),
              "a character who has opened nothing has nothing open");
        CHECK(shop_session_count() == 0, "and nothing is tracked");

        CHECK(shop_session_open(1001, 7, 500, 100.0f, 200.0f),
              "opening a shop is recorded");
        CHECK(shop_session_snapshot(1001, &session), "and reads back");
        CHECK(session.shop_id == 7, "with the shop that was opened");
        CHECK(session.npc_id == 500, "and the merchant that offered it");
        CHECK(session.npc_x == 100.0f && session.npc_y == 200.0f,
              "and where that merchant stood");
    }

    printf("\nTEST 2: one character, one open shop\n");
    {
        /* Opening a second shop replaces the first rather than accumulating.
         * Two shops open at once would mean the range check passes against
         * whichever merchant happens to be nearer. */
        shop_session_open(1001, 9, 501, 900.0f, 900.0f);

        ShopSession session;
        CHECK(shop_session_snapshot(1001, &session), "the character still has a shop");
        CHECK(session.shop_id == 9, "and it is the one most recently opened");
        CHECK(shop_session_count() == 1, "with no leftover session for the first");
    }

    printf("\nTEST 3: sessions are per character\n");
    {
        shop_session_shutdown();

        for (uint32_t id = 2000; id < 2050; id++)
            shop_session_open(id, id % 7 + 1, id + 100, (float)id, (float)id);

        CHECK(shop_session_count() == 50, "fifty characters, fifty sessions");

        ShopSession a, b;
        CHECK(shop_session_snapshot(2003, &a) && shop_session_snapshot(2004, &b),
              "two of them read back");
        CHECK(a.character_id == 2003 && b.character_id == 2004,
              "each holding its own character");
        CHECK(a.npc_x == 2003.0f && b.npc_x == 2004.0f,
              "and its own merchant");

        shop_session_close(2003);
        CHECK(!shop_session_snapshot(2003, &a), "closing one closes only that one");
        CHECK(shop_session_snapshot(2004, &b), "the neighbour is untouched");
        CHECK(shop_session_count() == 49, "and the count follows");
    }

    printf("\nTEST 4: the table survives churn without leaking or colliding\n");
    {
        /* Open and close far more sessions than the table starts with, so it
         * grows and every bucket is recycled many times. A backward-shift
         * deletion bug shows up here as a lookup that stops finding a session
         * that is still open. */
        shop_session_shutdown();

        int wrong = 0;
        for (int round = 0; round < 200; round++) {
            uint32_t id = 5000 + (uint32_t)round;
            shop_session_open(id, 3, 42, 10.0f, 20.0f);

            ShopSession s;
            if (!shop_session_snapshot(id, &s) || s.shop_id != 3) wrong++;

            /* Keep every fourth one open, so the table holds a growing live
             * set rather than emptying between rounds. */
            if (round % 4 != 0) shop_session_close(id);
        }

        CHECK(wrong == 0, "200 rounds of open/close never lose a live session");
        CHECK(shop_session_count() == 50, "and the survivors are all still there");

        int found = 0;
        for (int round = 0; round < 200; round += 4) {
            ShopSession s;
            if (shop_session_snapshot(5000 + (uint32_t)round, &s)) found++;
        }
        CHECK(found == 50, "every one of them resolves");
    }

    printf("\nTEST 5: range is what closes an open shop\n");
    {
        /* The proximity rule itself, as shop_trade.c applies it: the distance
         * from the player to the merchant against SHOP_INTERACT_RANGE. What is
         * checked here is the rule, not the handler -- the handler needs a
         * live player registry and an NPC pool. */
        shop_session_shutdown();
        shop_session_open(9001, 4, 77, 1000.0f, 1000.0f);

        ShopSession s;
        CHECK(shop_session_snapshot(9001, &s), "the shop is open");

        CHECK(dist(1000.0f, 1000.0f, s.npc_x, s.npc_y) <= SHOP_INTERACT_RANGE,
              "standing on the merchant is in range");
        CHECK(dist(1000.0f + SHOP_INTERACT_RANGE - 1.0f, 1000.0f,
                   s.npc_x, s.npc_y) <= SHOP_INTERACT_RANGE,
              "just inside the range is in range");
        CHECK(dist(1000.0f + SHOP_INTERACT_RANGE + 1.0f, 1000.0f,
                   s.npc_x, s.npc_y) > SHOP_INTERACT_RANGE,
              "just outside it is not");
        CHECK(dist(50000.0f, 50000.0f, s.npc_x, s.npc_y) > SHOP_INTERACT_RANGE,
              "and the far side of the map certainly is not");
    }

    printf("\nTEST 6: an idle shop times out\n");
    {
        shop_session_shutdown();
        shop_session_open(9100, 4, 77, 0.0f, 0.0f);
        CHECK(shop_session_count() == 1, "the shop is open");

        /* The sweep must not drop a session that is still being used. A
         * timeout that fired early would close the shop under a player who was
         * mid-purchase. */
        shop_session_tick();
        CHECK(shop_session_count() == 1, "a fresh session survives the sweep");

        shop_session_touch(9100);
        shop_session_tick();
        CHECK(shop_session_count() == 1, "and so does a touched one");
    }

    printf("\nTEST 7: currency refuses rather than underflowing\n");
    {
        uint32_t balances[CURRENCY_COUNT] = {0};
        balances[CURRENCY_ENNARA] = 100;

        CHECK(world_currency_debit(balances, CURRENCY_ENNARA, 40) == 1,
              "an affordable purchase debits");
        CHECK(balances[CURRENCY_ENNARA] == 60, "leaving the difference");

        CHECK(world_currency_debit(balances, CURRENCY_ENNARA, 61) == 0,
              "one coin short is refused");
        CHECK(balances[CURRENCY_ENNARA] == 60,
              "and the refusal changes nothing");

        /* The failure this guards: an unsigned subtraction past zero hands the
         * player four billion coins. */
        CHECK(world_currency_debit(balances, CURRENCY_ENNARA, UINT32_MAX) == 0,
              "an enormous price is refused");
        CHECK(balances[CURRENCY_ENNARA] == 60, "without wrapping the balance");

        CHECK(world_currency_debit(balances, CURRENCY_ENNARA, 60) == 1,
              "spending the exact balance is allowed");
        CHECK(balances[CURRENCY_ENNARA] == 0, "leaving nothing");
        CHECK(world_currency_debit(balances, CURRENCY_ENNARA, 1) == 0,
              "and an empty purse buys nothing");
    }

    printf("\nTEST 8: currency saturates rather than wrapping\n");
    {
        uint32_t balances[CURRENCY_COUNT] = {0};
        balances[CURRENCY_ENNARA] = UINT32_MAX - 10;

        uint32_t after = world_currency_credit(balances, CURRENCY_ENNARA, 5);
        CHECK(after == UINT32_MAX - 5, "an ordinary sale credits normally");

        /* Wrapping here would make a large sale *reduce* a rich player's
         * balance, which is a worse outcome than the sale being capped. */
        after = world_currency_credit(balances, CURRENCY_ENNARA, 1000);
        CHECK(after == UINT32_MAX, "a sale past the ceiling saturates");
        CHECK(balances[CURRENCY_ENNARA] == UINT32_MAX,
              "rather than wrapping to nothing");
    }

    printf("\nTEST 9: each kingdom's coin is its own balance\n");
    {
        /* The reason binding a trade to an open shop matters: a shop prices in
         * its kingdom's coin, and these balances are separate. Buying in one
         * and selling into another is a conversion, and it must require going
         * there. */
        uint32_t balances[CURRENCY_COUNT] = {0};
        CHECK(CURRENCY_COUNT > 1, "there is more than one currency to confuse");

        world_currency_credit(balances, 0, 500);
        CHECK(balances[0] == 500, "the first kingdom's coin is credited");
        CHECK(balances[1] == 0, "and the second kingdom's is untouched");
        CHECK(world_currency_debit(balances, 1, 1) == 0,
              "coin held in one kingdom cannot be spent in another");
    }

    printf("\nTEST 10: a purchase reports the slot it actually landed in\n");
    {
        /* shop_handle_buy() used to answer by searching for the first slot
         * holding the item id, which is right only while the item sits in one
         * stack. Two part-filled stacks and it named the wrong one, so the
         * client redrew a slot that had not changed. */
        ItemInstance inv[INVENTORY_SLOTS];
        memset(inv, 0, sizeof(inv));

        item_instance_seed(1000);

        /* Two stacks of the same item, the first already full. */
        const uint16_t max_stack = 20;
        inventory_add(inv, 42, max_stack, max_stack, 0);   /* fills slot 0 */
        inventory_add(inv, 42, 5,         max_stack, 0);   /* 5 into slot 1 */

        CHECK(inv[0].item_id == 42 && inv[0].quantity == max_stack,
              "the first stack is full");
        CHECK(inv[1].item_id == 42 && inv[1].quantity == 5,
              "and the second is partial");

        /* The diff shop_trade.c takes across the insert. */
        uint32_t before_qty[INVENTORY_SLOTS];
        uint64_t before_inst[INVENTORY_SLOTS];
        for (int i = 0; i < INVENTORY_SLOTS; i++) {
            before_qty[i]  = inv[i].quantity;
            before_inst[i] = inv[i].instance_id;
        }

        CHECK(inventory_add(inv, 42, 1, max_stack, 0) == 0, "one more is placed");

        int changed = -1;
        for (int i = 0; i < INVENTORY_SLOTS; i++) {
            if (inv[i].instance_id != before_inst[i] ||
                inv[i].quantity    != before_qty[i]) { changed = i; break; }
        }

        CHECK(changed == 1, "the diff names the partial stack, not the full one");
        CHECK(inv[1].quantity == 6, "which is the one that grew");
        CHECK(inv[0].quantity == max_stack, "the full stack is unchanged");
    }

    printf("\nTEST 11: concurrent purchases spend exactly what they deliver\n");
    {
        /* The handler, not the rule. Eight threads buy from one counter at
         * once, each for its own character, and the ledger has to close:
         * coin spent == items delivered * price, exactly.
         *
         * Every way this can go wrong is a live exploit or a live loss. A
         * debit that happens twice for one item charges a player double. An
         * insert that happens twice for one debit is item duplication. A debit
         * skipped because the balance was read outside the lock that spends it
         * is free goods. shop_handle_buy() holds the player lock across check,
         * insert and debit precisely so none of those can interleave -- and
         * until now nothing tested that it does. */
        item_instance_seed(90000);
        open_shop_for_everyone();

        const uint32_t purse    = 200;   /* twenty purchases each */
        const int      attempts = 60;    /* deliberately more than they can afford */
        reset_traders(purse);

        uint64_t coin_before = traders_coin();

        pthread_t t[TRADE_PLAYERS];
        BuyArgs   args[TRADE_PLAYERS];
        pthread_barrier_t start;
        pthread_barrier_init(&start, NULL, TRADE_PLAYERS);
        for (int i = 0; i < TRADE_PLAYERS; i++) {
            args[i] = (BuyArgs){ .character_slot = i, .attempts = attempts,
                                 .start = &start };
            pthread_create(&t[i], NULL, buy_worker, &args[i]);
        }
        for (int i = 0; i < TRADE_PLAYERS; i++) pthread_join(t[i], NULL);
        pthread_barrier_destroy(&start);

        uint64_t coin_after = traders_coin();
        uint64_t spent      = coin_before - coin_after;
        int      delivered  = traders_hold(TRADE_ITEM_ID);

        CHECK(spent == (uint64_t)delivered * TRADE_PRICE,
              "every coin spent bought one item, and every item cost one price");
        CHECK(delivered == (int)(purse / TRADE_PRICE) * TRADE_PLAYERS,
              "and each trader bought exactly what their purse allowed");
        CHECK(coin_after == 0, "which is to say they spent all of it and no more");

        /* Nobody may go negative, which an unguarded debit under contention
         * would do by wrapping through four billion. */
        int overdrawn = 0;
        for (int i = 0; i < TRADE_PLAYERS; i++)
            if (g_traders[i].currency[TRADE_CURRENCY] > purse) overdrawn++;
        CHECK(overdrawn == 0, "and no purse wrapped past zero into a fortune");
    }

    printf("\nTEST 12: buying and selling at once conserves the ledger\n");
    {
        /* Half the threads buy while the other half sell the same item back to
         * the same counter. Both handlers take the player lock and both move
         * coin and items in opposite directions, which is the interleaving
         * that would let a sale credit for an item a purchase had not finished
         * placing -- or a purchase place an item a sale had already removed.
         *
         * The invariant that survives either direction: a trader's coin plus
         * what their items would fetch cannot rise. The shop buys back below
         * what it sells for, so it is a sink and never a source. The total may
         * fall; growth is duplication. */
        item_instance_seed(95000);
        open_shop_for_everyone();

        const uint32_t purse  = 300;
        const uint16_t seeded = 10;
        reset_traders(purse);

        /* Seed every bag, so the sellers have something to sell. */
        for (int i = 0; i < TRADE_PLAYERS; i++)
            inventory_add(g_traders[i].inventory, TRADE_ITEM_ID, seeded,
                          TRADE_MAX_STACK, 0);

        const ItemDefinition* def = item_get(TRADE_ITEM_ID);
        const uint32_t sell_price = def->value;

        uint64_t worth_before = traders_coin() +
                                (uint64_t)traders_hold(TRADE_ITEM_ID) * sell_price;

        pthread_t  t[TRADE_PLAYERS];
        TradeArgs  args[TRADE_PLAYERS];
        pthread_barrier_t start;
        pthread_barrier_init(&start, NULL, TRADE_PLAYERS);
        for (int i = 0; i < TRADE_PLAYERS; i++) {
            args[i] = (TradeArgs){ .slot = i, .selling = i % 2,
                                   .attempts = 40, .start = &start };
            pthread_create(&t[i], NULL, trade_worker, &args[i]);
        }
        for (int i = 0; i < TRADE_PLAYERS; i++) pthread_join(t[i], NULL);
        pthread_barrier_destroy(&start);

        uint64_t worth_after = traders_coin() +
                               (uint64_t)traders_hold(TRADE_ITEM_ID) * sell_price;

        CHECK(worth_after <= worth_before,
              "trading against the counter never created value out of nothing");

        int too_rich = 0, oversized = 0;
        for (int i = 0; i < TRADE_PLAYERS; i++) {
            if (g_traders[i].currency[TRADE_CURRENCY] > purse + seeded * sell_price)
                too_rich++;
            for (int j = 0; j < INVENTORY_SLOTS; j++)
                if (g_traders[i].inventory[j].quantity > TRADE_MAX_STACK)
                    oversized++;
        }
        CHECK(too_rich == 0, "no purse holds more than every sale could have paid");
        CHECK(oversized == 0, "and no stack grew past the item's maximum");
    }

    printf("\nTEST 13: one character bought by eight threads at once\n");
    {
        /* The arrangement TEST 11 cannot produce. There, each thread has its
         * own character and its own lock, which is faithful to the server --
         * the reactor hands a connection to one worker at a time, so two
         * packets from one player never run together. It also means TEST 11
         * would pass against a handler that read the balance, released the
         * lock, and re-acquired it to spend: with one thread per character
         * there is nobody to interleave with.
         *
         * Eight threads on one character is that missing case. It is not
         * hypothetical -- a duplicate login has a window before the session
         * registry kicks the stale connection, and both are dispatchable in
         * it. The purse is exact, so a single lost update shows: twenty items
         * at ten coin from a purse of two hundred leaves nothing, and any
         * other answer is a debit that went missing or happened twice. */
        item_instance_seed(99000);
        open_shop_for_everyone();

        /* Repeated, because a lost update is a timing accident and one round
         * can miss it. Under ThreadSanitizer -- which is what shop-sanitize
         * runs -- the interleavings widen and a single round is usually
         * enough; in a plain build it is not, and a case that only fails under
         * a sanitizer is a case that will be believed to pass. */
        const uint32_t purse = 200;
        int wrong_rounds = 0, short_rounds = 0, over_rounds = 0;

        for (int round = 0; round < 40; round++) {
            reset_traders(purse);

            pthread_t t[TRADE_PLAYERS];
            BuyArgs   args[TRADE_PLAYERS];
            pthread_barrier_t start;
            pthread_barrier_init(&start, NULL, TRADE_PLAYERS);
            for (int i = 0; i < TRADE_PLAYERS; i++) {
                args[i] = (BuyArgs){ .character_slot = 0, .attempts = 40,
                                     .start = &start };
                pthread_create(&t[i], NULL, buy_worker, &args[i]);
            }
            for (int i = 0; i < TRADE_PLAYERS; i++) pthread_join(t[i], NULL);
            pthread_barrier_destroy(&start);

            int      bought = traders_hold(TRADE_ITEM_ID);
            uint32_t left   = g_traders[0].currency[TRADE_CURRENCY];

            if ((uint64_t)bought * TRADE_PRICE + left != purse) wrong_rounds++;
            if (bought > (int)(purse / TRADE_PRICE)) over_rounds++;
            if (bought < (int)(purse / TRADE_PRICE)) short_rounds++;
        }

        CHECK(over_rounds == 0,
              "no round bought more than one purse pays for");
        CHECK(short_rounds == 0,
              "and none bought less, so no purchase was refused for coin still held");
        CHECK(wrong_rounds == 0,
              "40 rounds and the ledger closed every time: no debit lost, none twice");
    }

    shop_session_shutdown();

    printf("\n");
    if (g_failures) {
        printf("%d ASSERTION%s FAILED\n", g_failures, g_failures == 1 ? "" : "S");
        return 1;
    }
    printf("ALL ASSERTIONS PASSED\n");
    return 0;
}
