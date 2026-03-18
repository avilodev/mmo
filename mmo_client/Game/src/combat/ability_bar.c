// ============================================================================
// ability_bar.c — Client-side ability bar rendering and logic
// ============================================================================

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

// ---------------------------------------------------------------------------
// Class color palettes for ability slots
// ---------------------------------------------------------------------------

// Returns 1 if the ability name indicates a healing ability
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

// ============================================================================
// INIT
// ============================================================================

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

void ability_bar_cleanup(AbilityBarState* bar) {
    for (int i = 0; i < MAX_ABILITY_SLOTS; i++) {
        if (bar->slots[i].texture_id) {
            texture_unload(bar->slots[i].texture_id);
            bar->slots[i].texture_id = 0;
        }
    }
    bar->slot_count = 0;
}

// ============================================================================
// SET ABILITIES (from server data on login)
// ============================================================================

void ability_bar_set_abilities(AbilityBarState* bar,
                               uint16_t* ability_ids,
                               const char** ability_names,
                               float* cooldowns,
                               float* cast_times,
                               int* mana_costs,
                               const char** images,
                               int count) {
    bar->slot_count = count;
    if (bar->slot_count > MAX_ABILITY_SLOTS)
        bar->slot_count = MAX_ABILITY_SLOTS;

    for (int i = 0; i < bar->slot_count; i++) {
        // Unload old texture if slot is being replaced
        if (bar->slots[i].texture_id) {
            texture_unload(bar->slots[i].texture_id);
            bar->slots[i].texture_id = 0;
        }

        bar->slots[i].id = ability_ids[i];
        strncpy(bar->slots[i].name, ability_names[i], MAX_ABILITY_NAME - 1);
        bar->slots[i].cooldown_total     = cooldowns[i];
        bar->slots[i].cooldown_remaining = 0.0f;
        bar->slots[i].cast_time          = cast_times[i];
        bar->slots[i].mana_cost          = mana_costs[i];
        bar->slots[i].is_heal            = ability_name_is_heal(ability_names[i]);

        // Store icon path and load texture
        bar->slots[i].image[0] = '\0';
        if (images && images[i] && images[i][0]) {
            strncpy(bar->slots[i].image, images[i], 31);
            bar->slots[i].image[31] = '\0';

            char path[128];
            snprintf(path, sizeof(path), "Game/Sprites/Abilities/%s", bar->slots[i].image);
            bar->slots[i].texture_id = texture_load(path);
        }

        // Fallback color (shown when no icon)
        get_class_color_for_slot(i, count,
            &bar->slots[i].color_r, &bar->slots[i].color_g,
            &bar->slots[i].color_b, &bar->slots[i].color_a);
    }

    // Clear unused slots
    for (int i = bar->slot_count; i < MAX_ABILITY_SLOTS; i++) {
        memset(&bar->slots[i], 0, sizeof(AbilitySlot));
    }

    printf("[ABILITY_BAR] Set %d abilities\n", bar->slot_count);
}

// ============================================================================
// UPDATE (per frame)
// ============================================================================

uint16_t ability_bar_update(AbilityBarState* bar, float delta_time,
                            const int* keys_just_pressed, uint32_t player_class) {
    (void)player_class;
    // Tick cooldowns
    for (int i = 0; i < bar->slot_count; i++) {
        if (bar->slots[i].cooldown_remaining > 0.0f) {
            bar->slots[i].cooldown_remaining -= delta_time;
            if (bar->slots[i].cooldown_remaining < 0.0f)
                bar->slots[i].cooldown_remaining = 0.0f;
        }
    }

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
    if (!bar->is_casting && bar->slot_count > 0) {
        int key_map[5] = {
            g_keybinds.ability[0], g_keybinds.ability[1], g_keybinds.ability[2],
            g_keybinds.ability[3], g_keybinds.ability[4]
        };

        for (int i = 0; i < bar->slot_count; i++) {
            if (!keys_just_pressed[key_map[i]]) continue;

            printf("[ABILITY_BAR] Key %d pressed -> slot %d (id=%u, cd=%.1f, mana_cost=%d, mana=%d)\n",
                   i + 1, i, bar->slots[i].id,
                   bar->slots[i].cooldown_remaining,
                   bar->slots[i].mana_cost, bar->mana);

            if (bar->slots[i].id == 0) {
                printf("[ABILITY_BAR] Slot %d has no ability assigned!\n", i);
                continue;
            }

            if (bar->slots[i].cooldown_remaining > 0.0f) {
                printf("[ABILITY_BAR] Slot %d on cooldown (%.1fs)\n",
                       i, bar->slots[i].cooldown_remaining);
                continue;
            }

            if (bar->slots[i].mana_cost > 0 && bar->slots[i].mana_cost > bar->mana) {
                printf("[ABILITY_BAR] Not enough mana for '%s' (%d/%d)\n",
                       bar->slots[i].name, bar->mana, bar->slots[i].mana_cost);
                continue;
            }

            printf("[ABILITY_BAR] Casting ability '%s' (id=%u)!\n",
                   bar->slots[i].name, bar->slots[i].id);
            return bar->slots[i].id;
        }
    }

    return 0; // No cast requested
}

// ============================================================================
// SERVER EVENT HANDLERS
// ============================================================================

void ability_bar_on_cast_start(AbilityBarState* bar, uint16_t ability_id, float cast_time) {
    bar->is_casting = 1;
    bar->cast_elapsed = 0.0f;
    bar->cast_duration = cast_time;
    bar->casting_ability_id = ability_id;
    bar->cast_is_heal = 0;

    // Find ability name and heal flag
    for (int i = 0; i < bar->slot_count; i++) {
        if (bar->slots[i].id == ability_id) {
            strncpy(bar->casting_ability_name, bar->slots[i].name, MAX_ABILITY_NAME - 1);
            bar->cast_is_heal = bar->slots[i].is_heal;
            break;
        }
    }
}

void ability_bar_on_cast_resolve(AbilityBarState* bar, uint16_t ability_id) {
    bar->is_casting = 0;

    // Start cooldown on the resolved ability
    for (int i = 0; i < bar->slot_count; i++) {
        if (bar->slots[i].id == ability_id) {
            bar->slots[i].cooldown_remaining = bar->slots[i].cooldown_total;
            break;
        }
    }
}

void ability_bar_on_cast_cancel(AbilityBarState* bar, uint16_t ability_id) {
    bar->is_casting = 0;
    bar->cast_elapsed = 0.0f;

    // Flash the rejected slot red so the player knows why the cast failed
    if (ability_id > 0) {
        for (int i = 0; i < bar->slot_count; i++) {
            if (bar->slots[i].id == ability_id) {
                bar->reject_flash[i] = 0.3f;
                break;
            }
        }
    }
}

void ability_bar_on_cooldown(AbilityBarState* bar, uint16_t ability_id, float cooldown) {
    for (int i = 0; i < bar->slot_count; i++) {
        if (bar->slots[i].id == ability_id) {
            bar->slots[i].cooldown_remaining = cooldown;
            bar->slots[i].cooldown_total = cooldown;
            break;
        }
    }
}

void ability_bar_on_mana_update(AbilityBarState* bar, int32_t mana, int32_t max_mana) {
    bar->mana = mana;
    bar->max_mana = max_mana;
}

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

void ability_bar_on_effect_remove(AbilityBarState* bar, uint8_t effect_type) {
    for (int i = 0; i < MAX_CLIENT_EFFECTS; i++) {
        if (bar->effects[i].active && bar->effects[i].effect_type == effect_type) {
            bar->effects[i].active = 0;
            return;
        }
    }
}

// ============================================================================
// RENDER — ABILITY SLOTS
// ============================================================================

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

    for (int i = 0; i < MAX_ABILITY_SLOTS; i++) {
        float sx = x + i * (size + pad);
        float sy = y;

        int has_ability = (i < bar->slot_count && bar->slots[i].id > 0);

        // Slot background
        if (has_ability) {
            renderer_draw_rect(sx, sy, size, size, 0.15f, 0.15f, 0.25f, 1.0f);

            if (bar->slots[i].texture_id) {
                // Draw icon
                renderer_draw_sprite(sx + 2, sy + 2, size - 4, size - 4,
                                     bar->slots[i].texture_id);
            } else {
                // Fallback: colored fill
                renderer_draw_rect(sx + 2, sy + 2, size - 4, size - 4,
                                   bar->slots[i].color_r,
                                   bar->slots[i].color_g,
                                   bar->slots[i].color_b,
                                   0.4f);
            }
        } else {
            renderer_draw_rect(sx, sy, size, size, 0.1f, 0.1f, 0.15f, 0.6f);
        }

        // Slot border
        float border_r = 0.4f, border_g = 0.4f, border_b = 0.5f;
        if (has_ability && bar->slots[i].cooldown_remaining <= 0.0f) {
            border_r = 0.6f; border_g = 0.6f; border_b = 0.7f;
        }
        renderer_draw_rect(sx, sy, size, 2, border_r, border_g, border_b, 1.0f);
        renderer_draw_rect(sx, sy + size - 2, size, 2, border_r, border_g, border_b, 1.0f);
        renderer_draw_rect(sx, sy, 2, size, border_r, border_g, border_b, 1.0f);
        renderer_draw_rect(sx + size - 2, sy, 2, size, border_r, border_g, border_b, 1.0f);

        // Cooldown overlay
        if (has_ability && bar->slots[i].cooldown_remaining > 0.0f) {
            float cd_pct = bar->slots[i].cooldown_remaining / bar->slots[i].cooldown_total;
            if (cd_pct > 1.0f) cd_pct = 1.0f;

            // Dark overlay sweeping down
            float cd_height = size * cd_pct;
            renderer_draw_rect(sx, sy, size, cd_height, 0.0f, 0.0f, 0.0f, 0.6f);

            // Cooldown timer text
            char cd_buf[8];
            snprintf(cd_buf, sizeof(cd_buf), "%.0f", bar->slots[i].cooldown_remaining);
            renderer_draw_text(sx + size / 2 - 5, sy + size / 2 + 5, cd_buf);
        }

        // Not enough mana indicator
        if (has_ability && bar->slots[i].mana_cost > bar->mana &&
            bar->slots[i].mana_cost > 0) {
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
            strncpy(short_name, bar->slots[i].name, 5);
            renderer_draw_text(sx + 2, sy + size - 4, short_name);
        }
    }
}

// ============================================================================
// RENDER — STATUS EFFECTS (buff/debuff icons)
// ============================================================================

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

// ============================================================================
// RENDER — ABILITY CAST BAR
// ============================================================================

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