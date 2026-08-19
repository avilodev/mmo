/**
 * @file
 * Manage client ability slots, cooldowns, cast state, effects, and rendering.
 */

#include "ability_bar.h"
#include "render/renderer.h"
#include "texture/texture.h"
#include "core/keybinds.h"

#include <string.h>
#include <stdio.h>
#include <math.h>
#include <GLFW/glfw3.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/**
 * Check whether an ability name contains the ASCII substring "heal".
 *
 * @return      Nonzero on a case-insensitive match; otherwise zero.
 */
static int ability_name_is_heal(const char* name) {
    for (const char* p = name; *p; p++) {
        char c0 = p[0] | 0x20;
        char c1 = p[1] ? (p[1] | 0x20) : 0;
        char c2 = p[2] ? (p[2] | 0x20) : 0;
        char c3 = p[3] ? (p[3] | 0x20) : 0;
        if (c0 == 'h' && c1 == 'e' && c2 == 'a' && c3 == 'l')
            return 1;
    }
    return 0;
}

static void get_class_color_for_slot(int slot_index, int slot_count,
                                     float* r, float* g, float* b, float* a) {
    // Default neutral color
    *r = 0.4f; *g = 0.4f; *b = 0.5f; *a = 0.8f;
    (void)slot_index;
    (void)slot_count;
}

/**
 * Return the bar for the form currently in use.
 */
AbilityFormBar* ability_bar_active(AbilityBarState* bar) {
    uint8_t form = bar->active_form < FORM_COUNT ? bar->active_form : FORM_HUMAN;
    return &bar->forms[form];
}

/**
 * Return the bar for the form currently in use, for read-only callers.
 */
const AbilityFormBar* ability_bar_active_const(const AbilityBarState* bar) {
    uint8_t form = bar->active_form < FORM_COUNT ? bar->active_form : FORM_HUMAN;
    return &bar->forms[form];
}

/**
 * Report whether the active form has a resource pool to draw.
 *
 * Human Form is cooldown-only and carries no pool, so the HUD hides the bar rather
 * than drawing an empty one.
 */
int ability_bar_has_resource(const AbilityBarState* bar) {
    return bar->resource_type != RESOURCE_NONE && bar->max_resource > 0;
}

/**
 * Initialize ability-bar state and bottom-center layout.
 */
void ability_bar_init(AbilityBarState* bar, float screen_width, float screen_height) {
    memset(bar, 0, sizeof(AbilityBarState));

    bar->screen_width  = screen_width;
    bar->screen_height = screen_height;
    bar->slot_size     = 48.0f;
    bar->slot_padding  = 6.0f;
    bar->hovered_slot  = -1;

    // Center the bar horizontally, near bottom of screen
    float total_width = MAX_ABILITY_SLOTS * bar->slot_size +
                        (MAX_ABILITY_SLOTS - 1) * bar->slot_padding;
    bar->bar_x = (screen_width - total_width) / 2.0f;
    bar->bar_y = screen_height - bar->slot_size - 20.0f;
}

/**
 * Release all loaded ability icon textures and clear the slot count.
 */
void ability_bar_cleanup(AbilityBarState* bar) {
    for (int form = 0; form < FORM_COUNT; form++) {
        for (int i = 0; i < MAX_ABILITY_SLOTS; i++) {
            if (bar->forms[form].slots[i].texture_id) {
                texture_unload(bar->forms[form].slots[i].texture_id);
                bar->forms[form].slots[i].texture_id = 0;
            }
        }
        bar->forms[form].slot_count = 0;
    }
}

/**
 * Replace one form's ability slots with parallel arrays received from the server.
 *
 * Existing icon textures are released before replacement. A slot that still holds the
 * same ability keeps its running cooldown: the server treats cooldowns as absolute
 * expiry instants that tick in both forms, so a bar refresh from a level-up or a form
 * swap must not present a cooldown as reset.
 *
 * @param count  Number of array entries, clamped to MAX_ABILITY_SLOTS.
 */
void ability_bar_set_form_abilities(AbilityBarState* bar,
                                    uint8_t form,
                                    uint16_t* ability_ids,
                                    const char** ability_names,
                                    float* cooldowns,
                                    float* cast_times,
                                    int* resource_costs,
                                    const char** images,
                                    int count) {
    if (form >= FORM_COUNT) return;

    AbilityFormBar* target = &bar->forms[form];

    target->slot_count = count;
    if (target->slot_count > MAX_ABILITY_SLOTS)
        target->slot_count = MAX_ABILITY_SLOTS;

    for (int i = 0; i < target->slot_count; i++) {
        AbilitySlot* slot = &target->slots[i];

        /* Preserve a cooldown only when the slot is genuinely unchanged. */
        float retained = (slot->id == ability_ids[i]) ? slot->cooldown_remaining : 0.0f;

        if (slot->texture_id) {
            texture_unload(slot->texture_id);
            slot->texture_id = 0;
        }

        slot->id = ability_ids[i];
        strncpy(slot->name, ability_names[i], MAX_ABILITY_NAME - 1);
        slot->name[MAX_ABILITY_NAME - 1] = '\0';
        slot->cooldown_total     = cooldowns[i];
        slot->cooldown_remaining = retained;
        slot->cast_time          = cast_times[i];
        slot->resource_cost      = resource_costs[i];
        slot->is_heal            = ability_name_is_heal(ability_names[i]);

        // Store icon path and load texture
        slot->image[0] = '\0';
        if (images && images[i] && images[i][0]) {
            strncpy(slot->image, images[i], 31);
            slot->image[31] = '\0';

            char path[128];
            snprintf(path, sizeof(path), "Game/Sprites/Abilities/%s", slot->image);
            slot->texture_id = texture_load(path);
        }

        // Fallback color (shown when no icon)
        get_class_color_for_slot(i, count,
            &slot->color_r, &slot->color_g, &slot->color_b, &slot->color_a);
    }

    // Clear unused slots
    for (int i = target->slot_count; i < MAX_ABILITY_SLOTS; i++) {
        if (target->slots[i].texture_id) {
            texture_unload(target->slots[i].texture_id);
        }
        memset(&target->slots[i], 0, sizeof(AbilitySlot));
    }

    printf("[ABILITY_BAR] Set %d abilities for %s form\n",
           target->slot_count, form == FORM_ANIMAL ? "animal" : "human");
}

/**
 * Switch which form's bar is displayed and drives input.
 *
 * The server's per-slot remaining times are authoritative and overwrite whatever the
 * client had been counting down locally, which is what keeps a cooldown that ran
 * while the player was in the other form displaying correctly on return.
 */
void ability_bar_set_form(AbilityBarState* bar, uint8_t form,
                          int32_t resource, int32_t max_resource,
                          uint8_t resource_type, float swap_ready_in,
                          const float* ready_in) {
    if (form >= FORM_COUNT) return;

    bar->active_form   = form;
    bar->resource      = resource;
    bar->max_resource  = max_resource;
    bar->resource_type = resource_type;
    bar->swap_ready_in = swap_ready_in;

    /* A swap interrupts nothing, but the cast bar belonged to the old form. */
    bar->is_casting = 0;
    bar->cast_elapsed = 0.0f;

    if (!ready_in) return;
    for (int i = 0; i < MAX_ABILITY_SLOTS; i++) {
        bar->forms[form].slots[i].cooldown_remaining = ready_in[i];
    }
}

/**
 * Advance ability timers and resolve newly pressed ability bindings.
 *
 * @param delta_time  Elapsed frame time in seconds.
 * @param keys_just_pressed  GLFW-indexed edge-triggered key array.
 * @return      Requested ability identifier, or zero when no cast is requested.
 */
uint16_t ability_bar_update(AbilityBarState* bar, float delta_time,
                            const int* keys_just_pressed, uint32_t race_id) {
    (void)race_id;

    /* Tick both forms' cooldowns, not just the visible one. The server runs them off
     * absolute expiry instants that keep advancing regardless of form, so a client
     * that only ticked the active bar would show the other form's cooldowns frozen
     * at whatever they read when the player swapped away. */
    for (int form = 0; form < FORM_COUNT; form++) {
        for (int i = 0; i < bar->forms[form].slot_count; i++) {
            AbilitySlot* slot = &bar->forms[form].slots[i];
            if (slot->cooldown_remaining <= 0.0f) continue;

            slot->cooldown_remaining -= delta_time;
            if (slot->cooldown_remaining < 0.0f) slot->cooldown_remaining = 0.0f;
        }
    }

    if (bar->swap_ready_in > 0.0f) {
        bar->swap_ready_in -= delta_time;
        if (bar->swap_ready_in < 0.0f) bar->swap_ready_in = 0.0f;
    }

    AbilityFormBar* active = ability_bar_active(bar);

    // Tick reject flash
    for (int i = 0; i < MAX_ABILITY_SLOTS; i++) {
        if (bar->reject_flash[i] > 0.0f) {
            bar->reject_flash[i] -= delta_time;
            if (bar->reject_flash[i] < 0.0f)
                bar->reject_flash[i] = 0.0f;
        }
    }

    // Tick cast bar
    if (bar->is_casting) {
        bar->cast_elapsed += delta_time;
        if (bar->cast_elapsed >= bar->cast_duration) {
            bar->is_casting = 0;
        }
    }

    // Tick status effects
    for (int i = 0; i < MAX_CLIENT_EFFECTS; i++) {
        if (!bar->effects[i].active) continue;
        bar->effects[i].duration_remaining -= delta_time;
        if (bar->effects[i].duration_remaining <= 0.0f) {
            bar->effects[i].active = 0;
        }
    }

    // Check input: ability keys from keybinds (default 1-5)
    if (!bar->is_casting && active->slot_count > 0) {
        int key_map[MAX_ABILITY_SLOTS] = {
            g_keybinds.ability[0], g_keybinds.ability[1], g_keybinds.ability[2],
            g_keybinds.ability[3], g_keybinds.ability[4]
        };

        for (int i = 0; i < active->slot_count; i++) {
            if (!keys_just_pressed[key_map[i]]) continue;

            AbilitySlot* slot = &active->slots[i];
            printf("[ABILITY_BAR] Key %d pressed -> slot %d (id=%u, cd=%.1f, cost=%d, resource=%d)\n",
                   i + 1, i, slot->id, slot->cooldown_remaining,
                   slot->resource_cost, bar->resource);

            if (slot->id == 0) {
                printf("[ABILITY_BAR] Slot %d has no ability assigned!\n", i);
                continue;
            }

            if (slot->cooldown_remaining > 0.0f) {
                printf("[ABILITY_BAR] Slot %d on cooldown (%.1fs)\n",
                       i, slot->cooldown_remaining);
                continue;
            }

            /* Human Form abilities cost nothing, so this check is naturally inert
             * there rather than needing to be skipped. */
            if (slot->resource_cost > 0 && slot->resource_cost > bar->resource) {
                printf("[ABILITY_BAR] Not enough resource for '%s' (%d/%d)\n",
                       slot->name, bar->resource, slot->resource_cost);
                bar->reject_flash[i] = 0.3f;
                continue;
            }

            printf("[ABILITY_BAR] Casting ability '%s' (id=%u)!\n", slot->name, slot->id);
            return slot->id;
        }
    }

    return 0; // No cast requested
}

/**
 * Start the server-confirmed ability cast display.
 *
 * @param cast_time  Cast duration in seconds.
 */
void ability_bar_on_cast_start(AbilityBarState* bar, uint16_t ability_id, float cast_time) {
    bar->is_casting = 1;
    bar->cast_elapsed = 0.0f;
    bar->cast_duration = cast_time;
    bar->casting_ability_id = ability_id;
    bar->cast_is_heal = 0;

    // Find ability name and heal flag
    AbilityFormBar* active = ability_bar_active(bar);
    for (int i = 0; i < active->slot_count; i++) {
        if (active->slots[i].id == ability_id) {
            strncpy(bar->casting_ability_name, active->slots[i].name, MAX_ABILITY_NAME - 1);
            bar->casting_ability_name[MAX_ABILITY_NAME - 1] = '\0';
            bar->cast_is_heal = active->slots[i].is_heal;
            break;
        }
    }
}

/**
 * Resolve a cast and start its configured cooldown.
 */
void ability_bar_on_cast_resolve(AbilityBarState* bar, uint16_t ability_id) {
    bar->is_casting = 0;

    // Start cooldown on the resolved ability
    AbilityFormBar* active = ability_bar_active(bar);
    for (int i = 0; i < active->slot_count; i++) {
        if (active->slots[i].id == ability_id) {
            active->slots[i].cooldown_remaining = active->slots[i].cooldown_total;
            break;
        }
    }
}

/**
 * Cancel the active cast and flash the identified slot.
 */
void ability_bar_on_cast_cancel(AbilityBarState* bar, uint16_t ability_id) {
    bar->is_casting = 0;
    bar->cast_elapsed = 0.0f;

    // Flash the rejected slot red so the player knows why the cast failed
    if (ability_id > 0) {
        AbilityFormBar* active = ability_bar_active(bar);
        for (int i = 0; i < active->slot_count; i++) {
            if (active->slots[i].id == ability_id) {
                bar->reject_flash[i] = 0.3f;
                break;
            }
        }
    }
}

/**
 * Apply a server-provided cooldown to an ability slot.
 *
 * @param cooldown  Cooldown duration in seconds.
 */
void ability_bar_on_cooldown(AbilityBarState* bar, uint16_t ability_id, float cooldown) {
    /* Search both forms: an ability that started a cooldown may belong to the bar the
     * player has since swapped away from, and that cooldown still has to be recorded. */
    for (int form = 0; form < FORM_COUNT; form++) {
        for (int i = 0; i < bar->forms[form].slot_count; i++) {
            if (bar->forms[form].slots[i].id != ability_id) continue;

            bar->forms[form].slots[i].cooldown_remaining = cooldown;
            bar->forms[form].slots[i].cooldown_total = cooldown;
            return;
        }
    }
}

/**
 * Replace the displayed resource pool.
 *
 * Whichever pool the active form carries — mana, stamina, or rage — travels on the
 * same update, because the wire pair generalised rather than multiplied.
 */
void ability_bar_on_resource_update(AbilityBarState* bar, int32_t resource, int32_t max_resource) {
    bar->resource = resource;
    bar->max_resource = max_resource;
}

/**
 * Refresh or add a client status-effect display entry.
 *
 * New effects are discarded when every effect slot is active.
 *
 * @param duration  Remaining effect duration in seconds.
 */
void ability_bar_on_effect_apply(AbilityBarState* bar, uint8_t effect_type,
                                  int value, float duration, uint32_t source_id) {
    // Check if we already have this effect type — refresh duration
    for (int i = 0; i < MAX_CLIENT_EFFECTS; i++) {
        if (bar->effects[i].active && bar->effects[i].effect_type == effect_type) {
            bar->effects[i].duration_remaining = duration;
            bar->effects[i].value = value;
            bar->effects[i].source_id = source_id;
            return;
        }
    }

    // Find free slot
    for (int i = 0; i < MAX_CLIENT_EFFECTS; i++) {
        if (!bar->effects[i].active) {
            bar->effects[i].active = 1;
            bar->effects[i].effect_type = effect_type;
            bar->effects[i].value = value;
            bar->effects[i].duration_remaining = duration;
            bar->effects[i].source_id = source_id;
            return;
        }
    }
}

/**
 * Remove the first active status effect of a type.
 */
void ability_bar_on_effect_remove(AbilityBarState* bar, uint8_t effect_type) {
    for (int i = 0; i < MAX_CLIENT_EFFECTS; i++) {
        if (bar->effects[i].active && bar->effects[i].effect_type == effect_type) {
            bar->effects[i].active = 0;
            return;
        }
    }
}

/**
 * Render ability slots, cooldown overlays, costs, and key labels.
 */
void ability_bar_render(const AbilityBarState* bar) {
    float x = bar->bar_x;
    float y = bar->bar_y;
    float size = bar->slot_size;
    float pad = bar->slot_padding;

    // Background panel
    float total_w = MAX_ABILITY_SLOTS * size + (MAX_ABILITY_SLOTS - 1) * pad + 16;
    renderer_draw_rect(x - 8, y - 8, total_w, size + 16,
                       0.05f, 0.05f, 0.1f, 0.85f);

    // Border
    renderer_draw_rect(x - 8, y - 8, total_w, 2, 0.3f, 0.3f, 0.4f, 1.0f);
    renderer_draw_rect(x - 8, y + size + 6, total_w, 2, 0.3f, 0.3f, 0.4f, 1.0f);

    const AbilityFormBar* active = ability_bar_active_const(bar);

    for (int i = 0; i < MAX_ABILITY_SLOTS; i++) {
        float sx = x + i * (size + pad);
        float sy = y;

        const AbilitySlot* slot = &active->slots[i];
        int has_ability = (i < active->slot_count && slot->id > 0);

        // Slot background
        if (has_ability) {
            renderer_draw_rect(sx, sy, size, size, 0.15f, 0.15f, 0.25f, 1.0f);

            if (slot->texture_id) {
                // Draw icon
                renderer_draw_sprite(sx + 2, sy + 2, size - 4, size - 4,
                                     slot->texture_id);
            } else {
                // Fallback: colored fill
                renderer_draw_rect(sx + 2, sy + 2, size - 4, size - 4,
                                   slot->color_r,
                                   slot->color_g,
                                   slot->color_b,
                                   0.4f);
            }
        } else {
            renderer_draw_rect(sx, sy, size, size, 0.1f, 0.1f, 0.15f, 0.6f);
        }

        // Slot border
        float border_r = 0.4f, border_g = 0.4f, border_b = 0.5f;
        if (has_ability && slot->cooldown_remaining <= 0.0f) {
            border_r = 0.6f; border_g = 0.6f; border_b = 0.7f;
        }
        renderer_draw_rect(sx, sy, size, 2, border_r, border_g, border_b, 1.0f);
        renderer_draw_rect(sx, sy + size - 2, size, 2, border_r, border_g, border_b, 1.0f);
        renderer_draw_rect(sx, sy, 2, size, border_r, border_g, border_b, 1.0f);
        renderer_draw_rect(sx + size - 2, sy, 2, size, border_r, border_g, border_b, 1.0f);

        // Cooldown overlay
        if (has_ability && slot->cooldown_remaining > 0.0f) {
            float cd_pct = slot->cooldown_remaining / slot->cooldown_total;
            if (cd_pct > 1.0f) cd_pct = 1.0f;

            // Dark overlay sweeping down
            float cd_height = size * cd_pct;
            renderer_draw_rect(sx, sy, size, cd_height, 0.0f, 0.0f, 0.0f, 0.6f);

            // Cooldown timer text
            char cd_buf[8];
            snprintf(cd_buf, sizeof(cd_buf), "%.0f", slot->cooldown_remaining);
            renderer_draw_text(sx + size / 2 - 5, sy + size / 2 + 5, cd_buf);
        }

        // Not enough mana indicator
        /* Dim a slot the player cannot currently afford. Human Form abilities cost
         * nothing, so nothing is ever dimmed there. */
        if (has_ability && slot->resource_cost > 0 && slot->resource_cost > bar->resource) {
            renderer_draw_rect(sx, sy, size, size, 0.0f, 0.0f, 0.3f, 0.4f);
        }

        // Rejection flash
        if (bar->reject_flash[i] > 0.0f) {
            float alpha = bar->reject_flash[i] / 0.3f;  // fade out
            renderer_draw_rect(sx, sy, size, size, 0.9f, 0.1f, 0.1f, alpha * 0.6f);
        }

        // Keybind number (top-left)
        char key_buf[4];
        snprintf(key_buf, sizeof(key_buf), "%d", i + 1);
        renderer_draw_text(sx + 3, sy + 14, key_buf);

        // Ability name (bottom of slot, small)
        if (has_ability) {
            // Show first 4 chars of name
            char short_name[6] = {0};
            strncpy(short_name, slot->name, 5);
            renderer_draw_text(sx + 2, sy + size - 4, short_name);
        }
    }
}

/**
 * Render active status effects as timed colored icons.
 */
void ability_bar_render_effects(const AbilityBarState* bar, float x, float y) {
    float icon_size = 28.0f;
    float icon_pad = 4.0f;
    int drawn = 0;

    for (int i = 0; i < MAX_CLIENT_EFFECTS; i++) {
        if (!bar->effects[i].active) continue;

        float ix = x + drawn * (icon_size + icon_pad);
        float iy = y;

        // Color by effect type
        float r = 0.5f, g = 0.5f, b = 0.5f;
        switch (bar->effects[i].effect_type) {
            case CLIENT_EFFECT_DOT:     r = 0.8f; g = 0.2f; b = 0.2f; break;
            case CLIENT_EFFECT_HOT:     r = 0.2f; g = 0.8f; b = 0.3f; break;
            case CLIENT_EFFECT_STUN:    r = 1.0f; g = 1.0f; b = 0.2f; break;
            case CLIENT_EFFECT_SLOW:    r = 0.3f; g = 0.3f; b = 0.8f; break;
            case CLIENT_EFFECT_BUFF:    r = 0.9f; g = 0.7f; b = 0.2f; break;
            case CLIENT_EFFECT_STEALTH: r = 0.5f; g = 0.5f; b = 0.7f; break;
            default: break;
        }

        // Background
        renderer_draw_rect(ix, iy, icon_size, icon_size, 0.05f, 0.05f, 0.1f, 0.9f);

        // Color fill
        renderer_draw_rect(ix + 2, iy + 2, icon_size - 4, icon_size - 4,
                           r, g, b, 0.6f);

        // Border
        renderer_draw_rect(ix, iy, icon_size, 1, 0.5f, 0.5f, 0.5f, 1.0f);
        renderer_draw_rect(ix, iy + icon_size - 1, icon_size, 1, 0.5f, 0.5f, 0.5f, 1.0f);
        renderer_draw_rect(ix, iy, 1, icon_size, 0.5f, 0.5f, 0.5f, 1.0f);
        renderer_draw_rect(ix + icon_size - 1, iy, 1, icon_size, 0.5f, 0.5f, 0.5f, 1.0f);

        // Duration remaining
        char dur_buf[8];
        snprintf(dur_buf, sizeof(dur_buf), "%.0f", bar->effects[i].duration_remaining);
        renderer_draw_text(ix + icon_size / 2 - 4, iy + icon_size - 3, dur_buf);

        drawn++;
    }
}

/**
 * Render the active ability cast bar above the ability slots.
 */
void ability_bar_render_cast_bar(const AbilityBarState* bar,
                                  float screen_width, float screen_height) {
    if (!bar->is_casting || bar->cast_duration <= 0.0f) return;

    float bar_width = 250.0f;
    float bar_height = 20.0f;
    float bx = (screen_width - bar_width) / 2.0f;
    float by = screen_height - 120.0f;  // Above the ability bar

    float progress = bar->cast_elapsed / bar->cast_duration;
    if (progress > 1.0f) progress = 1.0f;

    // Background
    renderer_draw_rect(bx - 2, by - 2, bar_width + 4, bar_height + 4,
                       0.0f, 0.0f, 0.0f, 0.8f);

    // Empty bar
    renderer_draw_rect(bx, by, bar_width, bar_height,
                       0.15f, 0.15f, 0.2f, 1.0f);

    // Fill — green for heals, red for damage abilities
    float cr = bar->cast_is_heal ? 0.15f : 0.90f;
    float cg = bar->cast_is_heal ? 0.85f : 0.18f;
    float cb = bar->cast_is_heal ? 0.30f : 0.18f;
    renderer_draw_rect(bx, by, bar_width * progress, bar_height, cr, cg, cb, 0.9f);

    // Glowing edge
    if (progress > 0.01f && progress < 0.99f) {
        float edge_x = bx + bar_width * progress - 2;
        renderer_draw_rect(edge_x, by, 4, bar_height, 1.0f, 1.0f, 1.0f, 0.8f);
    }

    // Border
    renderer_draw_rect(bx, by, bar_width, 2, 0.5f, 0.5f, 0.6f, 1.0f);
    renderer_draw_rect(bx, by + bar_height - 2, bar_width, 2, 0.5f, 0.5f, 0.6f, 1.0f);
    renderer_draw_rect(bx, by, 2, bar_height, 0.5f, 0.5f, 0.6f, 1.0f);
    renderer_draw_rect(bx + bar_width - 2, by, 2, bar_height, 0.5f, 0.5f, 0.6f, 1.0f);

    // Ability name (left of bar)
    renderer_draw_text(bx, by - 14, bar->casting_ability_name);

    // Cast time remaining (right of bar)
    float remaining = bar->cast_duration - bar->cast_elapsed;
    if (remaining < 0) remaining = 0;
    char time_buf[16];
    snprintf(time_buf, sizeof(time_buf), "%.1fs", remaining);
    renderer_draw_text(bx + bar_width + 10, by + bar_height - 5, time_buf);
}
