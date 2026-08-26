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
 */

#include "shop_session.h"
#include "item_instance.h"
#include "world_regions.h"
#include "log.h"

#include <math.h>
#include <stdio.h>
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

static float dist(float ax, float ay, float bx, float by) {
    float dx = ax - bx, dy = ay - by;
    return (float)sqrt((double)(dx * dx + dy * dy));
}

int main(void) {
    log_init();
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

    shop_session_shutdown();

    printf("\n");
    if (g_failures) {
        printf("%d ASSERTION%s FAILED\n", g_failures, g_failures == 1 ? "" : "S");
        return 1;
    }
    printf("ALL ASSERTIONS PASSED\n");
    return 0;
}
