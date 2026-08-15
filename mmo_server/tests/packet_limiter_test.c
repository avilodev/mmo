// Unit test for the world server's per-connection packet budget.
//
// Build:
//   gcc -Wall -Wextra -pthread -Icommon/include -Iworld_server/include
//       -o packet_limiter_test tests/packet_limiter_test.c
//       world_server/src/packet_limiter.c common/src/log.c

#include "packet_limiter.h"
#include "protocol.h"
#include "log.h"

#include <assert.h>
#include <stdio.h>
#include <unistd.h>

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

int main(void) {
    log_init();
    log_set_level(LOG_LEVEL_ERROR);
    packet_limiter_init();

    int a, d, k;

    // ------------------------------------------------------------------
    printf("TEST 1: legitimate client at 60Hz movement is never throttled\n");
    packet_limiter_reset(10);
    int drops = 0;
    for (int tick = 0; tick < 120; tick++) {          // 2 seconds of play
        if (packet_limiter_check(10, PACKET_PLAYER_MOVE) != PACKET_LIMIT_ALLOW) drops++;
        usleep(16666);                                 // ~60Hz
    }
    printf("  drops over 120 ticks at 60Hz: %d (expect 0)\n", drops);
    assert(drops == 0);

    // ------------------------------------------------------------------
    printf("\nTEST 2: movement flood is capped at the burst capacity\n");
    packet_limiter_reset(11);
    count_verdicts(11, PACKET_PLAYER_MOVE, 1000, &a, &d, &k);
    // Past VIOLATION_LIMIT the verdict escalates from DROP to KICK, so the
    // rejected packets are split across both counters. In the real server the
    // first KICK closes the socket; here we keep calling to see the split.
    printf("  allowed=%d dropped=%d kicked=%d (expect allowed≈300, rest rejected)\n", a, d, k);
    assert(a >= 250 && a <= 320);
    assert(d + k > 600);
    assert(k > 0);   // a flood this large must escalate to a disconnect

    // ------------------------------------------------------------------
    printf("\nTEST 3: chat flood is capped much tighter than movement\n");
    packet_limiter_reset(12);
    count_verdicts(12, PACKET_CHAT_SEND, 200, &a, &d, &k);
    printf("  chat allowed=%d (expect ≈15, the social burst capacity)\n", a);
    assert(a >= 10 && a <= 20);

    // ------------------------------------------------------------------
    printf("\nTEST 4: budgets are independent per connection\n");
    packet_limiter_reset(13);
    packet_limiter_reset(14);
    count_verdicts(13, PACKET_CHAT_SEND, 200, &a, &d, &k);   // exhaust fd 13
    int a2, d2, k2;
    count_verdicts(14, PACKET_CHAT_SEND, 5, &a2, &d2, &k2);  // fd 14 untouched
    printf("  fd13 exhausted (allowed=%d), fd14 still allowed=%d/5\n", a, a2);
    assert(a2 == 5);

    // ------------------------------------------------------------------
    printf("\nTEST 5: separate classes draw on separate budgets\n");
    packet_limiter_reset(15);
    count_verdicts(15, PACKET_CHAT_SEND, 200, &a, &d, &k);   // drain social
    count_verdicts(15, PACKET_PLAYER_MOVE, 10, &a2, &d2, &k2);
    printf("  social drained (allowed=%d), movement still allowed=%d/10\n", a, a2);
    assert(a2 == 10);

    // ------------------------------------------------------------------
    printf("\nTEST 6: buckets refill over time\n");
    packet_limiter_reset(16);
    count_verdicts(16, PACKET_CHAT_SEND, 200, &a, &d, &k);   // drain
    sleep(2);                                                 // 5/s * 2s = 10 tokens
    count_verdicts(16, PACKET_CHAT_SEND, 20, &a2, &d2, &k2);
    printf("  after 2s idle, chat allowed=%d (expect ≈10)\n", a2);
    assert(a2 >= 7 && a2 <= 13);

    // ------------------------------------------------------------------
    printf("\nTEST 7: sustained abuse eventually demands a disconnect\n");
    packet_limiter_reset(17);
    count_verdicts(17, PACKET_CHAT_SEND, 5000, &a, &d, &k);
    printf("  allowed=%d dropped=%d kicked=%d (expect kicked>0)\n", a, d, k);
    assert(k > 0);

    // ------------------------------------------------------------------
    printf("\nTEST 8: unknown opcodes fall into the default budget, not unlimited\n");
    packet_limiter_reset(18);
    count_verdicts(18, 250, 500, &a, &d, &k);   // opcode 250 is not routed
    printf("  unknown-opcode allowed=%d of 500 (expect ≈40)\n", a);
    assert(a >= 30 && a <= 50);

    printf("\nALL ASSERTIONS PASSED\n");
    return 0;
}
