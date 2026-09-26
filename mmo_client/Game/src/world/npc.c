/**
 * @file
 * Locate and render NPCs visible to the MMO client.
 */
#include "npc.h"
#include "renderer.h"
#include "camera/camera.h"
#include "world/npc_types.h"
#include "ui/quest_tracker.h"

#include <stdio.h>
#include <math.h>

/** The Kingdom Slime draws as a wide, low blob rather than a box. */
#define NPC_TYPE_KINGDOM_SLIME 5

float npc_body_half_height(const VisibleNPC* npc, int tile_size) {
    float npc_size = (float)(tile_size * 2);
    if (npc->npc_type_id == NPC_TYPE_KINGDOM_SLIME) return npc_size * 0.75f / 2.0f;

    float style_rgb[3];
    float size_scale = 1.0f;
    if (!npc_type_get_style(npc->npc_type_id, style_rgb, &size_scale)) size_scale = 1.0f;
    int scaled = (int)(npc_size * size_scale);
    if (scaled < 4) scaled = 4;
    return (float)scaled / 2.0f;
}

/**
 * Draw one NPC's body, bars and badges in its own 2D coordinates.
 */
static void npc_render_card(const VisibleNPC* npc, int tile_size, uint32_t tracked_npc_type) {
    int npc_size = tile_size * 2;
    float x = npc->pos_x;
    float y = npc->pos_y + npc->visual_y_offset;

    // Kingdom Slime — unique green blob appearance
    if (npc->npc_type_id == NPC_TYPE_KINGDOM_SLIME) {
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
        if (tracked_npc_type != 0 && npc->npc_type_id == tracked_npc_type)
            quest_tracker_render_world_badge(x, bar_y - 20.0f);
        return;
    }

    /* Faction picks the hue, role picks the value and the box size -- both
     * resolved by the generator and read from the display table, so the client
     * never decides what an enemy looks like.
     *
     * Category is the fallback, not the default: a type the table does not carry
     * is either a client older than the content it is talking to or an NPC added
     * since the table was generated, and red/gold/blue keeps both legible. */
    float cr, cg, cb;
    float style_rgb[3];
    float size_scale = 1.0f;

    if (npc_type_get_style(npc->npc_type_id, style_rgb, &size_scale)) {
        cr = style_rgb[0];
        cg = style_rgb[1];
        cb = style_rgb[2];
    } else {
        switch (npc->category) {
            case 1:  cr = 0.85f; cg = 0.20f; cb = 0.20f; break; // hostile - red
            case 2:  cr = 0.90f; cg = 0.75f; cb = 0.10f; break; // quest   - gold
            default: cr = 0.20f; cg = 0.55f; cb = 0.80f; break; // passive - blue
        }
    }

    npc_size = (int)((float)npc_size * size_scale);
    if (npc_size < 4) npc_size = 4;   // a swarm critter still has to be clickable

    // Thin dark outline for legibility
    float half = (float)npc_size / 2.0f;
    renderer_draw_rect(x - half - 1, y - half - 1,
                       (float)npc_size + 2, (float)npc_size + 2,
                       0.0f, 0.0f, 0.0f, 0.6f);

    /* Elites and mini-bosses carry a second, brighter outline. It is the one cue
     * that survives at a distance with no art, which is why the scheme spends it
     * on "this one is different" rather than on anything a player can read up
     * close anyway. */
    float outline_rgb[3];
    if (npc_type_get_outline(npc->npc_type_id, outline_rgb)) {
        renderer_draw_rect(x - half - 3, y - half - 3,
                           (float)npc_size + 6, (float)npc_size + 6,
                           outline_rgb[0], outline_rgb[1], outline_rgb[2], 0.9f);
    }

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

    /* The tracked objective's target gets the loud badge; any other quest giver
     * gets the quiet one, so "somebody here has work" and "this is your next
     * step" do not look the same. */
    if (tracked_npc_type != 0 && npc->npc_type_id == tracked_npc_type) {
        quest_tracker_render_world_badge(x, bar_y - 12.0f);
    } else if (npc->is_interactable && npc->category == 2) {
        renderer_draw_rect(x - 4.0f, bar_y - 18.0f, 8.0f, 13.0f, 0.62f, 0.55f, 0.20f, 0.75f);
    }
}

/**
 * Render one living NPC with its category styling and health display, standing
 * on its position in the 3D view.
 */
void npc_render(const VisibleNPC* npc, int tile_size, uint32_t tracked_npc_type) {
    if (!npc->is_alive) return;
    camera_billboard_begin(npc->pos_x, npc->pos_y, npc_body_half_height(npc, tile_size), 0.0f);
    npc_render_card(npc, tile_size, tracked_npc_type);
    camera_billboard_end();
}

/**
 * Render every NPC in a visible-NPC array.
 */
void npc_render_all(const VisibleNPC* npcs, int count, int tile_size,
                    uint32_t tracked_npc_type) {
    for (int i = 0; i < count; i++) {
        npc_render(&npcs[i], tile_size, tracked_npc_type);
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

    camera_billboard_begin(x, y, npc_body_half_height(npc, tile_size), 0.0f);

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

    camera_billboard_end();
}
