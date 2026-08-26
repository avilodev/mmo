#ifndef SERVER_TYPES_H
#define SERVER_TYPES_H

/** @file Define server-only player and world runtime state. */

#include "protocol.h"
#include "item_instance.h"

#include <pthread.h>
#include <time.h>

/** Track one quest a character is currently working on.
 *
 * Declared here rather than in quest_system.h because ActivePlayer embeds the
 * log, and common/ cannot include a world_server/ header.
 */
struct PlayerQuestSlot {
    uint32_t quest_id;
    uint8_t  is_active;
    uint8_t  is_complete;   /**< Objectives met; waiting to be handed in. */
    uint8_t  _pad[2];
    int32_t  progress[MAX_QUEST_OBJECTIVES];
};

/** Hold the quests a character has in progress.
 *
 * Grows; there is no ceiling. The fixed array this replaced held 32 entries and
 * never released one, because a turned-in quest had to stay behind as the only
 * record that it had been done -- so 32 was a limit on quests taken in a
 * character's entire life, not on quests held at once. Splitting completion out
 * into QuestHistory is what makes this list purely work-in-progress, and what
 * lets it be unbounded without the memory growing without purpose.
 */
typedef struct {
    struct PlayerQuestSlot* slots;
    int                     count;
    int                     capacity;
} PlayerQuestLog;

/** Remember every quest a character has finished.
 *
 * An open-addressed set of quest identifiers, sized by how many quests the
 * character has actually completed rather than by how the world numbers them.
 *
 * A bitfield would be smaller per entry and was the obvious first idea, but it
 * has to be indexed by something. Indexed by quest id it punishes sparse
 * numbering -- one quest numbered a million costs every character 125 KB.
 * Indexed by a dense ordinal it silently corrupts the moment a quest is added
 * or removed and the ordinals shift under saved data. Identifiers are stable
 * and author-owned, so the set stores those.
 *
 * Quest id zero is never valid, which is what makes it usable as the
 * empty-bucket marker.
 */
typedef struct {
    uint32_t* ids;    /**< mask + 1 buckets; zero marks an empty one. */
    uint32_t  mask;   /**< Capacity minus one; capacity is a power of two. */
    int       count;
} QuestHistory;

/** Count how often one repeatable quest has been turned in, and when last.
 *
 * A separate, narrower table rather than a wider history entry, because the
 * two are paid for by very different populations. Most completed quests are
 * story quests: done once, count permanently one, window meaningless forever.
 * A character may finish two thousand of those and thirty repeatables, and
 * widening every history entry would pay the cost two thousand times to
 * describe thirty.
 *
 * A window *index* rather than a time_t is what keeps this at eight bytes with
 * no padding at all: a reset check only ever needs to know which period the
 * last turn-in fell in, never the wall-clock instant it happened.
 */
typedef struct {
    uint32_t quest_id;
    uint16_t count;    /**< Lifetime turn-ins; saturates rather than wrapping. */
    uint16_t window;   /**< Reset window index of the last turn-in. */
} QuestCounter;

_Static_assert(sizeof(QuestCounter) == 8,
               "QuestCounter is written to disk verbatim and must stay 8 bytes");

/** Hold one entry per repeatable quest a character has actually turned in.
 *
 * An open-addressed table keyed by quest identifier, sized the same way and for
 * the same reasons as QuestHistory. A character who only does story quests
 * never allocates one.
 *
 * Quest id zero is never valid, which is what makes it usable as the
 * empty-bucket marker.
 */
typedef struct {
    QuestCounter* entries;  /**< mask + 1 buckets; quest_id zero marks an empty one. */
    uint32_t      mask;     /**< Capacity minus one; capacity is a power of two. */
    int           count;
} QuestCounters;

/** Bundle every per-character quest table into one value.
 *
 * There is more than one table because they answer different questions and are
 * paid for by different populations: the log is work in progress, the history
 * is "did this ever happen", and the counters exist only for the handful of
 * quests that repeat. Passing them as one value is what keeps that free to grow -- a save,
 * a load, a snapshot and a release each take a PlayerQuestState, so adding a
 * table is a change inside quest_storage.c and nowhere else.
 */
typedef struct {
    PlayerQuestLog log;
    QuestHistory   history;
    QuestCounters  counters;
} PlayerQuestState;

/** Track capped movement credit accrued between packets. */
typedef struct {
    float           credit;  /**< Available movement distance in world pixels. */
    struct timespec last_tv; /**< Timestamp used to accrue additional credit. */
} MoveBudget;

/** Hold one active player's server-only mutable state. */
typedef struct {
    uint32_t character_id;
    uint32_t account_id;
    int client_fd;

    /** Fused race/class identifier; indexes the race registry loaded from races.json. */
    uint32_t race_id;

    char username[32];
    float pos_x, pos_y;
    float vel_x, vel_y;
    int level;
    int health;
    int max_health;
    /** Hold whichever pool the active spec's role selects; see resource_type. */
    int32_t resource;
    int32_t max_resource;
    uint8_t resource_type;  /**< ResourceType; RESOURCE_NONE while in Human Form. */
    uint64_t experience;

    /**
     * Balance per kingdom, indexed by CurrencyId.
     *
     * There is no universal coin: each kingdom mints its own, and a player
     * holds a separate balance in each. Sized from the shared city table, so
     * adding a kingdom needs no change here.
     */
    uint32_t currency[CURRENCY_COUNT];

    /** Preserve item identity when moving stacks between bag and equipment. */
    ItemInstance inventory[INVENTORY_SLOTS];
    ItemInstance equipment[EQUIP_SLOTS];

    double last_attack_time;
    float attack_cooldown;

    int is_loaded;
    int is_reserved; /**< Marks a slot claimed by an in-progress database load. */
    int is_ready;
    int is_dirty;
    time_t last_save;
    time_t last_activity;
    MoveBudget move_budget;

    uint8_t current_zone_id;

    /** Hold one hotbar per form. Swapping forms swaps which row is live. */
    uint16_t ability_slots[FORM_COUNT][MAX_ABILITY_SLOTS];
    uint8_t  ability_count[FORM_COUNT];
    /** Store an absolute expiry instant on the monotonic clock, not a remaining time.
     *
     * Cooldowns therefore keep ticking for the form the player is not in, and no
     * swap sequence can shorten one. A slot is ready when now >= its entry. */
    double   ability_ready_at[FORM_COUNT][MAX_ABILITY_SLOTS];
    uint8_t  form;              /**< PlayerForm currently active. */
    double   form_swap_ready_at; /**< Absolute expiry of the shared swap cooldown. */

    /** Hold the eleven attributes indexed by StatId, after gear and buffs. */
    int stats[STAT_COUNT];
    /** Hold the same attributes before gear and buffs, straight from the race curve. */
    int base_stats[STAT_COUNT];
    float move_speed;
    int weapon_damage;
    double last_combat_time;

    uint8_t is_dead;
    double death_time;

/** Bound concurrent status effects stored for one active player. */
#define MAX_ACTIVE_EFFECTS 8
    /** Track one active status effect and its remaining timers. */
    struct {
        uint8_t active;
        uint8_t effect_type;
        uint8_t buff_stat;
        uint8_t _pad;
        int value;
        float duration_remaining;
        float tick_remaining;
        float tick_rate;
        uint32_t source_id;
    } active_effects[MAX_ACTIVE_EFFECTS];

    double last_consumable_time;
    uint32_t party_id;
    uint16_t ping_ms;

    /** Quests in progress, and every quest ever finished.
     *
     * Owns heap storage. player_slot_clear() releases it before recycling the
     * slot, which is the only reason the whole-struct memset there is safe.
     */
    PlayerQuestState quests;

    pthread_mutex_t lock;
} ActivePlayer;

/** Track one configured world process from the realm server. */
typedef struct {
    char name[64];
    /** Display region, straight from the world table.
     *
     * The realm used to parse a region heading out of worlds.txt, keep it in a
     * local, and then send every world to the client labelled "Unknown"
     * because the parsed value had nowhere to live. It lives here. */
    char region[64];
    char host[64];
    uint16_t port;        /**< Port clients connect to; published in the world list. */
    uint16_t realm_port;  /**< Port the realm's own heartbeat link connects to. */
    int fd;
    /** The TLS session over `fd`, or NULL when this world is not connected.
     *
     * Forward-declared rather than SSL*, so every translation unit that touches
     * a WorldServer does not have to pull in OpenSSL. Only main.c and
     * world_connect.c ever dereference it, and both include tls.h.
     *
     * Owned with the descriptor: whoever closes one closes the other.
     */
    struct ssl_st* tls;
    uint8_t online;
    int connection_logged;
    uint32_t player_count;
    uint32_t max_players;
    time_t last_heartbeat;
} WorldServer;

#endif
