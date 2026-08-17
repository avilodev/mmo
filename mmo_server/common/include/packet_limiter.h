#ifndef PACKET_LIMITER_H
#define PACKET_LIMITER_H

#include <stdint.h>

/** @file Enforce per-connection class and overall token-bucket limits.
 * Each file descriptor must remain owned by one thread for its connection lifetime.
 */

/** Select the action taken after accounting for an inbound packet. */
typedef enum {
    PACKET_LIMIT_ALLOW = 0,   // within budget, process normally
    PACKET_LIMIT_DROP  = 1,   // over budget, discard this packet
    PACKET_LIMIT_KICK  = 2    // sustained abuse, caller should close the socket
} PacketLimitVerdict;

/** Group packet opcodes by traffic and processing cost. */
typedef enum {
    LIMIT_CLASS_MOVEMENT = 0,   // movement and keepalives — highest volume
    LIMIT_CLASS_COMBAT,         // attacks and ability casts
    LIMIT_CLASS_ITEM,           // inventory, equipment, loot
    LIMIT_CLASS_SOCIAL,         // chat, party — cheapest to abuse, spammiest
    LIMIT_CLASS_QUERY,          // data requests, shops, dialogue, characters
    LIMIT_CLASS_COUNT
} PacketLimitClass;

/** Configure a token bucket's sustained rate and burst allowance. */
typedef struct {
    double rate;       /**< Tokens replenished per second. */
    double capacity;   /**< Maximum accumulated tokens. */
} LimitBudget;

/** Assign one opcode to a limit class and token cost. */
typedef struct {
    uint8_t cls;    /**< PacketLimitClass value. */
    uint8_t cost;   /**< Tokens charged to class and overall buckets; zero selects the default. */
} LimitOpcodeRule;

/** Supply fail-safe accounting for opcodes omitted from a profile. */
#define LIMIT_DEFAULT_CLASS  LIMIT_CLASS_QUERY
#define LIMIT_DEFAULT_COST   1

/** Configure all token buckets, opcode rules, and abuse thresholds for one server. */
typedef struct {
    const char*     name;                       /**< Server name used in logs. */
    LimitBudget     overall;                    /**< Bucket charged for every packet. */
    LimitBudget     classes[LIMIT_CLASS_COUNT];
    LimitOpcodeRule opcodes[256];               // indexed directly by opcode

    /** Select classes whose dropped requests receive rejection packets. */
    uint8_t notify_on_drop[LIMIT_CLASS_COUNT];

    uint32_t violation_limit;    /**< Drops tolerated per violation window. */
    double   violation_window;   /**< Violation window duration in seconds. */
} PacketLimitProfile;

/** Override selected profile fields, using zero to retain compiled defaults. */
typedef struct {
    double   overall_rate;
    double   overall_capacity;
    double   class_rate[LIMIT_CLASS_COUNT];
    double   class_capacity[LIMIT_CLASS_COUNT];
    double   violation_window;
    uint32_t violation_limit;
} PacketLimitOverrides;

// accept NULL overrides to copy the base profile unchanged
void packet_limiter_apply_overrides(PacketLimitProfile* out,
                                    const PacketLimitProfile* base,
                                    const PacketLimitOverrides* overrides);

// return -1 for an unrecognized class name
int packet_limiter_class_from_name(const char* name);

// install a copied profile before accepting connections
void packet_limiter_init(const PacketLimitProfile* profile);

// reset on connection open and close to handle descriptor reuse
void packet_limiter_reset(int fd);

PacketLimitVerdict packet_limiter_check(int fd, uint8_t packet_type);

// return whether a dropped opcode requires a rejection packet
int packet_limiter_wants_rejection(uint8_t packet_type);

// return an advisory retry delay in milliseconds
uint16_t packet_limiter_retry_after_ms(int fd, uint8_t packet_type);

// expose opcode classes for logging and rejection messages
PacketLimitClass packet_limiter_class_of(uint8_t packet_type);
const char*      packet_limiter_class_name(PacketLimitClass cls);

#endif // PACKET_LIMITER_H
