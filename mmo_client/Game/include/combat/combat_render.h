#ifndef COMBAT_RENDER_H
#define COMBAT_RENDER_H

#include "combat_state.h"

// Draw the attack indicator (world space)
void combat_render_indicator(const CombatState* combat);

// Draw floating damage numbers (world space)
void combat_render_damage_numbers(CombatState* combat);

// Draw cast bar (screen space)
void combat_render_cast_bar(const CombatState* combat, float screen_width, float screen_height);

// Draw cooldown overlay on ability icon (screen space)
void combat_render_cooldown(const CombatState* combat, float x, float y, float size);

#endif // COMBAT_RENDER_H