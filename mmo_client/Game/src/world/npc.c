#include "npc.h"
#include "renderer.h"

#include <stdio.h>

void npc_render(const VisibleNPC* npc, int tile_size) {
    if (!npc->is_alive) return;

    int npc_size = tile_size * 2;
    float x = npc->pos_x;
    float y = npc->pos_y;

    // Category-based body color
    // 0 = passive  (calm blue-green)
    // 1 = hostile  (danger red)
    // 2 = quest    (golden yellow)
    float cr, cg, cb;
    switch (npc->category) {
        case 1:  cr = 0.85f; cg = 0.20f; cb = 0.20f; break; // hostile - red
        case 2:  cr = 0.90f; cg = 0.75f; cb = 0.10f; break; // quest   - gold
        default: cr = 0.20f; cg = 0.55f; cb = 0.80f; break; // passive - blue
    }

    // Thin dark outline for legibility
    float half = (float)npc_size / 2.0f;
    renderer_draw_rect(x - half - 1, y - half - 1,
                       (float)npc_size + 2, (float)npc_size + 2,
                       0.0f, 0.0f, 0.0f, 0.6f);

    // Body fill
    renderer_draw_rect(x - half, y - half,
                       (float)npc_size, (float)npc_size,
                       cr, cg, cb, 1.0f);

    // Health bar (sits just above the body)
    float bar_width  = 40.0f;
    float bar_height = 4.0f;
    float bar_x = x - bar_width / 2.0f;
    float bar_y = y - half - 10.0f;

    renderer_draw_rect(bar_x, bar_y, bar_width, bar_height, 0.15f, 0.15f, 0.15f, 0.9f);
    if (npc->max_health > 0) {
        float pct = (float)npc->health / (float)npc->max_health;
        // Color shifts red->yellow->green based on health
        float hr = (pct < 0.5f) ? 1.0f : (2.0f - pct * 2.0f);
        float hg = (pct > 0.5f) ? 1.0f : (pct * 2.0f);
        renderer_draw_rect(bar_x, bar_y, bar_width * pct, bar_height, hr, hg, 0.05f, 1.0f);
    }

    // Name label (centered above health bar)
    if (npc->name[0] != '\0') {
        renderer_draw_text(x - 20.0f, bar_y - 2.0f, npc->name);
    }

    // Interactable indicator: small "!" badge above the name for quest givers
    if (npc->is_interactable && npc->category == 2) {
        renderer_draw_rect(x - 5.0f, bar_y - 20.0f, 10.0f, 16.0f, 0.9f, 0.75f, 0.1f, 0.9f);
        renderer_draw_text(x - 2.5f, bar_y - 6.0f, "!");
    }
}

void npc_render_all(const VisibleNPC* npcs, int count, int tile_size) {
    for (int i = 0; i < count; i++) {
        npc_render(&npcs[i], tile_size);
    }
}

const VisibleNPC* npc_find_by_id(const VisibleNPC* npcs, int count, uint32_t npc_id) {
    for (int i = 0; i < count; i++) {
        if (npcs[i].npc_id == npc_id) {
            return &npcs[i];
        }
    }
    return NULL;
}

int npc_get_position(const VisibleNPC* npcs, int count, uint32_t npc_id,
                     float* out_x, float* out_y) {
    const VisibleNPC* npc = npc_find_by_id(npcs, count, npc_id);
    if (npc) {
        *out_x = npc->pos_x;
        *out_y = npc->pos_y;
        return 1;
    }
    return 0;
}