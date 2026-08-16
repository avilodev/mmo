#ifndef PACKET_LIMITER_H
#define PACKET_LIMITER_H

#include <stdint.h>

// ============================================================================
// packet_limiter.h — Per-connection inbound packet budgets, shared by all three
// servers.
//
// Every packet is charged against two buckets:
//
//   1. its class bucket  — movement, combat, item, social, query
//   2. the overall bucket — every packet from this connection, whatever its type
//
// The class bucket stops targeted abuse of one expensive opcode. The overall
// bucket stops an attacker spreading evenly across classes to stay under each
// individual limit while still flooding in aggregate. A packet is allowed only
// if both buckets can pay; neither is charged otherwise.
//
// There is no bucket per opcode. An opcode contributes two things: which class
// it draws from, and how many tokens it costs. Weighting by cost is what lets a
// database-backed request drain a bucket ten times faster than a ping without
// needing a class of its own — so adding opcodes stays free as the protocol
// grows.
//
// Enforcement is entirely server-side and unconditional. Nothing here depends
// on the client behaving, pacing itself, or being honest about anything.
//
// Threading: each connection is serviced start-to-finish by a single thread, so
// per-fd state needs no locking. This still holds under the world server's
// worker pool, because a worker runs one connection's handler inline until that
// connection closes rather than multiplexing several — the pool bounds how many
// threads exist, not which thread owns an fd.
//
// That invariant is load-bearing. If a server ever moves to epoll with workers
// picking up whichever socket is ready, every slot here becomes a data race and
// this module needs per-slot locking before that lands.
// ============================================================================

typedef enum {
    PACKET_LIMIT_ALLOW = 0,   // within budget, process normally
    PACKET_LIMIT_DROP  = 1,   // over budget, discard this packet
    PACKET_LIMIT_KICK  = 2    // sustained abuse, caller should close the socket
} PacketLimitVerdict;

typedef enum {
    LIMIT_CLASS_MOVEMENT = 0,   // movement and keepalives — highest volume
    LIMIT_CLASS_COMBAT,         // attacks and ability casts
    LIMIT_CLASS_ITEM,           // inventory, equipment, loot
    LIMIT_CLASS_SOCIAL,         // chat, party — cheapest to abuse, spammiest
    LIMIT_CLASS_QUERY,          // data requests, shops, dialogue, characters
    LIMIT_CLASS_COUNT
} PacketLimitClass;

typedef struct {
    double rate;       // tokens added per second (sustained rate)
    double capacity;   // bucket size (burst allowance)
} LimitBudget;

// One row per opcode.
//
// A cost of 0 does not mean "free" — it means "not specified", and is
// normalized at init to the fail-safe default below. That way a profile only
// lists the opcodes it cares about, and any opcode added to the protocol later
// is budgeted automatically instead of slipping through unmetered.
typedef struct {
    uint8_t cls;    // PacketLimitClass
    uint8_t cost;   // tokens charged to both the class and overall buckets
} LimitOpcodeRule;

#define LIMIT_DEFAULT_CLASS  LIMIT_CLASS_QUERY
#define LIMIT_DEFAULT_COST   1

typedef struct {
    const char*     name;                       // server name, for logs
    LimitBudget     overall;                    // charged for every packet
    LimitBudget     classes[LIMIT_CLASS_COUNT];
    LimitOpcodeRule opcodes[256];               // indexed directly by opcode

    // Per class: should a dropped packet be answered so the client's pending UI
    // can recover? Movement and combat are self-superseding — the next packet
    // replaces the dropped one, so silence is correct and cheaper. Request and
    // response traffic is not: dropping it silently leaves a spinner up forever.
    uint8_t notify_on_drop[LIMIT_CLASS_COUNT];

    uint32_t violation_limit;    // drops tolerated in one window before a kick
    double   violation_window;   // seconds
} PacketLimitProfile;

// Optional per-deployment overrides, typically read from a config file.
//
// Any field left at 0 keeps the profile's compiled-in value, so a config only
// has to state what it wants to change. That also means a rate of 0 cannot be
// configured — which is intentional: silently zeroing a bucket would block a
// class outright, and there are better ways to say that than a typo.
typedef struct {
    double   overall_rate;
    double   overall_capacity;
    double   class_rate[LIMIT_CLASS_COUNT];
    double   class_capacity[LIMIT_CLASS_COUNT];
    double   violation_window;
    uint32_t violation_limit;
} PacketLimitOverrides;

// Copy `base` into `out`, applying any non-zero override on top. `overrides`
// may be NULL, in which case this is a plain copy.
void packet_limiter_apply_overrides(PacketLimitProfile* out,
                                    const PacketLimitProfile* base,
                                    const PacketLimitOverrides* overrides);

// Map a class name ("movement", "combat", "item", "social", "query") to its
// index. Returns -1 if the name is not recognized.
int packet_limiter_class_from_name(const char* name);

// Install a profile and size the slot table from the process's file descriptor
// limit, so no live connection can ever fall outside it. Call once at startup,
// after any setrlimit and before accepting connections.
//
// The profile is copied, so the caller may pass a stack temporary.
void packet_limiter_init(const PacketLimitProfile* profile);

// Prepare (or reset) the budget for a connection. Call on connect and on
// disconnect so a recycled file descriptor never inherits an old budget.
void packet_limiter_reset(int fd);

// Account for one inbound packet and decide what to do with it.
PacketLimitVerdict packet_limiter_check(int fd, uint8_t packet_type);

// 1 if a dropped packet of this type should be answered with a rejection,
// 0 if dropping it silently is correct. See notify_on_drop above.
int packet_limiter_wants_rejection(uint8_t packet_type);

// Milliseconds until a packet of this type could be afforded again on this
// connection. Advisory only — it is a hint for the client's UI, never a promise
// and never something enforcement depends on.
uint16_t packet_limiter_retry_after_ms(int fd, uint8_t packet_type);

// Class an opcode is charged to. Exposed for logging and for callers that need
// to explain a rejection.
PacketLimitClass packet_limiter_class_of(uint8_t packet_type);
const char*      packet_limiter_class_name(PacketLimitClass cls);

#endif // PACKET_LIMITER_H
