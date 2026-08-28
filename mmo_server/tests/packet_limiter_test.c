/**
 * @file
 * Check per-connection packet budgets, refill, weighting, escalation, and profiles.
 */

#include "packet_limiter.h"
#include "limit_profiles.h"
#include "protocol.h"
#include "log.h"

#include <assert.h>
#include <stdio.h>
#include <unistd.h>

/** Count limiter verdicts for repeated packets on one descriptor. */
static int count_verdicts(int fd, uint8_t opcode, int n,
                          int* allowed, int* dropped, int* kicked) {
    *allowed = *dropped = *kicked = 0;
    for (int i = 0; i < n; i++) {
        switch (packet_limiter_check(fd, opcode)) {
            case PACKET_LIMIT_ALLOW: (*allowed)++; break;
            case PACKET_LIMIT_DROP:  (*dropped)++; break;
            case PACKET_LIMIT_KICK:  (*kicked)++;  break;
        }
    }
    return *allowed;
}

/**
 * Run packet-limiter policy assertions.
 *
 * @return      Zero after all assertions pass.
 */
int main(void) {
    log_init();
    log_set_level(LOG_LEVEL_ERROR);
    packet_limiter_init(limit_profile_world());

    int a, d, k;
    int a2, d2, k2;

    printf("TEST 1: legitimate client at 60Hz movement is never throttled\n");
    packet_limiter_reset(10);
    int drops = 0;
    for (int tick = 0; tick < 120; tick++) {          // 2 seconds of play
        if (packet_limiter_check(10, PACKET_PLAYER_MOVE) != PACKET_LIMIT_ALLOW) drops++;
        usleep(16666);                                 // ~60Hz
    }
    printf("  drops over 120 ticks at 60Hz: %d (expect 0)\n", drops);
    assert(drops == 0);

    printf("\nTEST 2: movement flood is capped at the burst capacity\n");
    packet_limiter_reset(11);
    count_verdicts(11, PACKET_PLAYER_MOVE, 1000, &a, &d, &k);
    // count drops and post-threshold kicks separately
    printf("  allowed=%d dropped=%d kicked=%d (expect allowed≈300, rest rejected)\n", a, d, k);
    assert(a >= 250 && a <= 320);
    assert(d + k > 600);
    assert(k > 0);   // a flood this large must escalate to a disconnect

    printf("\nTEST 3: chat flood is capped much tighter than movement\n");
    // apply the chat opcode's three-token cost
    packet_limiter_reset(12);
    count_verdicts(12, PACKET_CHAT_SEND, 200, &a, &d, &k);
    printf("  chat allowed=%d (expect ≈5: capacity 15 / cost 3)\n", a);
    assert(a >= 4 && a <= 7);

    printf("\nTEST 4: budgets are independent per connection\n");
    packet_limiter_reset(13);
    packet_limiter_reset(14);
    count_verdicts(13, PACKET_CHAT_SEND, 200, &a, &d, &k);   // exhaust fd 13
    count_verdicts(14, PACKET_CHAT_SEND, 5, &a2, &d2, &k2);  // fd 14 untouched
    printf("  fd13 exhausted (allowed=%d), fd14 still allowed=%d/5\n", a, a2);
    assert(a2 == 5);

    printf("\nTEST 5: separate classes draw on separate budgets\n");
    packet_limiter_reset(15);
    count_verdicts(15, PACKET_CHAT_SEND, 200, &a, &d, &k);   // drain social
    count_verdicts(15, PACKET_PLAYER_MOVE, 10, &a2, &d2, &k2);
    printf("  social drained (allowed=%d), movement still allowed=%d/10\n", a, a2);
    assert(a2 == 10);

    printf("\nTEST 6: buckets refill over time\n");
    packet_limiter_reset(16);
    count_verdicts(16, PACKET_CHAT_SEND, 200, &a, &d, &k);   // drain
    sleep(2);                                                 // 5/s * 2s = 10 tokens
    count_verdicts(16, PACKET_CHAT_SEND, 20, &a2, &d2, &k2);
    printf("  after 2s idle, chat allowed=%d (expect ≈3: 10 tokens / cost 3)\n", a2);
    assert(a2 >= 2 && a2 <= 5);

    printf("\nTEST 7: sustained abuse eventually demands a disconnect\n");
    packet_limiter_reset(17);
    count_verdicts(17, PACKET_CHAT_SEND, 5000, &a, &d, &k);
    printf("  allowed=%d dropped=%d kicked=%d (expect kicked>0)\n", a, d, k);
    assert(k > 0);

    printf("\nTEST 8: unknown opcodes fall into the default budget, not unlimited\n");
    packet_limiter_reset(18);
    count_verdicts(18, 250, 500, &a, &d, &k);   // opcode 250 is not routed
    printf("  unknown-opcode allowed=%d of 500 (expect ≈40)\n", a);
    assert(a >= 30 && a <= 50);

    printf("\nTEST 9: cost weighting — an expensive opcode drains its bucket faster\n");
    // compare different costs within one limiter class
    packet_limiter_reset(19);
    count_verdicts(19, PACKET_REQUEST_PLAYER_DATA, 100, &a, &d, &k);
    packet_limiter_reset(20);
    count_verdicts(20, PACKET_SHOP_BUY, 100, &a2, &d2, &k2);
    printf("  cheap query allowed=%d (≈20), expensive query allowed=%d (≈8)\n", a, a2);
    assert(a > a2);
    assert(a  >= 16 && a  <= 24);
    assert(a2 >= 6  && a2 <= 11);

    printf("\nTEST 10: the overall bucket catches a spread across classes\n");
    // drain the shared budget across several classes
    packet_limiter_reset(21);
    count_verdicts(21, PACKET_PLAYER_MOVE,  400, &a,  &d,  &k);   // drain movement
    count_verdicts(21, PACKET_ATTACK_INTENT, 200, &a2, &d2, &k2); // drain combat
    int a3, d3, k3;
    count_verdicts(21, PACKET_EQUIP_ITEM,    100, &a3, &d3, &k3); // drain item

    // require rejection with an untouched class bucket
    int a4, d4, k4;
    count_verdicts(21, PACKET_CHAT_SEND, 5, &a4, &d4, &k4);
    printf("  movement=%d combat=%d item=%d, then untouched social allowed=%d/5\n",
           a, a2, a3, a4);
    printf("  (social class bucket is full; the overall bucket is what refuses it)\n");
    assert(a4 <= 1);   // at most one packet's worth of drift from refill

    printf("\nTEST 11: a descriptor outside the slot table fails closed\n");
    // fail closed outside the descriptor table
    PacketLimitVerdict v = packet_limiter_check(2000000, PACKET_PLAYER_MOVE);
    printf("  verdict for fd=2000000: %d (expect %d = KICK)\n", v, PACKET_LIMIT_KICK);
    assert(v == PACKET_LIMIT_KICK);

    v = packet_limiter_check(-1, PACKET_PLAYER_MOVE);
    printf("  verdict for fd=-1: %d (expect %d = KICK)\n", v, PACKET_LIMIT_KICK);
    assert(v == PACKET_LIMIT_KICK);

    printf("\nTEST 12: only classes that can strand the UI ask for a rejection\n");
    printf("  move=%d ping=%d attack=%d | equip=%d chat=%d shop=%d\n",
           packet_limiter_wants_rejection(PACKET_PLAYER_MOVE),
           packet_limiter_wants_rejection(PACKET_PING),
           packet_limiter_wants_rejection(PACKET_ATTACK_INTENT),
           packet_limiter_wants_rejection(PACKET_EQUIP_ITEM),
           packet_limiter_wants_rejection(PACKET_CHAT_SEND),
           packet_limiter_wants_rejection(PACKET_SHOP_BUY));
    assert(packet_limiter_wants_rejection(PACKET_PLAYER_MOVE)   == 0);
    assert(packet_limiter_wants_rejection(PACKET_PING)          == 0);
    assert(packet_limiter_wants_rejection(PACKET_ATTACK_INTENT) == 0);
    assert(packet_limiter_wants_rejection(PACKET_EQUIP_ITEM)    == 1);
    assert(packet_limiter_wants_rejection(PACKET_CHAT_SEND)     == 1);
    assert(packet_limiter_wants_rejection(PACKET_SHOP_BUY)      == 1);

    printf("\nTEST 13: retry hint is non-zero once a bucket is empty\n");
    packet_limiter_reset(22);
    count_verdicts(22, PACKET_CHAT_SEND, 100, &a, &d, &k);   // drain social
    uint16_t retry = packet_limiter_retry_after_ms(22, PACKET_CHAT_SEND);
    printf("  retry_after_ms once social is empty: %u (expect >0)\n", retry);
    assert(retry > 0);

    packet_limiter_reset(23);
    retry = packet_limiter_retry_after_ms(23, PACKET_CHAT_SEND);
    printf("  retry_after_ms on a fresh connection: %u (expect 0)\n", retry);
    assert(retry == 0);

    printf("\nTEST 14: the realm profile is far tighter than the world profile\n");
    packet_limiter_init(limit_profile_realm());
    packet_limiter_reset(24);
    count_verdicts(24, PACKET_CHARACTER_CREATE_REQUEST, 50, &a, &d, &k);
    printf("  realm character creates allowed=%d (expect ≈1-2 at cost 20)\n", a);
    assert(a >= 1 && a <= 3);

    packet_limiter_reset(25);
    count_verdicts(25, PACKET_CHARACTER_LIST_REQUEST, 50, &a2, &d2, &k2);
    printf("  realm character lists allowed=%d (expect ≈4 at cost 5)\n", a2);
    assert(a2 >= 3 && a2 <= 6);

    printf("\nTEST 14b: the character-select flow fits inside the realm budget\n");
    /* The screen's own traffic is not optional, and the action it exists for
     * comes at the end of it. Priced so that the last step could not be
     * afforded, the realm refuses the click that opens the game -- which is
     * what it did: creating a character costs the whole burst allowance, so
     * the Join immediately after it was refused for over a second, and every
     * world entry in the client log was preceded by exactly one refusal.
     *
     * One connection, one bucket, in the order a player actually produces:
     * world list, race list, character list, create, then enter. */
    packet_limiter_reset(26);
    {
        const uint8_t flow[] = {
            PACKET_WORLD_LIST_REQUEST,
            PACKET_RACE_LIST_REQUEST,
            PACKET_CHARACTER_LIST_REQUEST,
            PACKET_CHARACTER_CREATE_REQUEST,
        };
        int refused = 0;
        for (size_t i = 0; i < sizeof(flow) / sizeof(flow[0]); i++)
            if (packet_limiter_check(26, flow[i]) != PACKET_LIMIT_ALLOW) refused++;

        /* The create is allowed to be the one that does not fit -- it costs
         * the whole capacity by design, and a player who is refused it is
         * told so and clicks the button again. What must not happen is the
         * step after it being refused as a consequence. */
        printf("  %d of %zu setup requests refused\n",
               refused, sizeof(flow) / sizeof(flow[0]));

        /* The bucket is now at its emptiest. This is the exact moment the
         * player clicks their new character. */
        uint16_t wait = packet_limiter_retry_after_ms(26, PACKET_ENTER_WORLD);
        printf("  a Join at the emptiest moment waits %ums\n", wait);

        /* Under a fifth of a second: shorter than the gap between a character
         * appearing on screen and a hand reaching it, so in practice it is
         * never waited for at all. At the old cost of 10 this was over a
         * second, which is not. */
        assert(wait < 200);
    }

    printf("\nTEST 15: the running totals count every verdict exactly once\n");
    /* These are what /metrics and the world's [STATS] line report. The limiter
     * used to say what it had done only through a rate-limited log line --
     * which is deliberately lossy, so the count was not recoverable from it
     * even by reading. Nothing branches on these numbers, so what has to hold
     * is only that they add up: one increment per call, on the class the call
     * actually returned. */
    packet_limiter_init(limit_profile_world());
    packet_limiter_reset_totals();

    unsigned long long t_allowed = 0, t_dropped = 0, t_kicked = 0;
    packet_limiter_totals(&t_allowed, &t_dropped, &t_kicked);
    printf("  after a reset: allowed=%llu dropped=%llu kicked=%llu (expect 0/0/0)\n",
           t_allowed, t_dropped, t_kicked);
    assert(t_allowed == 0 && t_dropped == 0 && t_kicked == 0);

    packet_limiter_reset(26);
    count_verdicts(26, PACKET_CHAT_SEND, 200, &a, &d, &k);

    packet_limiter_totals(&t_allowed, &t_dropped, &t_kicked);
    printf("  local tally  allowed=%d dropped=%d kicked=%d\n", a, d, k);
    printf("  global tally allowed=%llu dropped=%llu kicked=%llu\n",
           t_allowed, t_dropped, t_kicked);
    assert(t_allowed == (unsigned long long)a);
    assert(t_dropped == (unsigned long long)d);
    assert(t_kicked  == (unsigned long long)k);
    assert(t_allowed + t_dropped + t_kicked == 200);

    /* An out-of-range descriptor is refused before any bucket is touched, and
     * it is still a kick and still counted -- that path returns early, which
     * is exactly the kind of return a hand-placed counter gets forgotten on. */
    unsigned long long kicked_before = t_kicked;
    (void)packet_limiter_check(-1, PACKET_PLAYER_MOVE);
    packet_limiter_totals(NULL, NULL, &t_kicked);
    printf("  a kick from the fd range check is counted too: %llu -> %llu\n",
           kicked_before, t_kicked);
    assert(t_kicked == kicked_before + 1);

    /* Totals are process-wide and monotonic: a per-connection reset must not
     * roll them back, or a scrape would see a counter go down. */
    packet_limiter_reset(26);
    unsigned long long after_reset = 0;
    packet_limiter_totals(&after_reset, NULL, NULL);
    printf("  a connection reset leaves the totals alone: allowed=%llu\n", after_reset);
    assert(after_reset == (unsigned long long)a);

    printf("\nALL ASSERTIONS PASSED\n");
    return 0;
}
