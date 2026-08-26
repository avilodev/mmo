/**
 * @file
 * Enforce per-connection token-bucket budgets for inbound server packets.
 */

#include "packet_limiter.h"

#include "log.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

/** Hold lazily refilled class and overall budgets for one descriptor. */
typedef struct {
    double   tokens[LIMIT_CLASS_COUNT];
    double   last_refill[LIMIT_CLASS_COUNT];
    double   overall_tokens;
    double   overall_last_refill;
    double   violation_window;   // start of the current violation window
    uint32_t violations;
    int      in_use;
} LimiterSlot;

static PacketLimitProfile g_profile;
static LimiterSlot*       g_slots      = NULL;
static int                g_slot_count = 0;

/** Cap descriptor-indexed limiter storage independently of RLIMIT_NOFILE. */
#define LIMITER_MAX_SLOTS   65536
#define LIMITER_MIN_SLOTS   1024

static const char* const CLASS_NAMES[LIMIT_CLASS_COUNT] = {
    "movement", "combat", "item", "social", "query"
};

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/** Refill one bucket for elapsed monotonic time without exceeding its capacity. */
static void refill(double* tokens, double* last_refill,
                   const LimitBudget* budget, double now) {
    double elapsed = now - *last_refill;
    if (elapsed < 0.0) elapsed = 0.0;
    *last_refill = now;

    double filled = *tokens + budget->rate * elapsed;
    *tokens = filled > budget->capacity ? budget->capacity : filled;
}

/**
 * Map a configured class name to its limiter index.
 *
 * @return      The class index, or -1 when the name is absent or unknown.
 */
int packet_limiter_class_from_name(const char* name) {
    if (!name) return -1;
    for (int i = 0; i < LIMIT_CLASS_COUNT; i++)
        if (strcmp(name, CLASS_NAMES[i]) == 0) return i;
    return -1;
}

/** Apply nonzero deployment overrides to a copied packet-limit profile. */
void packet_limiter_apply_overrides(PacketLimitProfile* out,
                                    const PacketLimitProfile* base,
                                    const PacketLimitOverrides* overrides) {
    if (!out || !base) return;

    *out = *base;
    if (!overrides) return;

    if (overrides->overall_rate     > 0.0) out->overall.rate     = overrides->overall_rate;
    if (overrides->overall_capacity > 0.0) out->overall.capacity = overrides->overall_capacity;

    for (int i = 0; i < LIMIT_CLASS_COUNT; i++) {
        if (overrides->class_rate[i]     > 0.0) out->classes[i].rate     = overrides->class_rate[i];
        if (overrides->class_capacity[i] > 0.0) out->classes[i].capacity = overrides->class_capacity[i];
    }

    if (overrides->violation_window > 0.0) out->violation_window = overrides->violation_window;
    if (overrides->violation_limit  > 0)   out->violation_limit  = overrides->violation_limit;

    // A capacity below the sustained rate would let a bucket refill past what
    // it can hold, turning the burst allowance into a cap tighter than the rate
    // it advertises. Cheaper to catch here than to debug as mysterious drops.
    for (int i = 0; i < LIMIT_CLASS_COUNT; i++) {
        if (out->classes[i].capacity < out->classes[i].rate) {
            LOG_WARN("[LIMIT] %s burst (%.1f) is below its rate (%.1f) — "
                     "raising burst to match",
                     CLASS_NAMES[i], out->classes[i].capacity, out->classes[i].rate);
            out->classes[i].capacity = out->classes[i].rate;
        }
    }
    if (out->overall.capacity < out->overall.rate) {
        LOG_WARN("[LIMIT] overall burst (%.1f) is below its rate (%.1f) — "
                 "raising burst to match",
                 out->overall.capacity, out->overall.rate);
        out->overall.capacity = out->overall.rate;
    }
}

/**
 * Install a packet-limit profile and allocate descriptor-indexed limiter slots.
 *
 * Unspecified opcode rules receive the fail-safe default class and cost.
 */
void packet_limiter_init(const PacketLimitProfile* profile) {
    if (!profile) return;

    g_profile = *profile;

    // Normalize the opcode table. An unspecified row (cost 0) becomes the
    // fail-safe default, so opcodes added to the protocol later are budgeted
    // without anyone having to remember to come back here.
    for (int i = 0; i < 256; i++) {
        if (g_profile.opcodes[i].cost == 0) {
            g_profile.opcodes[i].cls  = LIMIT_DEFAULT_CLASS;
            g_profile.opcodes[i].cost = LIMIT_DEFAULT_COST;
        } else if (g_profile.opcodes[i].cls >= LIMIT_CLASS_COUNT) {
            LOG_WARN("[LIMIT] opcode %d has invalid class %u, using default",
                     i, g_profile.opcodes[i].cls);
            g_profile.opcodes[i].cls = LIMIT_DEFAULT_CLASS;
        }
    }

    // Size the table from the descriptor limit so no live fd falls outside it.
    struct rlimit rl;
    long wanted = LIMITER_MIN_SLOTS;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY)
        wanted = (long)rl.rlim_cur;
    else
        wanted = LIMITER_MAX_SLOTS;

    if (wanted < LIMITER_MIN_SLOTS) wanted = LIMITER_MIN_SLOTS;
    if (wanted > LIMITER_MAX_SLOTS) wanted = LIMITER_MAX_SLOTS;

    free(g_slots);
    g_slots = calloc((size_t)wanted, sizeof(LimiterSlot));
    if (!g_slots) {
        LOG_ERROR("[LIMIT] could not allocate %ld slots — limiter disabled, "
                  "refusing to run unprotected", wanted);
        g_slot_count = 0;
        return;
    }
    g_slot_count = (int)wanted;

    LOG_INFO("[LIMIT] %s profile active: %d slots, overall %.0f/s (burst %.0f), "
             "kick at %u drops in %.0fs",
             g_profile.name ? g_profile.name : "unnamed", g_slot_count,
             g_profile.overall.rate, g_profile.overall.capacity,
             g_profile.violation_limit, g_profile.violation_window);
}

/** Reset a descriptor's counters with full initial burst allowances. */
void packet_limiter_reset(int fd) {
    if (!g_slots || fd < 0 || fd >= g_slot_count) return;

    LimiterSlot* slot = &g_slots[fd];
    memset(slot, 0, sizeof(*slot));

    double now = now_seconds();
    slot->violation_window    = now;
    slot->overall_last_refill = now;
    slot->in_use              = 1;

    // Start each connection with a full burst allowance so a legitimate client
    // can send its initial flurry of requests on entering the world.
    slot->overall_tokens = g_profile.overall.capacity;
    for (int i = 0; i < LIMIT_CLASS_COUNT; i++) {
        slot->tokens[i]      = g_profile.classes[i].capacity;
        slot->last_refill[i] = now;
    }
}

/* --- What the limiter has actually done ----------------------------------
 *
 * The limiter's only output was a rate-limited log line, which means the
 * question "is this world dropping packets right now, and how many" could be
 * answered by reading text and by nothing else -- and the log line is
 * deliberately rate-limited, so the text does not even carry the count.
 *
 * Three counters, monotonic since startup, relaxed because nothing branches on
 * them and a scrape that is one packet stale is a scrape that is one packet
 * stale. They are read by each service's /metrics provider and by the world's
 * periodic [STATS] line.
 */
static atomic_ullong g_allowed_total = 0;
static atomic_ullong g_dropped_total = 0;
static atomic_ullong g_kicked_total  = 0;

/** Read the running totals. Any pointer may be NULL. */
void packet_limiter_totals(unsigned long long* allowed,
                           unsigned long long* dropped,
                           unsigned long long* kicked) {
    if (allowed) *allowed = atomic_load_explicit(&g_allowed_total, memory_order_relaxed);
    if (dropped) *dropped = atomic_load_explicit(&g_dropped_total, memory_order_relaxed);
    if (kicked)  *kicked  = atomic_load_explicit(&g_kicked_total,  memory_order_relaxed);
}

/** Reset the running totals. For tests, which need a known starting point. */
void packet_limiter_reset_totals(void) {
    atomic_store_explicit(&g_allowed_total, 0, memory_order_relaxed);
    atomic_store_explicit(&g_dropped_total, 0, memory_order_relaxed);
    atomic_store_explicit(&g_kicked_total,  0, memory_order_relaxed);
}

/** Count one verdict and hand it back, so no return path can forget to. */
static PacketLimitVerdict counted(PacketLimitVerdict verdict) {
    switch (verdict) {
        case PACKET_LIMIT_ALLOW:
            atomic_fetch_add_explicit(&g_allowed_total, 1, memory_order_relaxed); break;
        case PACKET_LIMIT_DROP:
            atomic_fetch_add_explicit(&g_dropped_total, 1, memory_order_relaxed); break;
        case PACKET_LIMIT_KICK:
            atomic_fetch_add_explicit(&g_kicked_total, 1, memory_order_relaxed); break;
    }
    return verdict;
}

/**
 * Charge an inbound packet to its class and overall token buckets.
 *
 * @return      Allow, drop, or disconnect according to available tokens and recent violations.
 */
PacketLimitVerdict packet_limiter_check(int fd, uint8_t packet_type) {
    // No table means init failed. Refuse rather than serve unmetered traffic.
    if (!g_slots) return counted(PACKET_LIMIT_KICK);

    if (fd < 0 || fd >= g_slot_count) {
        // The table is sized from the descriptor limit, so this cannot happen
        // for a live connection. If it does, the invariant is broken and the
        // safe reading is that this connection is unbudgeted — close it.
        LOG_WARN_RL(5, 60,
                    "[LIMIT] fd=%d outside slot table (%d) — closing connection",
                    fd, g_slot_count);
        return counted(PACKET_LIMIT_KICK);
    }

    LimiterSlot* slot = &g_slots[fd];
    if (!slot->in_use) packet_limiter_reset(fd);

    const LimitOpcodeRule rule = g_profile.opcodes[packet_type];
    const int    cls  = rule.cls;
    const double cost = (double)rule.cost;

    double now = now_seconds();

    // Only the two buckets this packet touches are brought up to date.
    refill(&slot->overall_tokens, &slot->overall_last_refill,
           &g_profile.overall, now);
    refill(&slot->tokens[cls], &slot->last_refill[cls],
           &g_profile.classes[cls], now);

    // Roll the violation window over once it expires.
    if (now - slot->violation_window >= g_profile.violation_window) {
        slot->violation_window = now;
        slot->violations = 0;
    }

    // Charge only if both buckets can pay, so a packet that gets dropped never
    // drains the bucket that could have afforded it.
    if (slot->overall_tokens >= cost && slot->tokens[cls] >= cost) {
        slot->overall_tokens -= cost;
        slot->tokens[cls]    -= cost;
        return counted(PACKET_LIMIT_ALLOW);
    }

    slot->violations++;
    if (slot->violations >= g_profile.violation_limit) {
        LOG_WARN_RL(5, 60,
                    "[LIMIT] fd=%d exceeded packet budget %u times in %.0fs "
                    "(class=%s, last opcode=%u) — closing connection",
                    fd, slot->violations, g_profile.violation_window,
                    CLASS_NAMES[cls], packet_type);
        return counted(PACKET_LIMIT_KICK);
    }

    LOG_WARN_RL(5, 60,
                "[LIMIT] fd=%d over budget for class=%s (opcode=%u, cost=%u), "
                "dropping packet",
                fd, CLASS_NAMES[cls], packet_type, rule.cost);
    return counted(PACKET_LIMIT_DROP);
}

/**
 * Estimate when both buckets could next afford a packet type.
 *
 * @return      The advisory delay in milliseconds, clamped to 65535, or zero for an unavailable slot.
 */
uint16_t packet_limiter_retry_after_ms(int fd, uint8_t packet_type) {
    if (!g_slots || fd < 0 || fd >= g_slot_count) return 0;

    const LimiterSlot*    slot = &g_slots[fd];
    const LimitOpcodeRule rule = g_profile.opcodes[packet_type];
    const double          cost = (double)rule.cost;

    // The caller waits on whichever of the two buckets recovers last.
    double wait = 0.0;

    double deficit = cost - slot->overall_tokens;
    if (deficit > 0.0 && g_profile.overall.rate > 0.0)
        wait = deficit / g_profile.overall.rate;

    deficit = cost - slot->tokens[rule.cls];
    if (deficit > 0.0 && g_profile.classes[rule.cls].rate > 0.0) {
        double class_wait = deficit / g_profile.classes[rule.cls].rate;
        if (class_wait > wait) wait = class_wait;
    }

    double ms = wait * 1000.0;
    if (ms < 0.0)       ms = 0.0;
    if (ms > 65535.0)   ms = 65535.0;
    return (uint16_t)ms;
}

/**
 * Report whether a dropped packet's class requires an explicit rejection.
 *
 * @return      Nonzero when the caller should send a rejection, otherwise zero.
 */
int packet_limiter_wants_rejection(uint8_t packet_type) {
    if (!g_slots) return 0;
    return g_profile.notify_on_drop[g_profile.opcodes[packet_type].cls] != 0;
}

/** Return the limiter class assigned to an opcode. */
PacketLimitClass packet_limiter_class_of(uint8_t packet_type) {
    if (!g_slots) return LIMIT_DEFAULT_CLASS;
    return (PacketLimitClass)g_profile.opcodes[packet_type].cls;
}

/**
 * Return the display name for a limiter class.
 *
 * @return      The class name, or "unknown" for an invalid class.
 */
const char* packet_limiter_class_name(PacketLimitClass cls) {
    if (cls < 0 || cls >= LIMIT_CLASS_COUNT) return "unknown";
    return CLASS_NAMES[cls];
}
