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
#include "ui/emote_hub.h"
#include "ui/weather_window.h"
#include "player/emote_slots.h"
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
#include "render/sky_renderer.h"
#include "render/precip_renderer.h"
#include "render/scene_light.h"
#include "render/weather_map.h"
#include "render/shadow_pass.h"
#include "world/world_lighting.h"
#include "world/sky.h"
#include "world/world_clock.h"
#include "world/weather_now.h"
#include "world/weather_state.h"
#include "world/cover_field.h"
#include "audio/weather_sound.h"

#include "player/emote.h"
#include "network/heading_wire.h"
#include "states/emote_client.h"
#include "world_seats.h"
#include "states/world_loader.h"
#include "ui/loading_screen.h"

#include <math.h>
#include <string.h>

/**
 * Stand-in body when the character model did not load: a coloured card, so a
 * missing asset is a visible placeholder, not an invisible player.
 */
static void render_fallback_body(float x, float y, float z, const float rgb[3]) {
    const float w = 20.0f, h = CHARACTER_HEIGHT;
    camera_billboard_begin(x, y, 0.0f, z);
    renderer_draw_rect(x - w / 2.0f, y - h, w, h, rgb[0], rgb[1], rgb[2], 1.0f);
    camera_billboard_end();
}

static void render_player_body(CharacterKind kind, uint32_t id, float x, float y, float z,
                               uint32_t race_id, uint32_t appearance,
                               const CharacterHints* hints) {
	const RaceInfo* race = client_race_find(race_id);
	// Not tinted: a player is drawn in the colours the model was authored with
    CharacterStyle style = { { 0 }, 1.0f, 0.0f, { 0 }, race ? race->key : NULL, appearance, 0 };
    if (hints) style.hints = *hints;
	if(character_renderer_ready()) {
		character_renderer_draw(kind, id, x, y, z, &style);
	} else {
		// No model to draw: a card in the race's map colour, so they are still seen
		float card[3];

		client_race_color(race_id, &card[0], &card[1], &card[2]);
		render_fallback_body(x, y, z, card);
	}
}

// Where and how a body is drawn while it emotes: the clip it plays and, for a
// seat, the spot it is drawn on. The real position stays where it stood.
static void apply_emote(const emote_state* emote, float real_heading, float* x,
                        float* y, float* z, CharacterHints* hints) {
	float durations[CHARACTER_ANIM_COUNT];
	emote_pose pose;

	if(!emote_holds_body(emote))
		return;

	emote_client_durations(emote, durations);
	hints->has_pose = 1;
	hints->pose = emote_clip(emote);
	hints->pivoting = 0;
	if(emote->id == EMOTE_CUSTOM)
		hints->clip_custom = (unsigned char)(emote_custom_clip(emote) + 1);
	if(!emote_draw_pose(emote, *x, *y, *z, real_heading, durations, &pose))
		return;

	*x = pose.x;
	*y = pose.y;
	*z = pose.z;
	hints->has_facing = 1;
	hints->facing_yaw = pose.heading;
}

// The local body: told its facing and pivot by locomotion, or in its emote.
static void local_body(const GameState* game, float* x, float* y, float* z,
                       CharacterHints* hints) {
	const PlayerState* p = &game->player;

	memset(hints, 0, sizeof(*hints));
	// the clips picked in the emote hub; others see the defaults
	hints->clip_walk = (unsigned char)(emote_slots_get(EMOTE_SLOT_WALK) + 1);
	hints->clip_jog = (unsigned char)(emote_slots_get(EMOTE_SLOT_RUN) + 1);
	hints->clip_sit = (unsigned char)(emote_slots_get(EMOTE_SLOT_SIT) + 1);
	// pushed along a wall it turns to the way it moves, not the way it is pushed
	hints->has_facing = !p->wall_slide;
	hints->facing_yaw = p->loco.heading;
	hints->pivoting = p->loco.pivoting;
	*x = p->x;
	*y = p->y;
	*z = p->visual_z;
	hints->airborne = !p->vstate.grounded;
	// sliding down a slope it stands still in its pose, facing downhill
	if(p->slide_speed > 0.0f) {
		hints->has_pose = 1;
		hints->pose = CHARACTER_ANIM_IDLE;
		hints->has_facing = 1;
		hints->facing_yaw = atan2f(p->slide_x, p->slide_y);
	}
	apply_emote(&p->emote, p->loco.heading, x, y, z, hints);
}

// Another player: where they are, or in their emote.
static void remote_body(const NearbyPlayer* p, float* x, float* y, float* z,
                        CharacterHints* hints) {
	// their facing is not sent; a seated body turns from the seat's heading
	const world_seat* seat = world_seats_get(p->emote.arg);

	memset(hints, 0, sizeof(*hints));
	*x = p->pos_x;
	*y = p->pos_y;
	*z = p->pos_z;
	hints->airborne = p->airborne;
	hints->has_speed = 1;
	hints->speed = p->speed;
	hints->has_heading = 1;
	hints->heading = heading_from_wire(p->heading);
	hints->has_idle_state = 1;
	hints->idle_fidget = p->idle_fidget;
	apply_emote(&p->emote, seat ? seat->heading : 0.0f, x, y, z, hints);
}

/**
 * Draw the 3D bodies of the local player and everyone nearby. Called between
 * character_renderer_begin() and character_renderer_end().
 */
static void playing_render_player_bodies(GameState* game) {
    for (int i = 0; i < game->playing->nearby_player_count; i++) {
        const NearbyPlayer* p = &game->playing->nearby_players[i];
        if (p->is_dead) continue;
		CharacterHints hints;
		float x, y, z;
		remote_body(p, &x, &y, &z, &hints);
		render_player_body(CHARACTER_KIND_PLAYER, p->player_id, x, y, z, p->player_race,
		                   p->appearance, &hints);
    }

	CharacterHints own;
	float x, y, z;
	local_body(game, &x, &y, &z, &own);
	render_player_body(CHARACTER_KIND_LOCAL, 0, x, y, z,
	                   game->player.info_loaded ? game->player.info.race_id : 0,
	                   game->player.info_loaded ? game->player.info.appearance : 0, &own);
}

/** Draw a health bar on a card whose point (x, y) is the top of the head. */
static void render_player_health_bar(float x, float y, float z, int32_t health,
                                     int32_t max_health) {
	if(!character_renderer_in_label_range(x, y))
		return;
	camera_billboard_begin(x, y, 0.0f, z + CHARACTER_HEIGHT);

	float bar_w = 40.0f, bar_h = 4.0f;
	float bar_x = x - bar_w / 2.0f;
	float bar_y = y - 10.0f;
	renderer_draw_rect(bar_x, bar_y, bar_w, bar_h, 0.2f, 0.2f, 0.2f, 1.0f);
	if(max_health > 0) {
		float pct = (float)health / (float)max_health;
		renderer_draw_rect(bar_x, bar_y, bar_w * pct, bar_h, 0.2f, 0.8f, 0.2f, 1.0f);
	}

	camera_billboard_end();
}

/** Health bars for everyone nearby. */
static void playing_render_player_labels(GameState* game) {
	for(int i = 0; i < game->playing->nearby_player_count; i++) {
		const NearbyPlayer* p = &game->playing->nearby_players[i];
		if(p->is_dead)
			continue;
		render_player_health_bar(p->pos_x, p->pos_y, p->pos_z, p->health, p->max_health);
	}
}

/** The selection ring under a targeted player. */
static void playing_render_player_ground_marks(GameState* game) {
    uint32_t target = game->playing->target_player_id;
    for (int i = 0; i < game->playing->nearby_player_count; i++) {
        const NearbyPlayer* p = &game->playing->nearby_players[i];
        if (p->is_dead) continue;
        if (p->player_id == target) {
            renderer_lift_begin(p->pos_z);
            renderer_draw_ring(p->pos_x, p->pos_y, 20.0f, 23.0f, 1.0f, 0.9f, 0.1f, 0.95f, 32);
            renderer_lift_end();
        }
    }
}

void playing_render_projectiles(GameState* game) {
    for (int i = 0; i < MAX_VISIBLE_PROJECTILES; i++) {
        if (!game->playing->projectiles[i].active) continue;
        VisibleProjectile* proj = &game->playing->projectiles[i];
        /* In flight where the server says: its pos_z already includes the
         * chest offset, so CAMERA_PROJECTILE_HEIGHT is not added again. */
        camera_billboard_begin(proj->pos_x, proj->pos_y, 0.0f, proj->pos_z);
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

        renderer_lift_begin(t->pos_z);
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
        renderer_lift_end();
    }
}

void playing_render_zones(GameState* game) {
    for (int i = 0; i < MAX_ZONES; i++) {
        if (!game->playing->zones[i].active) continue;
        VisibleZone* z = &game->playing->zones[i];
        renderer_lift_begin(z->pos_z);
        renderer_draw_circle(z->pos_x, z->pos_y, z->radius,
                            0.3f, 0.8f, 0.3f, 0.2f, 24);
        renderer_lift_end();
    }
}

/**
 * Draw expanding, fading rings for active healing effects.
 */
void playing_render_heal_vfxs(GameState* game) {
    for (int i = 0; i < MAX_HEAL_VFXS; i++) {
        HealVFX* vfx = &game->playing->heal_vfxs[i];
        if (!vfx->active) 
            continue;

        float t = vfx->age / vfx->duration;
        if (t > 1.0f) t = 1.0f;

        renderer_lift_begin(vfx->pos_z);

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
        renderer_lift_end();
    }
}

/**
 * Draw active ground-item markers.
 */
void playing_render_ground_items(GameState* game) {
    for (int i = 0; i < MAX_GROUND_ITEMS; i++) {
        if (!game->playing->ground_items[i].active) continue;
        GroundItem* item = &game->playing->ground_items[i];
        renderer_lift_begin(item->pos_z);
        // Sparkle border (drawn first, behind item)
        renderer_draw_rect(item->pos_x - 7, item->pos_y - 7,
                          14.0f, 14.0f, 1.0f, 0.9f, 0.3f, 0.4f);
        // Gold/yellow square (on top)
        renderer_draw_rect(item->pos_x - 6, item->pos_y - 6,
                          12.0f, 12.0f, 0.9f, 0.8f, 0.1f, 1.0f);
        renderer_lift_end();
    }
}

/**
 * Render the ordered world layers, entities, HUD, panels, and overlays.
 */
void playing_render(GameState* game) {
	if(world_loader_active()) {
		loading_screen_draw(game, world_loader_progress(), world_loader_label(),
		                    world_loader_failed() ? world_loader_error() : NULL);
		return;
	}

	// The weather first: it greys the sky and the light, and thickens the air
	weather_now_update(game->playing->frame_dt, weather_region_at(game->player.x, game->player.y),
	                   game->camera.x, game->camera.y);
	weather_map_update(game->camera.x, game->camera.y);
	// Something over the player's head: the rain is heard muffled
	weather_sound_update(game->playing->frame_dt,
	                     cover_field_covered(game->camera.x, game->camera.y, game->camera.z, 24.0f));
	// The ground haze colour paints whatever no sky or world covers
	sky_update(world_clock_hour_now());
	world_lighting_update(sky_current());
	renderer_clear(sky_current()->haze[0], sky_current()->haze[1], sky_current()->haze[2]);
    character_renderer_set_focus(game->camera.x, game->camera.y);
    renderer_begin_2d();
    camera_apply(&game->camera);

    /* The world is the authored city scene alone (D40, D41): world.dat's
     * ground and the structures grown from it are no longer drawn, though its
     * collision still decides where anyone can walk. The city goes before the
     * ground marks, so shadows, rings and telegraphs still show on its paving;
     * the catch is that they also show through its walls. */
    CameraView view;
	float eye[3];
    camera_get_view(&game->camera, &view);
	camera_view_eye(&view, eye);
	scene_light_set_eye(eye);
	// The shadows come first: the city and the bodies are lit by them
	shadow_pass_run(world_lighting_current(), game->camera.x, game->camera.y,
	                game->camera.z + CAMERA_TARGET_HEIGHT);
	sky_renderer_draw(&view, sky_current());
    city_renderer_draw(&view);

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
	precip_renderer_draw(&view, game->camera.x, game->camera.y, game->camera.z);

    /* Cards above heads, projectiles and damage numbers: screen-aligned, and
     * still hidden behind a building that stands in front of them. */
    /* One lookup for the whole pass: the badge and the map marker must agree
     * about which NPC the player is being sent to. */
    QuestTrackedStep tracked = quest_log_tracked_step(&game->playing->quest_log);
    uint32_t tracked_npc_type =
        (tracked.valid && (tracked.objective_type == QUEST_OBJECTIVE_KILL ||
                           tracked.objective_type == QUEST_OBJECTIVE_TALK))
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

	if(game->character_screen) {
		CharacterView self_view;

		character_view_from_self(game, &self_view);
		character_screen_render(game->character_screen, &self_view, "Character");
	}

	if(game->inspect_screen && game->playing->inspect.view.loaded)
		character_screen_render(game->inspect_screen, &game->playing->inspect.view, "Inspect");

    // Render dialogue (on top of everything)
    dialogue_render(game->camera.viewport_width, game->camera.viewport_height);

    // Chat, party, death, level-up overlays
    playing_render_chat(game);
    emote_hub_render_previews(game);
    emote_hub_draw(game);
    weather_window_draw(game);
    playing_render_party_frames(game);
    playing_render_party_invite(game);
    playing_render_death_screen(game);
    playing_render_level_up(game);
    playing_render_reward_notifications(game);

    // Quest log panel (before overlays)
    quest_log_render(&game->playing->quest_log,
                     game->camera.viewport_width,
                     game->camera.viewport_height,
                     game->player.x, game->player.y);

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
