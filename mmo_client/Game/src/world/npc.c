/**
 * @file
 * Locate and render NPCs visible to the MMO client.
 */
#include "npc.h"
#include "renderer.h"
#include "camera/camera.h"
#include "render/character_renderer.h"
#include "render/character_tuning.h"
#include "world/npc_types.h"
#include "player/emote.h"
#include "ui/npc_dialogue.h"
#include "ui/quest_tracker.h"

#include <stdio.h>
#include <math.h>

#define NPC_CATEGORY_HOSTILE 1
#define NPC_CATEGORY_QUEST   2

/** A swarm critter still has to be visible and clickable. */
#define NPC_MIN_SCALE 0.35f

/** Radius of the body for rings and clicks, at scale 1, in world units. */
#define NPC_BODY_RADIUS 14.0f

static float npc_scale(const VisibleNPC* npc) {
    float rgb[3], scale = 1.0f;
    if (!npc_type_get_style(npc->npc_type_id, rgb, &scale)) 
        scale = 1.0f;

    return scale < NPC_MIN_SCALE ? NPC_MIN_SCALE : scale;
}

float npc_body_height(const VisibleNPC* npc) {
    return CHARACTER_HEIGHT * npc_scale(npc);
}

/**
 * Body colour. Faction picks the hue and role the value, both resolved by the
 * generator into the display table, so the client never decides what an enemy
 * looks like. Category is the fallback for a type the table does not carry,
 * and red/gold/blue keeps those legible.
 */
static void npc_tint(const VisibleNPC* npc, float out[3]) {
    float scale;
    if (npc_type_get_style(npc->npc_type_id, out, &scale)) return;
    switch (npc->category) {
        case NPC_CATEGORY_HOSTILE: 
            out[0] = 0.85f; out[1] = 0.20f; out[2] = 0.20f; 
            break;
        case NPC_CATEGORY_QUEST:   
            out[0] = 0.90f; out[1] = 0.75f; out[2] = 0.10f; 
            break;
        default:                   
            out[0] = 0.20f; out[1] = 0.55f; out[2] = 0.80f; 
            break;
    }
}

// A seated shopkeeper's head is lower than a standing body's.
#define NPC_SEATED_HEIGHT_FRACTION 0.7f

static int npc_is_seated(const VisibleNPC* npc) {
	emote_pose pose;
	CharacterAnim clip;

	return emote_keeper_pose(npc->pos_x, npc->pos_y, &pose, &clip);
}

// A shopkeeper on its seat is drawn sitting there, facing out over the counter.
static void npc_apply_seat(const VisibleNPC* npc, float* x, float* y, float* z,
                           CharacterStyle* style) {
	emote_pose pose;
	CharacterAnim clip;

	if(!emote_keeper_pose(npc->pos_x, npc->pos_y, &pose, &clip))
		return;

	*x = pose.x;
	*y = pose.y;
	*z = pose.z;
	style->hints.has_look = 0;
	style->hints.talking = 0;
	style->hints.has_pose = 1;
	style->hints.pose = clip;
	style->hints.has_facing = 1;
	style->hints.facing_yaw = pose.heading;
}

void npc_render_ground_marks(const VisibleNPC* npcs, int count, uint32_t target_npc_id) {
    for (int i = 0; i < count; i++) {
        const VisibleNPC* npc = &npcs[i];
        if (!npc->is_alive) continue;
        float r = NPC_BODY_RADIUS * npc_scale(npc);

        /* Elites and mini-bosses keep the one cue that survives at a distance:
         * "this one is different". Once a box outline, now a ring at the feet. */
        float outline[3];
        renderer_lift_begin(npc->pos_z);
        if (npc_type_get_outline(npc->npc_type_id, outline))
            renderer_draw_ring(npc->pos_x, npc->pos_y, r + 2.0f, r + 5.0f,
                               outline[0], outline[1], outline[2], 0.85f, 32);

        if (npc->npc_id == target_npc_id)
            renderer_draw_ring(npc->pos_x, npc->pos_y, r + 6.0f, r + 9.0f,
                               1.0f, 0.9f, 0.1f, 0.95f, 32);
        renderer_lift_end();
    }
}

void npc_render_bodies(const VisibleNPC* npcs, int count, float player_x, float player_y) {
    uint32_t talking_to = dialogue_is_active() ? dialogue_get_current_npc() : 0;

    for (int i = 0; i < count; i++) {
        const VisibleNPC* npc = &npcs[i];
        if (!npc->is_alive) 
            continue;

        /* visual_y_offset is a jump, negative upward (the slime's slam). */
        CharacterStyle style = { { 0 }, npc_scale(npc), -npc->visual_y_offset, { 0 }, NULL, 0, 1 };
        npc_tint(npc, style.tint);
		// only players play the idle animation; an NPC standing about holds still
		style.hints.still_idle = 1;

        /* People who give quests notice the player walking up, and talk while
         * the player is reading them. Hostiles face the way they move. */
        if (npc->category != NPC_CATEGORY_HOSTILE) {
            float dx = player_x - npc->pos_x, dy = player_y - npc->pos_y;
            if (dx * dx + dy * dy < CHARACTER_NOTICE_RADIUS * CHARACTER_NOTICE_RADIUS) {
                style.hints.has_look = 1;
                style.hints.look_x   = player_x;
                style.hints.look_y   = player_y;
            }
            style.hints.talking = (talking_to != 0 && npc->npc_id == talking_to);
        }

		float x = npc->pos_x, y = npc->pos_y, z = npc->pos_z;

		npc_apply_seat(npc, &x, &y, &z, &style);
		character_renderer_draw(CHARACTER_KIND_NPC, npc->npc_id, x, y, z, &style);
    }
}

/**
 * Draw one NPC's bars and badges on a card whose point (x, y) is the top of
 * its head.
 */
static void npc_render_label(const VisibleNPC* npc, uint32_t tracked_npc_type) {
    float x = npc->pos_x;
    float y = npc->pos_y;

    float bar_width  = 40.0f;
    float bar_height = 4.0f;
    float bar_x = x - bar_width / 2.0f;
    float bar_y = y - 10.0f;

    renderer_draw_rect(bar_x, bar_y, bar_width, bar_height, 0.15f, 0.15f, 0.15f, 0.9f);
    if (npc->max_health > 0) {
        float pct = (float)npc->health / (float)npc->max_health;
        // Colour shifts red->yellow->green with health
        float hr = (pct < 0.5f) ? 1.0f : (2.0f - pct * 2.0f);
        float hg = (pct > 0.5f) ? 1.0f : (pct * 2.0f);
        renderer_draw_rect(bar_x, bar_y, bar_width * pct, bar_height, hr, hg, 0.05f, 1.0f);
    }

    if (npc->name[0] != '\0') {
        float label_w = 160.0f;
        renderer_draw_text_centered(x - label_w / 2.0f, bar_y - 16.0f, label_w, 0.0f, npc->name);
    }

    /* The tracked objective's target gets the loud badge; any other quest giver
     * gets the quiet one, so "somebody here has work" and "this is your next
     * step" do not look the same. */
    if (tracked_npc_type != 0 && npc->npc_type_id == tracked_npc_type) {
        quest_tracker_render_world_badge(x, bar_y - 28.0f);
    } else if (npc->is_interactable && npc->category == NPC_CATEGORY_QUEST) {
        renderer_draw_rect(x - 4.0f, bar_y - 36.0f, 8.0f, 13.0f, 0.62f, 0.55f, 0.20f, 0.75f);
    }
}

void npc_render_labels(const VisibleNPC* npcs, int count, uint32_t tracked_npc_type) {
    for (int i = 0; i < count; i++) {
        const VisibleNPC* npc = &npcs[i];
        if (!npc->is_alive) continue;
        if (!character_renderer_in_label_range(npc->pos_x, npc->pos_y)) continue;   /* too far for a plate */
		float height = npc_body_height(npc);

		if(npc_is_seated(npc))
			height *= NPC_SEATED_HEIGHT_FRACTION;

        camera_billboard_begin(npc->pos_x, npc->pos_y, 0.0f,
                               npc->pos_z + height - npc->visual_y_offset);
        npc_render_label(npc, tracked_npc_type);
        camera_billboard_end();
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
