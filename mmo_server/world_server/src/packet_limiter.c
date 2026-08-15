// ============================================================================
// packet_limiter.c — Token-bucket packet budgets, one set per connection.
//
// Each opcode class gets a bucket that refills at a steady rate up to a burst
// capacity. A packet costs one token. Empty bucket means the packet is dropped
// and a violation is recorded; enough violations inside one window and the
// caller is told to close the connection.
//
// Slots are indexed directly by file descriptor so the hot path is O(1). File
// descriptors above the table size fall back to "always allow" rather than
// sharing a slot, because sharing would let one connection spend another's
// budget.
// ============================================================================

#include "packet_limiter.h"

#include "log.h"
#include "protocol.h"

#include <string.h>
#include <time.h>

#define LIMITER_SLOTS 4096

// Violations tolerated inside one window before the connection is closed.
#define VIOLATION_LIMIT   200
#define VIOLATION_WINDOW  10.0   // seconds

typedef enum {
    CLASS_MOVEMENT = 0,   // movement and keepalives — highest volume
    CLASS_COMBAT,         // attacks and ability casts
    CLASS_ITEM,           // inventory, equipment, loot
    CLASS_SOCIAL,         // chat, party, whisper — cheapest to abuse, spammiest
    CLASS_QUERY,          // data/stat/session requests, shops, dialogue
    CLASS_COUNT
} PacketClass;

typedef struct {
    double rate;       // tokens added per second (sustained rate)
    double capacity;   // bucket size (burst allowance)
} ClassBudget;

// A well-behaved client sends movement at ~60Hz; 150/s sustained leaves ample
// headroom for lag-driven bursts while still capping a flood at a small
// multiple of legitimate traffic.
static const ClassBudget CLASS_BUDGETS[CLASS_COUNT] = {
    [CLASS_MOVEMENT] = { .rate = 150.0, .capacity = 300.0 },
    [CLASS_COMBAT]   = { .rate =  30.0, .capacity =  60.0 },
    [CLASS_ITEM]     = { .rate =  20.0, .capacity =  40.0 },
    [CLASS_SOCIAL]   = { .rate =   5.0, .capacity =  15.0 },
    [CLASS_QUERY]    = { .rate =  20.0, .capacity =  40.0 },
};

typedef struct {
    double tokens[CLASS_COUNT];
    double last_refill;          // CLOCK_MONOTONIC seconds
    double violation_window;     // start of the current violation window
    int    violations;
    int    in_use;
} LimiterSlot;

static LimiterSlot g_slots[LIMITER_SLOTS];

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static PacketClass classify(uint8_t type) {
    switch (type) {
        case PACKET_PLAYER_MOVE:
        case PACKET_PING:
            return CLASS_MOVEMENT;

        case PACKET_ATTACK_INTENT:
        case PACKET_CAST_CANCEL:
        case PACKET_ABILITY_CAST_INTENT:
        case PACKET_ABILITY_CAST_CANCEL:
            return CLASS_COMBAT;

        case PACKET_EQUIP_ITEM:
        case PACKET_UNEQUIP_ITEM:
        case PACKET_USE_ITEM:
        case PACKET_DROP_ITEM:
        case PACKET_MOVE_ITEM:
        case PACKET_LOOT_PICKUP_REQUEST:
            return CLASS_ITEM;

        case PACKET_CHAT_SEND:
        case PACKET_PARTY_INVITE:
        case PACKET_PARTY_ACCEPT:
        case PACKET_PARTY_DECLINE:
        case PACKET_PARTY_LEAVE:
        case PACKET_PARTY_KICK:
            return CLASS_SOCIAL;

        default:
            // Requests, shop traffic, dialogue, and any opcode added later.
            return CLASS_QUERY;
    }
}

void packet_limiter_init(void) {
    memset(g_slots, 0, sizeof(g_slots));
}

void packet_limiter_reset(int fd) {
    if (fd < 0 || fd >= LIMITER_SLOTS) return;

    LimiterSlot* slot = &g_slots[fd];
    memset(slot, 0, sizeof(*slot));

    double now = now_seconds();
    slot->last_refill      = now;
    slot->violation_window = now;
    slot->in_use           = 1;

    // Start each connection with a full burst allowance so a legitimate client
    // can send its initial flurry of requests on entering the world.
    for (int i = 0; i < CLASS_COUNT; i++)
        slot->tokens[i] = CLASS_BUDGETS[i].capacity;
}

PacketLimitVerdict packet_limiter_check(int fd, uint8_t packet_type) {
    // Descriptors beyond the table are not budgeted rather than sharing a slot.
    if (fd < 0 || fd >= LIMITER_SLOTS) return PACKET_LIMIT_ALLOW;

    LimiterSlot* slot = &g_slots[fd];
    if (!slot->in_use) packet_limiter_reset(fd);

    double now = now_seconds();
    double elapsed = now - slot->last_refill;
    if (elapsed < 0.0) elapsed = 0.0;   // guard against clock oddities
    slot->last_refill = now;

    // Refill every bucket for the time that has passed.
    for (int i = 0; i < CLASS_COUNT; i++) {
        double refilled = slot->tokens[i] + CLASS_BUDGETS[i].rate * elapsed;
        slot->tokens[i] = refilled > CLASS_BUDGETS[i].capacity
                        ? CLASS_BUDGETS[i].capacity
                        : refilled;
    }

    // Roll the violation window over once it expires.
    if (now - slot->violation_window >= VIOLATION_WINDOW) {
        slot->violation_window = now;
        slot->violations = 0;
    }

    PacketClass cls = classify(packet_type);

    if (slot->tokens[cls] >= 1.0) {
        slot->tokens[cls] -= 1.0;
        return PACKET_LIMIT_ALLOW;
    }

    slot->violations++;
    if (slot->violations >= VIOLATION_LIMIT) {
        LOG_WARN_RL(5, 60,
                    "[LIMIT] fd=%d exceeded packet budget %d times in %.0fs "
                    "(class=%d, last opcode=%u) — closing connection",
                    fd, slot->violations, VIOLATION_WINDOW, (int)cls, packet_type);
        return PACKET_LIMIT_KICK;
    }

    LOG_WARN_RL(5, 60, "[LIMIT] fd=%d over budget for class=%d (opcode=%u), dropping packet",
                fd, (int)cls, packet_type);
    return PACKET_LIMIT_DROP;
}
