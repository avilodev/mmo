// ============================================================================
// ability_bar.h — Client-side ability bar system
//
// Manages 5 ability slots (keys 1-5), per-slot cooldowns, mana display,
// and sends ABILITY_CAST_INTENT to the server.
// ============================================================================

#ifndef ABILITY_BAR_H
#define ABILITY_BAR_H

#include <stdint.h>

#define MAX_ABILITY_SLOTS 5
#define MAX_ABILITY_NAME  32

// ---------------------------------------------------------------------------
// Status effect types (must match server's StatusEffectType)
// ---------------------------------------------------------------------------

typedef enum {
    CLIENT_EFFECT_NONE    = 0,
    CLIENT_EFFECT_DOT     = 1,
    CLIENT_EFFECT_HOT     = 2,
    CLIENT_EFFECT_STUN    = 3,
    CLIENT_EFFECT_SLOW    = 4,
    CLIENT_EFFECT_BUFF    = 5,
    CLIENT_EFFECT_STEALTH = 6,
    CLIENT_EFFECT_KNOCKUP = 7,
    CLIENT_EFFECT_LINK    = 8,
    CLIENT_EFFECT_CLEANSE = 9
} ClientEffectType;

// ---------------------------------------------------------------------------
// Client-side ability slot definition
// ---------------------------------------------------------------------------

typedef struct {
    uint16_t    id;                     // Server ability ID (0 = empty slot)
    char        name[MAX_ABILITY_NAME];
    float       cooldown_total;         // Total CD duration (from server)
    float       cooldown_remaining;     // Current CD remaining (ticked locally)
    float       cast_time;              // For cast bar display
    int         mana_cost;
    int         is_heal;                // 1 = healing ability (green cast bar)

    // Icon
    char        image[32];              // Filename, e.g. "cleave.png"
    unsigned int texture_id;           // OpenGL texture (0 = not loaded yet)

    // Visual hints (fallback color when no icon)
    float       color_r, color_g, color_b, color_a;
} AbilitySlot;

// ---------------------------------------------------------------------------
// Active status effect displayed on the player's buff bar
// ---------------------------------------------------------------------------

#define MAX_CLIENT_EFFECTS 8

typedef struct {
    uint8_t     active;
    uint8_t     effect_type;        // ClientEffectType
    int         value;
    float       duration_remaining;
    uint32_t    source_id;
} ClientStatusEffect;

// ---------------------------------------------------------------------------
// Main ability bar state
// ---------------------------------------------------------------------------

typedef struct {
    AbilitySlot slots[MAX_ABILITY_SLOTS];
    int         slot_count;             // How many slots are filled

    // Mana
    int32_t     mana;
    int32_t     max_mana;

    // Cast state (mirrors combat for ability-specific casts)
    int         is_casting;
    float       cast_elapsed;
    float       cast_duration;
    uint16_t    casting_ability_id;
    char        casting_ability_name[MAX_ABILITY_NAME];
    int         cast_is_heal;           // 1 = current cast is a healing ability

    // Active effects on the player
    ClientStatusEffect effects[MAX_CLIENT_EFFECTS];

    // Layout (set once on init)
    float       bar_x;
    float       bar_y;
    float       slot_size;
    float       slot_padding;
    float       screen_width;
    float       screen_height;

    // Input state
    int         hovered_slot;           // -1 = none

    // Rejection flash (set when server cancels a cast before it started)
    float       reject_flash[MAX_ABILITY_SLOTS];  // >0 = flash red, ticks to 0
} AbilityBarState;

// ============================================================================
// API
// ============================================================================

// Initialize the ability bar (call once after game_init)
void ability_bar_init(AbilityBarState* bar, float screen_width, float screen_height);
void ability_bar_cleanup(AbilityBarState* bar);  // Unload all slot textures

// Set abilities from server data (call after player data is loaded)
// ability_ids and ability_names are parallel arrays of length count.
void ability_bar_set_abilities(AbilityBarState* bar,
                               uint16_t* ability_ids,
                               const char** ability_names,
                               float* cooldowns,
                               float* cast_times,
                               int* mana_costs,
                               const char** images,
                               int count);

// Update per frame (tick cooldowns, check input)
// Returns the ability ID to cast (>0) or 0 if no cast requested this frame.
uint16_t ability_bar_update(AbilityBarState* bar, float delta_time,
                            const int* keys_just_pressed, uint32_t player_class);

// Server told us a cast started
void ability_bar_on_cast_start(AbilityBarState* bar, uint16_t ability_id, float cast_time);

// Server told us the cast resolved (damage/heal applied)
void ability_bar_on_cast_resolve(AbilityBarState* bar, uint16_t ability_id);

// Server told us the cast was cancelled (ability_id=0 if unknown)
void ability_bar_on_cast_cancel(AbilityBarState* bar, uint16_t ability_id);

// Server told us a cooldown started for this ability
void ability_bar_on_cooldown(AbilityBarState* bar, uint16_t ability_id, float cooldown);

// Server sent mana update
void ability_bar_on_mana_update(AbilityBarState* bar, int32_t mana, int32_t max_mana);

// Server applied a status effect
void ability_bar_on_effect_apply(AbilityBarState* bar, uint8_t effect_type,
                                  int value, float duration, uint32_t source_id);

// Server removed a status effect
void ability_bar_on_effect_remove(AbilityBarState* bar, uint8_t effect_type);

// Render the ability bar (call in screen-space after camera pop)
void ability_bar_render(const AbilityBarState* bar);

// Render the mana bar (call in screen-space)
void ability_bar_render_mana(const AbilityBarState* bar, float x, float y,
                              float width, float height);

// Render active status effects as buff icons
void ability_bar_render_effects(const AbilityBarState* bar, float x, float y);

// Render ability cast bar (if casting an ability)
void ability_bar_render_cast_bar(const AbilityBarState* bar,
                                  float screen_width, float screen_height);

#endif // ABILITY_BAR_H