/**
 * @file
 * Draw the world-space pass, and run the frame's drawing order.
 *
 * Two things live here: the entities drawn inside the camera transform -- the
 * ones whose position is a world position, standing in 3D -- and
 * playing_render(), which is the running order of the whole frame. The screen-space overlays it calls
 * into are in state_playing_hud.c; keeping the order here and the panels there
 * means changing what a panel looks like and changing what it sits on top of
 * are edits to different files.
 */
#include "states/state_playing_internal.h"
#include "ui/friends_panel.h"
#include "core/race_registry.h"
#include "renderer.h"
#include "npc.h"
#include "player.h"
#include "combat_render.h"
#include "hud.h"
#include "inventory.h"
#include "character_screen.h"
#include "ui/npc_dialogue.h"
#include "ui/quest_tracker.h"
#include "ui/shop_ui.h"

#include "camera/camera_tuning.h"
#include "render/character_renderer.h"
#include "render/city_renderer.h"
#include "render/character_tuning.h"
#include "render/world_light.h"

#include <math.h>

/** A player's body colour: their race's, from the registry the server sent. */
static void player_tint(uint32_t race_id, float out[3]) {
    client_race_color(race_id, &out[0], &out[1], &out[2]);
}

/**
 * Stand-in body when the character model did not load: a coloured card, so a
 * missing asset is a visible placeholder, not an invisible player.
 */
static void render_fallback_body(float x, float y, const float rgb[3]) {
    const float w = 20.0f, h = CHARACTER_HEIGHT;
    camera_billboard_begin(x, y, 0.0f, 0.0f);
    renderer_draw_rect(x - w / 2.0f, y - h, w, h, rgb[0], rgb[1], rgb[2], 1.0f);
    camera_billboard_end();
}

static void render_player_body(CharacterKind kind, uint32_t id, float x, float y,
                               uint32_t race_id, const CharacterHints* hints) {
    CharacterStyle style = { { 0 }, 1.0f, 0.0f, { 0 } };
    player_tint(race_id, style.tint);
    if (hints) style.hints = *hints;
    if (character_renderer_ready())
        character_renderer_draw(kind, id, x, y, &style);
    else
        render_fallback_body(x, y, style.tint);
}

/**
 * Draw the 3D bodies of the local player and everyone nearby. Called between
 * character_renderer_begin() and character_renderer_end().
 */
static void playing_render_player_bodies(GameState* game) {
    for (int i = 0; i < game->playing->nearby_player_count; i++) {
        const NearbyPlayer* p = &game->playing->nearby_players[i];
        if (p->is_dead) continue;
        render_player_body(CHARACTER_KIND_PLAYER, p->player_id, p->pos_x, p->pos_y,
                           p->player_race, NULL);
    }

    /* The local body is told which way it faces and whether it is pivoting,
     * straight from its locomotion, rather than guessing from its path. */
    CharacterHints own = { 0 };
    own.has_facing = 1;
    own.facing_yaw = game->player.loco.heading;
    own.pivoting   = game->player.loco.pivoting;
    render_player_body(CHARACTER_KIND_LOCAL, 0, game->player.x, game->player.y,
                       game->player.info_loaded ? game->player.info.race_id : 0, &own);
}

/**
 * Draw "level - name", and a health bar when one is given, on a card whose
 * point (x, y) is the top of the head.
 */
static void render_player_label(float x, float y, unsigned level, const char* name,
                                int32_t health, int32_t max_health, int show_bar) {
    if (character_renderer_fog_at(x, y) > 0.5f) return;   /* lost in the fog */
    camera_billboard_begin(x, y, 0.0f, CHARACTER_HEIGHT);

    float label_y = y - 18.0f;
    if (show_bar) {
        float bar_w = 40.0f, bar_h = 4.0f;
        float bar_x = x - bar_w / 2.0f;
        float bar_y = y - 10.0f;
        renderer_draw_rect(bar_x, bar_y, bar_w, bar_h, 0.2f, 0.2f, 0.2f, 1.0f);
        if (max_health > 0) {
            float pct = (float)health / (float)max_health;
            renderer_draw_rect(bar_x, bar_y, bar_w * pct, bar_h, 0.2f, 0.8f, 0.2f, 1.0f);
        }
        label_y = bar_y - 18.0f;
    }

    if (name && name[0] != '\0') {
        char label[48];
        snprintf(label, sizeof(label), "%u - %s", level, name);
        float label_w = 160.0f;
        renderer_draw_text_centered(x - label_w / 2.0f, label_y, label_w, 0.0f, label);
    }

    camera_billboard_end();
}

/** Name plates for everyone nearby (with health) and for the local player. */
static void playing_render_player_labels(GameState* game) {
    for (int i = 0; i < game->playing->nearby_player_count; i++) {
        const NearbyPlayer* p = &game->playing->nearby_players[i];
        if (p->is_dead) continue;
        render_player_label(p->pos_x, p->pos_y, p->level, p->name,
                            p->health, p->max_health, 1);
    }
    if (game->player.info_loaded)
        render_player_label(game->player.x, game->player.y, game->player.info.level,
                            game->player.info.name, 0, 0, 0);
}

/** Shadows under every player, and the selection ring under a targeted one. */
static void playing_render_player_ground_marks(GameState* game) {
    uint32_t target = game->playing->target_player_id;
    for (int i = 0; i < game->playing->nearby_player_count; i++) {
        const NearbyPlayer* p = &game->playing->nearby_players[i];
        if (p->is_dead) continue;
        character_draw_shadow(p->pos_x, p->pos_y, 1.0f);
        if (p->player_id == target)
            renderer_draw_ring(p->pos_x, p->pos_y, 20.0f, 23.0f, 1.0f, 0.9f, 0.1f, 0.95f, 32);
    }
    character_draw_shadow(game->player.x, game->player.y, 1.0f);
}

void playing_render_projectiles(GameState* game) {
    for (int i = 0; i < MAX_VISIBLE_PROJECTILES; i++) {
        if (!game->playing->projectiles[i].active) continue;
        VisibleProjectile* proj = &game->playing->projectiles[i];
        /* In flight at chest height, not skidding along the ground. */
        camera_billboard_begin(proj->pos_x, proj->pos_y, 0.0f, CAMERA_PROJECTILE_HEIGHT);
        renderer_draw_rect(proj->pos_x - 3, proj->pos_y - 3,
                          6.0f, 6.0f, 1.0f, 0.8f, 0.2f, 1.0f);
        camera_billboard_end();
    }
}

/**
 * Draw active NPC cast telegraphs with shape-specific geometry.
 */
void playing_render_telegraphs(GameState* game) {
    for (int i = 0; i < MAX_TELEGRAPHS; i++) {
        if (!game->playing->telegraphs[i].active) continue;
        VisibleTelegraph* t = &game->playing->telegraphs[i];

        // Progress: 0.0 to 1.0 (more opaque as cast completes)
        float progress = (t->cast_time > 0) ? t->elapsed / t->cast_time : 1.0f;
        float alpha = 0.15f + progress * 0.25f;

        switch (t->shape) {
            case 0: // Circle
                renderer_draw_circle(t->pos_x, t->pos_y, t->radius,
                                    1.0f, 0.2f, 0.2f, alpha, 24);
                break;
            case 1: // Cone
                renderer_draw_cone(t->pos_x, t->pos_y, t->dir_x, t->dir_y,
                                  t->radius, t->angle,
                                  1.0f, 0.5f, 0.1f, alpha, 16);
                break;
            case 2: // Rectangle
            {
                float dx = t->dir_x, dy = t->dir_y;
                float len = sqrtf(dx*dx + dy*dy);
                if (len > 0.001f) { dx /= len; dy /= len; }
                else { dx = 1.0f; dy = 0.0f; }
                // Perpendicular
                float px = -dy, py = dx;
                float hw = t->width / 2.0f;
                float hl = t->length;

                glDisable(GL_TEXTURE_2D);
                glColor4f(1.0f, 0.3f, 0.1f, alpha);
                glBegin(GL_QUADS);
                glVertex2f(t->pos_x - px*hw, t->pos_y - py*hw);
                glVertex2f(t->pos_x + px*hw, t->pos_y + py*hw);
                glVertex2f(t->pos_x + dx*hl + px*hw, t->pos_y + dy*hl + py*hw);
                glVertex2f(t->pos_x + dx*hl - px*hw, t->pos_y + dy*hl - py*hw);
                glEnd();
                glEnable(GL_TEXTURE_2D);
                break;
            }
            case 3: // Line (thin rectangle)
            {
                float dx = t->dir_x, dy = t->dir_y;
                float len = sqrtf(dx*dx + dy*dy);
                if (len > 0.001f) { dx /= len; dy /= len; }
                else { dx = 1.0f; dy = 0.0f; }
                float px = -dy, py = dx;
                float hw = 4.0f; // thin line

                glDisable(GL_TEXTURE_2D);
                glColor4f(1.0f, 0.1f, 0.1f, alpha);
                glBegin(GL_QUADS);
                glVertex2f(t->pos_x - px*hw, t->pos_y - py*hw);
                glVertex2f(t->pos_x + px*hw, t->pos_y + py*hw);
                glVertex2f(t->pos_x + dx*t->length + px*hw, t->pos_y + dy*t->length + py*hw);
                glVertex2f(t->pos_x + dx*t->length - px*hw, t->pos_y + dy*t->length - py*hw);
                glEnd();
                glEnable(GL_TEXTURE_2D);
                break;
            }
        }
    }
}

void playing_render_zones(GameState* game) {
    for (int i = 0; i < MAX_ZONES; i++) {
        if (!game->playing->zones[i].active) continue;
        VisibleZone* z = &game->playing->zones[i];
        renderer_draw_circle(z->pos_x, z->pos_y, z->radius,
                            0.3f, 0.8f, 0.3f, 0.2f, 24);
    }
}

/**
 * Draw expanding, fading rings for active healing effects.
 */
void playing_render_heal_vfxs(GameState* game) {
    for (int i = 0; i < MAX_HEAL_VFXS; i++) {
        HealVFX* vfx = &game->playing->heal_vfxs[i];
        if (!vfx->active) continue;

        float t = vfx->age / vfx->duration;
        if (t > 1.0f) t = 1.0f;

        // Outer expanding ring fades out
        float outer_r = vfx->max_radius * t;
        float outer_a = (1.0f - t) * 0.7f;
        renderer_draw_circle(vfx->pos_x, vfx->pos_y, outer_r,
                             0.15f, 1.0f, 0.35f, outer_a, 32);

        // Second inner ring slightly delayed
        if (t > 0.15f) {
            float t2 = (t - 0.15f) / 0.85f;
            float inner_r = vfx->max_radius * 0.6f * t2;
            float inner_a = (1.0f - t2) * 0.45f;
            renderer_draw_circle(vfx->pos_x, vfx->pos_y, inner_r,
                                 0.3f, 1.0f, 0.5f, inner_a, 24);
        }
    }
}

/**
 * Draw active ground-item markers.
 */
void playing_render_ground_items(GameState* game) {
    for (int i = 0; i < MAX_GROUND_ITEMS; i++) {
        if (!game->playing->ground_items[i].active) continue;
        GroundItem* item = &game->playing->ground_items[i];
        // Sparkle border (drawn first, behind item)
        renderer_draw_rect(item->pos_x - 7, item->pos_y - 7,
                          14.0f, 14.0f, 1.0f, 0.9f, 0.3f, 0.4f);
        // Gold/yellow square (on top)
        renderer_draw_rect(item->pos_x - 6, item->pos_y - 6,
                          12.0f, 12.0f, 0.9f, 0.8f, 0.1f, 1.0f);
    }
}

/**
 * Render the ordered world layers, entities, HUD, panels, and overlays.
 */
void playing_render(GameState* game) {
    /* The sky: what the fog fades the world into, when the camera looks out. */
    renderer_clear(WORLD_SKY_R, WORLD_SKY_G, WORLD_SKY_B);
    character_renderer_set_focus(game->camera.x, game->camera.y);
    renderer_begin_2d();
    camera_apply(&game->camera);

    /* The world is the authored city scene alone (D40, D41): world.dat's
     * ground and the structures grown from it are no longer drawn, though its
     * collision still decides where anyone can walk. The city goes before the
     * ground marks, so shadows, rings and telegraphs still show on its paving;
     * the catch is that they also show through its walls. */
    CameraView view;
    camera_get_view(&game->camera, &view);
    city_renderer_draw(&view, game->camera.x, game->camera.y);

    playing_render_zones(game);
    playing_render_heal_vfxs(game);
    playing_render_telegraphs(game);
    playing_render_ground_items(game);
    combat_render_indicator(&game->playing->combat);
    npc_render_ground_marks(game->playing->visible_npcs, game->playing->visible_npc_count,
                            game->playing->target_npc_id);
    playing_render_player_ground_marks(game);

    /* The characters, depth-tested against the city. */
    character_renderer_begin(&view, game->camera.x, game->camera.y, game->playing->frame_dt);
    npc_render_bodies(game->playing->visible_npcs, game->playing->visible_npc_count,
                      game->player.x, game->player.y);
    playing_render_player_bodies(game);
    character_renderer_end();

    /* Cards above heads, projectiles and damage numbers: screen-aligned, and
     * still hidden behind a building that stands in front of them. */
    /* One lookup for the whole pass: the badge and the map marker must agree
     * about which NPC the player is being sent to. */
    QuestTrackedStep tracked = quest_log_tracked_step(&game->playing->quest_log);
    uint32_t tracked_npc_type =
        (tracked.valid && tracked.objective_type != QUEST_OBJECTIVE_COLLECT)
            ? tracked.target_id : 0;

    npc_render_labels(game->playing->visible_npcs, game->playing->visible_npc_count,
                      tracked_npc_type);
    playing_render_player_labels(game);
    playing_render_projectiles(game);
    combat_render_damage_numbers(&game->playing->combat);

    renderer_end_2d();

    // Full map overlay (before HUD so it sits on top of world but under chat/panels)
    playing_render_big_map(game);

    // Screen-space UI
    hud_render(&game->playing->hud, game);
    ability_bar_render(&game->playing->ability_bar);
    ability_bar_render_effects(&game->playing->ability_bar,
                               game->playing->ability_bar.bar_x,
                               game->playing->ability_bar.bar_y - 40.0f);
    ability_bar_render_cast_bar(&game->playing->ability_bar,
                                (float)game->camera.viewport_width,
                                (float)game->camera.viewport_height);
    combat_render_cast_bar(&game->playing->combat,
                            (float)game->camera.viewport_width,
                            (float)game->camera.viewport_height);

    // Render inventory and character screen (on top of everything)
    if (game->inventory) {
        inventory_render(game->inventory);
    }

    if (game->character_screen) {
        character_screen_render(game->character_screen, game);
    }

    // Render dialogue (on top of everything)
    dialogue_render(game->camera.viewport_width, game->camera.viewport_height);

    // Chat, party, death, level-up overlays
    playing_render_chat(game);
    playing_render_party_frames(game);
    playing_render_party_invite(game);
    playing_render_death_screen(game);
    playing_render_level_up(game);
    playing_render_reward_notifications(game);

    // Quest log panel (before overlays)
    quest_log_render(&game->playing->quest_log,
                     game->camera.viewport_width,
                     game->camera.viewport_height);

    /* The tracked step, always on screen. Drawn after the log so opening the
     * log does not hide the thing the log is pointing at. */
    quest_tracker_render_panel(&game->playing->quest_log,
                               game->camera.viewport_width,
                               game->camera.viewport_height);

    /* Currency panel. The highlighted row is the coin of whichever kingdom the
     * player is standing closest to, which is what local trade pays in. */
    currency_panel_render(&game->playing->currency_panel,
                          game->player.info.currency,
                          (int)world_local_currency_px(game->player.x,
                                                       game->player.y),
                          game->camera.viewport_width,
                          game->camera.viewport_height);

    /* Friends panel. Above the currency panel and below the shop, matching the
     * order the input layer checks them in -- whichever draws last is the one
     * a click reaches first. */
    friends_panel_render(&game->playing->friends,
                         game->camera.viewport_width,
                         game->camera.viewport_height);

    // Shop window (on top of game world, below pause)
    shop_ui_render(game);

    // Buff icon tooltip (before pause/settings overlays)
    playing_render_buff_tooltip(game);

    // Pause overlay
    playing_render_pause_overlay(game);

    // In-game settings overlay (on top of pause if both somehow active)
    playing_render_settings_overlay(game);

    const AbilityBarState* bar = &game->playing->ability_bar;
    char debug[160];
    if (ability_bar_has_resource(bar)) {
        snprintf(debug, sizeof(debug), "Pos: %.0f, %.0f  %s  Resource: %d/%d",
                 game->player.x, game->player.y,
                 bar->active_form == FORM_ANIMAL ? "Animal" : "Human",
                 bar->resource, bar->max_resource);
    } else {
        /* Human Form is cooldown-only, so there is no pool to report. */
        snprintf(debug, sizeof(debug), "Pos: %.0f, %.0f  Human (no resource)",
                 game->player.x, game->player.y);
    }
    renderer_draw_text(10, 20, debug);
}
