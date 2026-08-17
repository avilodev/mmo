/**
 * @file
 * Locate and render NPCs visible to the MMO client.
 */
#include "npc.h"
#include "renderer.h"

#include <stdio.h>
#include <math.h>

/**
 * Render one living NPC with its category styling and health display.
 */
void npc_render(const VisibleNPC* npc, int tile_size) {
    if (!npc->is_alive) return;

    int npc_size = tile_size * 2;
    float x = npc->pos_x;
    float y = npc->pos_y + npc->visual_y_offset;

    // Kingdom Slime — unique green blob appearance
    if (npc->npc_type_id == 5) {
        float sw = (float)npc_size * 1.4f;
        float sh = (float)npc_size * 0.75f;
        float half_w = sw / 2.0f;
        float half_h = sh / 2.0f;

        // Outline
        renderer_draw_rect(x - half_w - 1, y - half_h - 1,
                           sw + 2, sh + 2, 0.0f, 0.0f, 0.0f, 0.6f);
        // Body — lime green
        renderer_draw_rect(x - half_w, y - half_h, sw, sh, 0.15f, 0.80f, 0.25f, 1.0f);
        // Darker highlight on top
        renderer_draw_rect(x - half_w * 0.6f, y - half_h + 2.0f,
                           sw * 0.6f, sh * 0.3f, 0.3f, 1.0f, 0.45f, 0.5f);
        // Eyes
        renderer_draw_rect(x - half_w * 0.35f - 3, y - 2.0f, 5, 5, 0.05f, 0.1f, 0.05f, 1.0f);
        renderer_draw_rect(x + half_w * 0.35f - 2, y - 2.0f, 5, 5, 0.05f, 0.1f, 0.05f, 1.0f);

        // Health bar
        float bar_width  = 44.0f;
        float bar_height = 4.0f;
        float bar_x = x - bar_width / 2.0f;
        float bar_y = y - half_h - 10.0f;
        renderer_draw_rect(bar_x, bar_y, bar_width, bar_height, 0.15f, 0.15f, 0.15f, 0.9f);
        if (npc->max_health > 0) {
            float pct = (float)npc->health / (float)npc->max_health;
            float hr = (pct < 0.5f) ? 1.0f : (2.0f - pct * 2.0f);
            float hg = (pct > 0.5f) ? 1.0f : (pct * 2.0f);
            renderer_draw_rect(bar_x, bar_y, bar_width * pct, bar_height, hr, hg, 0.05f, 1.0f);
        }
        if (npc->name[0] != '\0') {
            renderer_draw_text(x - 24.0f, bar_y - 2.0f, npc->name);
        }
        return;
    }

    // category selects passive, hostile, or quest coloring
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

/**
 * Render every NPC in a visible-NPC array.
 */
void npc_render_all(const VisibleNPC* npcs, int count, int tile_size) {
    for (int i = 0; i < count; i++) {
        npc_render(&npcs[i], tile_size);
    }
}

/**
 * Find a visible NPC by its server identifier.
 *
 * @return      A pointer into the supplied array, or NULL when no entry matches.
 */
const VisibleNPC* npc_find_by_id(const VisibleNPC* npcs, int count, uint32_t npc_id) {
    for (int i = 0; i < count; i++) {
        if (npcs[i].npc_id == npc_id) {
            return &npcs[i];
        }
    }
    return NULL;
}

/**
 * Copy a visible NPC's world position into caller-provided outputs.
 *
 * @return      Nonzero when the NPC is found, otherwise zero.
 */
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

/**
 * Render selection brackets around the targeted living NPC.
 */
void npc_render_target_indicator(const VisibleNPC* npcs, int count, int tile_size,
                                  uint32_t target_npc_id) {
    if (target_npc_id == 0) return;

    const VisibleNPC* npc = npc_find_by_id(npcs, count, target_npc_id);
    if (!npc || !npc->is_alive) return;

    float half = (float)(tile_size * 2) / 2.0f;
    float x    = npc->pos_x;
    float y    = npc->pos_y;
    float t    = 2.5f; // bracket thickness

    // Draw four corner brackets in yellow as a selection indicator
    float bsize = half + 6.0f; // slightly larger than NPC body
    float blen  = 8.0f;        // length of each bracket arm

    // Top-left
    renderer_draw_rect(x - bsize,        y - bsize,        blen, t,    1.0f, 0.9f, 0.1f, 1.0f);
    renderer_draw_rect(x - bsize,        y - bsize,        t,    blen, 1.0f, 0.9f, 0.1f, 1.0f);
    // Top-right
    renderer_draw_rect(x + bsize - blen, y - bsize,        blen, t,    1.0f, 0.9f, 0.1f, 1.0f);
    renderer_draw_rect(x + bsize - t,    y - bsize,        t,    blen, 1.0f, 0.9f, 0.1f, 1.0f);
    // Bottom-left
    renderer_draw_rect(x - bsize,        y + bsize - t,    blen, t,    1.0f, 0.9f, 0.1f, 1.0f);
    renderer_draw_rect(x - bsize,        y + bsize - blen, t,    blen, 1.0f, 0.9f, 0.1f, 1.0f);
    // Bottom-right
    renderer_draw_rect(x + bsize - blen, y + bsize - t,    blen, t,    1.0f, 0.9f, 0.1f, 1.0f);
    renderer_draw_rect(x + bsize - t,    y + bsize - blen, t,    blen, 1.0f, 0.9f, 0.1f, 1.0f);
}
