/** @file Define authoritative attack profiles, NPC state, and pending casts. */

#ifndef COMBAT_CONFIG_H
#define COMBAT_CONFIG_H

#include "protocol.h"

#include <stdint.h>
#include <pthread.h>

/** Define authoritative timing, geometry, and damage for one race's basic attack.
 *
 * One profile per race, loaded from attack_profiles.json. Nothing here knows how many
 * races exist: the array is sized by the MAX_RACES bound and indexed by the same
 * fused identifier everything else uses.
 */
typedef struct {
    float       cast_time;          // Seconds from intent to damage resolution
    float       cooldown;           // Seconds after damage before next attack can start
    float       range;              // Max distance (world units) for target resolution
    int         base_damage;        // Flat damage before variance
    int         damage_variance;    // ±% applied randomly at resolution time
    uint8_t     attack_type;        // AttackType: SINGLE / AOE / CONE / LINE
    float       cone_half_angle;    // Degrees — only used when attack_type == CONE
    float       line_width;         // World units — only used when attack_type == LINE
    /** Configure projectile resolution for ranged class attacks. */
    uint8_t     is_ranged;          // 1 = spawn projectile at cast resolution instead of instant damage
    float       projectile_speed;   // World units per second
    float       projectile_width;   // Hitbox width of the projectile
    uint8_t     projectile_damage_stat;  // StatId that scales projectile damage
    uint8_t     projectile_damage_type;  // AbilityDamageType (0=phys, 1=earth, 2=spirit)
    /** Which attribute scales this attack's damage. */
    uint8_t     damage_stat;             // StatId
    uint8_t     is_loaded;               /**< Nonzero once a profile has been read for this race. */
} RaceAttackProfile;

/** Index attack profiles by race identifier, leaving index zero unused. */
extern RaceAttackProfile g_race_profiles[MAX_RACES + 1];

/** Identify NPC disposition used by AI and client presentation. */
typedef enum {
    NPC_CATEGORY_PASSIVE    = 0,   // Village NPCs, vendors — never attacks
    NPC_CATEGORY_HOSTILE    = 1,   // Enemies — will aggro and attack
    NPC_CATEGORY_QUEST      = 2,   // Quest givers — interactable, special marker
} NPCCategory;

/** Hold authoritative combat, spawn, dialogue, and AI state for one NPC. */
typedef struct {
    uint32_t    id;                // Unique entity ID (server-assigned)
    char        name[32];
    float       pos_x, pos_y;      // Current world position
    int         health;
    int         max_health;
    float       hitbox_radius;     // For collision / hit detection

    /** Flat damage reduction, matching the Armor attribute players carry.
     *
     * There is no evasion counterpart: the Blessed model drops dodge entirely, so an
     * attack that reaches an NPC always connects. */
    int         armor;
    uint32_t    xp_reward;         // XP granted to killer

    /* No coin reward: enemies drop items, which the killer sells to a
     * kingdom's NPCs for that kingdom's currency. See loot.h. */

    uint8_t     is_alive;          // 0 = dead, 1 = alive
    uint8_t     category;          // NPCCategory — passive/hostile/quest

    /** Retain spawn state used for return and respawn behavior. */
    float       spawn_x, spawn_y;  // Original spawn position
    float       respawn_time;      // Seconds until respawn (0 = no respawn)
    double      death_time;        // When the NPC died (CLOCK_MONOTONIC)
    uint16_t    npc_type_id;       // For loot table lookup

    /** Associate optional local dialogue data with an interactable NPC. */
    uint32_t    dialogue_id;       // 0 = no dialogue, otherwise dialogue ID from JSON
    uint8_t     is_interactable;   // 1 if player can talk to this NPC

    /** Track mutable AI target and cooldown state. */
    uint8_t     ai_state;          // NPCAIState: 0=idle, 1=aggro, 2=returning, 3=casting
    uint32_t    ai_target_id;      // Current target character_id (0 = no target)
    /** Force this NPC's target while a taunt holds.
     *
     * Roar is the tank's whole job in one ability, so the taunt has to override
     * target selection rather than merely nudge it. It is an absolute expiry on the
     * monotonic clock, for the same reason ability cooldowns are. */
    uint32_t    taunt_source_id;
    double      taunt_expires_at;
    /* Ability cooldowns used to live here, as an inline array sized by a second
     * #define that a comment asked you to keep equal to npc_ai.h's. They are a
     * per-slot array on the pool now -- npc_world_cooldowns() -- sized once from
     * the loaded content. See npc_world.h. */
    uint8_t     ai_cd_seeded;      // 1 once per-enemy cooldown phases have been randomized

    /** Preserve geometry while an NPC telegraph cast is active. */
    uint8_t     ai_is_casting;     // 1 = currently casting a telegraph
    int         ai_cast_ability_idx; // Which ability slot is being cast
    double      ai_cast_start;     // When the cast began (CLOCK_MONOTONIC)
    double      ai_cast_duration;  /**< Snapshotted at cast start; randomised casts vary it. */
    float       ai_cast_pos_x;     // Telegraph center position
    float       ai_cast_pos_y;
    float       ai_cast_dir_x;     // Telegraph direction (for cone/rect/line)
    float       ai_cast_dir_y;
    float       ai_cast_reach;     /**< Radius or length actually chosen; random_range varies it. */
    uint8_t     ai_cast_resolves;  /**< 0 for a feint, which broadcasts and deals nothing. */

    /* --- Behaviour state the sixteen verbs added ------------------------
     *
     * All of it scalars, deliberately. NPCEntity is memset and copied in
     * several places and the tick snapshot copies these fields wholesale, so
     * anything variable-length belongs in the pool's parallel arrays instead
     * (npc_world.h). Nothing below is variable-length. */

    /** Where this NPC is looking. Movement and casts write it; frontal block
     * and every arc test read it. Zero-length means "not yet facing", which
     * every reader treats as an unconditional hit rather than a divide by zero. */
    float       facing_x, facing_y;

    /** Multi-shot in progress (V2). A burst spans ticks rather than looping,
     * so the shots are spread across the wire the way a player sees them. */
    int         burst_ability_idx;
    int         burst_remaining;
    int         burst_index;        /**< Which shot of the volley is next, from 0. */
    double      burst_next_time;
    float       burst_dir_x, burst_dir_y;
    uint32_t    burst_target_id;

    /** Travelling displacement in progress (V8). */
    uint8_t     dash_active;
    float       dash_dir_x, dash_dir_y;
    float       dash_remaining;     /**< World units still to travel. */
    float       dash_speed;
    int         dash_damage;        /**< 0 when the dash does not damage on contact. */
    uint16_t    dash_ability_id;
    uint8_t     dash_stagger;       /**< Wagon Breaker: hitting terrain staggers the NPC. */

    /** Accumulated percentage modifiers from fired triggers and this NPC's affix.
     *
     * Accumulated rather than recomputed because a `repeating` timer trigger
     * stacks -- Escalation adds five percent every ten seconds, forever. A
     * latch-derived recomputation cannot express that. Cleared with the rest of
     * the slot's state on spawn and on respawn. */
    float       mod_move_speed_pct;
    float       mod_attack_speed_pct;
    float       mod_damage_pct;
    float       mod_damage_taken_pct;
    float       mod_cooldown_pct;
    float       mod_phase_duration_pct;
    /** Cast-time scaling, which affixes set and no trigger does. Kept apart from
     * attack speed because they are different quantities: the Duelist telegraphs
     * for 40% longer *and* hits 20% harder, which is legible only if the two
     * numbers stay separate. */
    float       mod_cast_time_pct;

    /** When this NPC's trigger clock started, for `timer` triggers. */
    double      trigger_epoch;
    /** Next firing instant for the repeating timer trigger, absolute. */
    double      trigger_timer_next;

    /** Event counters, incremented wherever the event happens and consumed by
     * the trigger evaluator on the gameplay thread.
     *
     * A counter rather than a flag: two hits between two AI ticks are two
     * events, and a flag would lose one. A counter rather than a queue because
     * every consumer only asks "did this happen since I last looked". */
    uint32_t    damage_events, damage_events_seen;
    uint32_t    kill_events, kill_events_seen;
    uint32_t    miss_events, miss_events_seen;
    float       last_damage_x, last_damage_y;   /**< Where the last hit came from. */

    /** Fleeing until this instant (V16 `flee`). Movement inverts; abilities hold. */
    double      flee_until;

    /** Recovery window after an ability resolves. Nothing may be cast until it
     * passes, which is what makes a heavy swing punishable. */
    double      recover_until;

    /** Summon parentage (V7). */
    uint32_t    parent_npc_id;      /**< 0 when this NPC was placed by a spawn table. */
    uint16_t    summon_children;    /**< Live summons this NPC owns, for its budget. */
    uint8_t     no_reward;          /**< Summon-only: grants no XP and rolls no loot. */

    /** Stealth (V10). A stealthed NPC is omitted from the broadcast entirely. */
    uint8_t     stealth_active;
    double      stealth_revealed_until;

    /** Mitigation (V9). A shield is state, not a stat, so it lives per instance. */
    int         shield_hp;          /**< Remaining break pool; 0 when the mode is not a pool. */
    uint8_t     shield_broken;
    double      shield_down_until;  /**< While broken and timed, when it comes back. */
    int         shield_window_hits;
    double      shield_window_start;

    /** Composed affix (V15), or -1 for none. An index into the registry. */
    int16_t     affix_index;

    /** Next instant this NPC lays a trail zone, for per-tick zone abilities (V6). */
    double      zone_trail_next;

    /** Charm: while set, this NPC fights for the player that charmed it. */
    uint32_t    charm_source_id;
    double      charm_expires_at;
} NPCEntity;

/* The pool that holds these entities, its capacity, and its locking all live in
 * npc_world.h. This header is deliberately left with plain data: NPCEntity is
 * memset and copied in several places, which an embedded lock would break, and
 * the tick snapshot copies these fields wholesale. */

/** Snapshot an in-flight basic attack between intent and resolution. */
typedef struct {
    uint8_t     is_active;          // 1 if this attacker has a pending cast
    /** Identify the character that started this cast.
     *
     * The array is indexed by player slot, and slots are recycled on logout. Without
     * an owner the cast of a player who disconnects mid-cast would resolve as whoever
     * next occupies the slot, handing them the damage credit, XP, loot, and quest kill.
     * Every reader must confirm this matches the character now in the slot.
     */
    uint32_t    character_id;
    double      cast_start_time;    // Epoch seconds when cast began
    float       cast_duration;      // How long it takes (from the race's profile)
    uint8_t     attack_type;        // Which shape to resolve at completion
    float       origin_x, origin_y; // Attacker position AT CAST START (snapshot)
    float       aim_x, aim_y;       // Aim point AT CAST START (snapshot)
    float       range;              // From the race's profile, snapshotted
    int         base_damage;
    int         damage_variance;
    float       cone_half_angle;    // Degrees, from the race's profile
    float       line_width;         // World units, from the race's profile
    float       cooldown;           // Stored so we can set last_attack_time on resolve
    uint8_t     damage_stat;        // StatId the race's profile scales this attack with
    /** Retain projectile parameters when the cast resolves at range. */
    uint8_t     is_ranged;
    float       projectile_speed;
    float       projectile_width;
    uint8_t     projectile_damage_stat;
    uint8_t     projectile_damage_type;
} PendingCast;

#endif // COMBAT_CONFIG_H