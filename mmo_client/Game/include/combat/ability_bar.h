/**
 * @file
 * Declare client ability slots, cast presentation, and status-effect display state.
 */

#ifndef ABILITY_BAR_H
#define ABILITY_BAR_H

#include "protocol.h"

#include <stdint.h>

/* MAX_ABILITY_SLOTS comes from protocol.h so the two repos cannot disagree on the
 * hotbar's size. Both forms share those five slots; swapping forms swaps what fills
 * them, and each form keeps its own cooldowns. */
#define MAX_ABILITY_NAME  32

/** Mirror the server StatusEffectType wire values. */
typedef enum {
    CLIENT_EFFECT_NONE         = 0,
    CLIENT_EFFECT_DOT          = 1,
    CLIENT_EFFECT_HOT          = 2,
    CLIENT_EFFECT_STUN         = 3,
    CLIENT_EFFECT_SLOW         = 4,
    CLIENT_EFFECT_BUFF         = 5,
    CLIENT_EFFECT_STEALTH      = 6,
    CLIENT_EFFECT_KNOCKUP      = 7,
    CLIENT_EFFECT_LINK         = 8,
    CLIENT_EFFECT_CLEANSE      = 9,
    CLIENT_EFFECT_TAUNT        = 10,
    CLIENT_EFFECT_RESOURCE     = 11,
    CLIENT_EFFECT_DAMAGE_TAKEN = 12,
    CLIENT_EFFECT_DAMAGE_DEALT = 13,
    CLIENT_EFFECT_ROOT         = 14,
    CLIENT_EFFECT_CHANNEL      = 15,
    CLIENT_EFFECT_HOT_PERCENT  = 16
} ClientEffectType;

/** Describe one server-defined ability slot and its client presentation state. */
typedef struct {
    uint16_t    id;                     /**< Server ability identifier, or 0 for an empty slot. */
    char        name[MAX_ABILITY_NAME];
    float       cooldown_total;         /**< Server-defined cooldown duration in seconds. */
    float       cooldown_remaining;     /**< Locally tracked remaining cooldown in seconds. */
    float       cast_time;               /**< Cast duration in seconds. */
    int         resource_cost;           /**< Always 0 in Human Form, which has no pool. */
    int         is_heal;

    char        image[32];
    unsigned int texture_id;            /**< OpenGL texture name, or 0 until loaded. */

    /** Fallback color used when no icon texture is available. */
    float       color_r, color_g, color_b, color_a;
} AbilitySlot;

#define MAX_CLIENT_EFFECTS 8

/** Track one active status effect for the player's buff display. */
typedef struct {
    uint8_t     active;
    uint8_t     effect_type;        /**< ClientEffectType wire value. */
    int         value;
    float       duration_remaining;
    uint32_t    source_id;
} ClientStatusEffect;

/** Hold the five slots and their cooldowns for one form. */
typedef struct {
    AbilitySlot slots[MAX_ABILITY_SLOTS];
    int         slot_count;
} AbilityFormBar;

/** Aggregate ability, resource, cast, effect, layout, and input presentation state. */
typedef struct {
    /** One bar per form. The server sends both on entry, so a swap needs no round trip. */
    AbilityFormBar forms[FORM_COUNT];
    uint8_t     active_form;

    /** Whichever pool the active form carries; both are zero in Human Form. */
    int32_t     resource;
    int32_t     max_resource;
    uint8_t     resource_type;      /**< ResourceType; RESOURCE_NONE hides the bar. */

    float       swap_ready_in;      /**< Seconds until another form swap is allowed. */

    /** Ability-specific cast state mirrored from server events. */
    int         is_casting;
    float       cast_elapsed;
    float       cast_duration;
    uint16_t    casting_ability_id;
    char        casting_ability_name[MAX_ABILITY_NAME];
    int         cast_is_heal;

    ClientStatusEffect effects[MAX_CLIENT_EFFECTS];

    /** Screen-space layout established during initialization. */
    float       bar_x;
    float       bar_y;
    float       slot_size;
    float       slot_padding;
    float       screen_width;
    float       screen_height;

    int         hovered_slot;           /**< Hovered slot index, or -1. */

    float       reject_flash[MAX_ABILITY_SLOTS];  /**< Remaining rejection-flash time by slot. */
} AbilityBarState;

void ability_bar_init(AbilityBarState* bar, float screen_width, float screen_height);
void ability_bar_cleanup(AbilityBarState* bar);

/** Return the bar for the form currently in use. */
AbilityFormBar* ability_bar_active(AbilityBarState* bar);

/** Return the bar for the form currently in use, for read-only callers. */
const AbilityFormBar* ability_bar_active_const(const AbilityBarState* bar);

/** Set one form's slots from parallel server arrays containing count entries.
 *
 * Cooldowns already running in that form are preserved: the server treats them as
 * absolute expiry instants that keep ticking regardless of form, so a bar refresh
 * must not present them as reset.
 */
void ability_bar_set_form_abilities(AbilityBarState* bar,
                                    uint8_t form,
                                    uint16_t* ability_ids,
                                    const char** ability_names,
                                    float* cooldowns,
                                    float* cast_times,
                                    int* resource_costs,
                                    const char** images,
                                    int count);

/** Switch which form's bar is displayed and drives input.
 *
 * @param resource_type  ResourceType for the new form; RESOURCE_NONE hides the bar.
 * @param ready_in       Per-slot cooldown remaining, as the server reports it; may be NULL.
 */
void ability_bar_set_form(AbilityBarState* bar, uint8_t form,
                          int32_t resource, int32_t max_resource,
                          uint8_t resource_type, float swap_ready_in,
                          const float* ready_in);

/** Report whether the active form has a resource pool to draw. */
int ability_bar_has_resource(const AbilityBarState* bar);

/** Return an ability identifier requested this frame, or 0. */
uint16_t ability_bar_update(AbilityBarState* bar, float delta_time,
                            const int* keys_just_pressed, uint32_t race_id);

void ability_bar_on_cast_start(AbilityBarState* bar, uint16_t ability_id, float cast_time);

void ability_bar_on_cast_resolve(AbilityBarState* bar, uint16_t ability_id);

void ability_bar_on_cast_cancel(AbilityBarState* bar, uint16_t ability_id);

void ability_bar_on_cooldown(AbilityBarState* bar, uint16_t ability_id, float cooldown);

/** Update the active form's resource pool from a server update. */
void ability_bar_on_resource_update(AbilityBarState* bar, int32_t resource, int32_t max_resource);

void ability_bar_on_effect_apply(AbilityBarState* bar, uint8_t effect_type,
                                  int value, float duration, uint32_t source_id);

void ability_bar_on_effect_remove(AbilityBarState* bar, uint8_t effect_type);

void ability_bar_render(const AbilityBarState* bar);

void ability_bar_render_effects(const AbilityBarState* bar, float x, float y);

void ability_bar_render_cast_bar(const AbilityBarState* bar,
                                  float screen_width, float screen_height);

#endif // ABILITY_BAR_H
