// ============================================================================
// combat_config.h
//
// Server-side combat configuration. ALL combat constants live here. Clients
// never see this file — they only see the results the server sends back.
// ============================================================================

#ifndef COMBAT_CONFIG_H
#define COMBAT_CONFIG_H

#include <stdint.h>
#include <pthread.h>

// ---------------------------------------------------------------------------
// Per-class attack profiles.
// The server looks up the attacker's class and loads these values. Clients
// have no input into any of this.
// ---------------------------------------------------------------------------

typedef struct {
    float       cast_time;          // Seconds from intent to damage resolution
    float       cooldown;           // Seconds after damage before next attack can start
    float       range;              // Max distance (world units) for target resolution
    int         base_damage;        // Flat damage before variance
    int         damage_variance;    // ±% applied randomly at resolution time
    uint8_t     attack_type;        // AttackType: SINGLE / AOE / CONE / LINE
    float       cone_half_angle;    // Degrees — only used when attack_type == CONE
    float       line_width;         // World units — only used when attack_type == LINE
} ClassAttackProfile;

// ---------------------------------------------------------------------------
// Class attack profiles indexed by class ID (1-based; index 0 is unused).
//
// Design intent:
//   Gladiator  — Slow wind-up, hits ONE target hard. Melee.
//   Ninja      — Fast, narrow cone (cleave). Melee. Low damage per hit but
//                can clip two targets standing side by side.
//   Landweaver — Slow cast, large AoE. Ranged. Medium damage spread across
//                everything nearby.
//   Spirit     — Medium cast, line/ray. Medium range. Piercing bolt that
//                damages everything in a straight line.
// ---------------------------------------------------------------------------

extern const ClassAttackProfile g_class_profiles[5];
 
// ---------------------------------------------------------------------------
// NPC definition — everything the server needs to track a living entity.
// This is intentionally minimal; expand as you add AI, loot tables, etc.
// ---------------------------------------------------------------------------

typedef struct {
    uint32_t    id;                // Unique entity ID (server-assigned)
    char        name[32];
    float       pos_x, pos_y;      // Current world position
    int         health;
    int         max_health;
    float       hitbox_radius;     // For collision / hit detection

    int         defense;           // Damage reduction stat
    int         evasion;           // Dodge chance stat  
    uint32_t    xp_reward;         // XP granted to killer

    uint8_t     is_alive;          // 0 = dead, 1 = alive
} NPCEntity;

// ---------------------------------------------------------------------------
// NPC world state — a fixed-size pool. In a real game this would be per-zone
// or dynamically allocated, but a static array is fine for prototyping.
// ---------------------------------------------------------------------------
#define MAX_NPCS    256

typedef struct {
    NPCEntity   npcs[MAX_NPCS];
    int         count;              // How many slots are in use
    pthread_mutex_t lock;           // Protects the entire array
} NPCWorld;

// ---------------------------------------------------------------------------
// Pending cast — tracks an in-flight attack between intent and resolution.
// Stored per-attacker on the server. When cast_time elapses, the combat
// tick resolves it and clears this.
// ---------------------------------------------------------------------------
typedef struct {
    uint8_t     is_active;          // 1 if this attacker has a pending cast
    double      cast_start_time;    // Epoch seconds when cast began
    float       cast_duration;      // How long it takes (from class profile)
    uint8_t     attack_type;        // Which shape to resolve at completion
    float       origin_x, origin_y; // Attacker position AT CAST START (snapshot)
    float       aim_x, aim_y;       // Aim point AT CAST START (snapshot)
    float       range;              // From class profile, snapshotted
    int         base_damage;
    int         damage_variance;
    float       cone_half_angle;    // Degrees, from class profile
    float       line_width;         // World units, from class profile
    float       cooldown;           // Stored so we can set last_attack_time on resolve
} PendingCast;

#endif // COMBAT_CONFIG_H