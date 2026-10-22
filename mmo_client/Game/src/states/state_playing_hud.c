/**
 * @file
 * Draw the gameplay overlays: chat, party, death, map, pause and settings.
 *
 * Screen-space, all of it: nothing here is inside the camera transform, and
 * nothing here changes any state -- these functions read PlayingState and
 * draw. Where each one sits in the frame is decided by playing_render() in
 * state_playing_render.c.
 *
 * The chat box's geometry is in states/chat_layout.h rather than here, because
 * the input router hit-tests the same rectangle and the two have to agree about
 * where it is.
 */
#include "states/state_playing_internal.h"
#include "renderer.h"
#include "ui/map_view.h"
#include "ui/quest_tracker.h"
#include "ui/settings_menu.h"
#include "ui/pause_menu.h"
#include "ui/ui_kit.h"
#include "hud.h"
#include "states/chat_channels.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

// Measures text in the game's font; the chat wraps and clips by it.
float playing_chat_measure(const char* text, size_t length) {
	char copy[CHAT_FORMAT_MAX];

	if(length >= sizeof(copy))
		length = sizeof(copy) - 1;
	memcpy(copy, text, length);
	copy[length] = '\0';

	return renderer_text_width(copy);
}

// Text with a dark edge, so it reads over the world with nothing behind it
static void draw_shadowed(float x, float y, const char* text, float r, float g, float b) {
	renderer_draw_text_colored(x + 1.5f, y + 1.5f, text, 0.0f, 0.0f, 0.0f);
	renderer_draw_text_colored(x, y, text, r, g, b);
}

// A thin outline round the whole box. It is what the player grabs to move chat.
static void draw_chat_outline(const GameState* game, const chat_box* box, int scrolled_back) {
	float x, y, w, h;
	float mx = game->input.mouse_x;
	float my = game->input.mouse_y;
	int hot;
	float a;

	chat_window_rect(box, &x, &y, &w, &h);
	hot = game->playing->chat.win.dragging ||
	      window_pos_on_edge(x, y, w, h, CHAT_EDGE_BAND, mx, my);
	a = hot ? 0.9f : 0.30f;

	renderer_draw_rect(x, y, w, 1.5f, 0.45f, 0.55f, 0.75f, a);
	renderer_draw_rect(x, y + h - 1.5f, w, 1.5f, 0.45f, 0.55f, 0.75f, a);
	renderer_draw_rect(x, y, 1.5f, h, 0.45f, 0.55f, 0.75f, a);
	renderer_draw_rect(x + w - 1.5f, y, 1.5f, h, 0.45f, 0.55f, 0.75f, a);

	if(scrolled_back) {
		ui_kit_px_begin(game->camera.viewport_width, game->camera.viewport_height);
		ui_kit_px_text(x + w - CHAT_PAD, y - 10.0f, "scrolled back - PgDn", 12.0f, KIT_HEAD,
		               KIT_RIGHT);
		ui_kit_px_end();
	}
}

// The messages that fit, wrapped, each in its channel's colour.
static void draw_chat_lines(const chat_box* box, const ChatState* chat) {
	chat_visible visible[CHAT_LINES];
	int count = chat_layout_visible(chat, playing_chat_measure, visible);

	for(int i = 0; i < count; i++) {
		const chat_visible* v = &visible[i];
		float r, g, b;

		chat_line_color(&chat->lines[v->line], &r, &g, &b);
		for(int row = 0; row < v->row_count; row++) {
			char text[CHAT_FORMAT_MAX];
			float y = box->y + CHAT_PAD + (float)(v->first_row + row) * CHAT_LINE_H + CHAT_LINE_H;

			snprintf(text, sizeof(text), "%.*s", v->rows[row].length,
					v->text + v->rows[row].start);
			draw_shadowed(box->x + CHAT_PAD, y, text, r, g, b);
		}
	}
}

// The input line while typing: a channel badge, the line's tail, a cursor.
static void draw_typing_box(const chat_box* box, const ChatState* chat) {
	const chat_channel_info* info = chat_channel_lookup(chat->active_channel);
	float text_y = box->input_y + box->input_h - 8.0f;
	char badge[48];
	char shown[MAX_CHAT_INPUT_LEN + 2];
	float badge_w;
	float room;

	chat_badge_format(chat->active_channel, chat->whisper_reply_target,
			badge, sizeof(badge));
	badge_w = renderer_text_width(badge);
	draw_shadowed(box->x + CHAT_PAD, text_y, badge, info->r, info->g, info->b);

	// The text starts where this channel's badge ends, so it differs by channel
	room = CHAT_TEXT_W - badge_w - renderer_text_width("_");
	snprintf(shown, sizeof(shown), "%s_",
			chat_input_tail(chat->input_buf, room, playing_chat_measure));
	draw_shadowed(box->x + CHAT_PAD + badge_w, text_y, shown, 1.0f, 1.0f, 1.0f);
}

/**
 * Draw chat history, channel state, and the active input buffer.
 * @param game  The GameState to read.
 */
void playing_render_chat(GameState* game) {
	ChatState* chat = &game->playing->chat;
	chat_box box = chat_box_at(&chat->win, (float)game->camera.viewport_width,
	                           (float)game->camera.viewport_height);

	draw_chat_outline(game, &box, chat->scroll > 0);
	draw_chat_lines(&box, chat);

	if(chat->is_typing)
		draw_typing_box(&box, chat);
}

/**
 * Draw health and mana frames for current party members.
 */
void playing_render_party_frames(GameState* game) {
    PartyState* ps = &game->playing->party;
    if (!ps->has_party) return;

    float frame_x = 10.0f;
    float frame_y = 80.0f;
    float frame_w = 160.0f;
    float frame_h = 50.0f;

    for (int i = 0; i < ps->member_count && i < MAX_PARTY_SIZE; i++) {
        PartyMember* m = &ps->members[i];
        float y = frame_y + (float)i * (frame_h + 4.0f);

        // Background
        renderer_draw_rect(frame_x, y, frame_w, frame_h, 0.1f, 0.1f, 0.15f, 0.7f);

        // Name
        renderer_draw_text(frame_x + 4.0f, y + 16.0f, m->name);

        // HP bar
        float hp_y = y + 22.0f;
        renderer_draw_rect(frame_x + 4, hp_y, frame_w - 8, 10.0f, 0.2f, 0.2f, 0.2f, 1.0f);
        if (m->max_health > 0) {
            float pct = (float)m->health / (float)m->max_health;
            renderer_draw_rect(frame_x + 4, hp_y, (frame_w - 8) * pct, 10.0f,
                             0.2f, 0.8f, 0.2f, 1.0f);
        }

        // Mana bar
        float mp_y = y + 34.0f;
        renderer_draw_rect(frame_x + 4, mp_y, frame_w - 8, 8.0f, 0.1f, 0.1f, 0.2f, 1.0f);
        if (m->max_mana > 0) {
            float pct = (float)m->mana / (float)m->max_mana;
            renderer_draw_rect(frame_x + 4, mp_y, (frame_w - 8) * pct, 8.0f,
                             0.2f, 0.3f, 0.9f, 1.0f);
        }
    }
}

/**
 * Draw the pending party invitation and response buttons.
 */
void playing_render_party_invite(GameState* game) {
    PartyState* ps = &game->playing->party;
    if (!ps->has_pending_invite) return;

    float w = 300.0f, h = 80.0f;
    float x = ((float)game->camera.viewport_width - w) / 2.0f;
    float y = 200.0f;

    // Background
    renderer_draw_rect(x, y, w, h, 0.1f, 0.1f, 0.2f, 0.85f);
    // Border
    renderer_draw_rect(x, y, w, 2.0f, 0.5f, 0.5f, 0.8f, 1.0f);

    char msg[96];
    snprintf(msg, sizeof(msg), "%s invited you to a party", ps->invite_from_name);
    renderer_draw_text(x + 10.0f, y + 22.0f, msg);

    char timer_str[32];
    snprintf(timer_str, sizeof(timer_str), "%.0fs remaining", ps->invite_timer);
    renderer_draw_text(x + 10.0f, y + 42.0f, timer_str);

    // Accept button
    renderer_draw_rect(x + 30, y + 50, 80, 24, 0.2f, 0.6f, 0.2f, 0.9f);
    renderer_draw_text(x + 42, y + 68, "Accept");

    // Decline button
    renderer_draw_rect(x + 190, y + 50, 80, 24, 0.6f, 0.2f, 0.2f, 0.9f);
    renderer_draw_text(x + 198, y + 68, "Decline");
}

/**
 * Draw the death overlay and animated respawn status.
 */
void playing_render_death_screen(GameState* game) {
    if (!game->playing->is_dead) return;

    float vw = (float)game->camera.viewport_width;
    float vh = (float)game->camera.viewport_height;

    // Dark red-tinted overlay
    renderer_draw_rect(0, 0, vw, vh, 0.15f, 0.0f, 0.0f, 0.72f);

    float cx = vw / 2.0f;
    float cy = vh / 2.0f;

    // Panel
    float pw = 260.0f, ph = 90.0f;
    float px = cx - pw * 0.5f, py = cy - ph * 0.5f;
    renderer_draw_rect(px, py, pw, ph, 0.08f, 0.05f, 0.05f, 0.95f);
    // Dark red border
    renderer_draw_rect(px,          py,          pw,   2.0f, 0.6f, 0.1f, 0.1f, 1.0f);
    renderer_draw_rect(px,          py + ph - 2, pw,   2.0f, 0.6f, 0.1f, 0.1f, 1.0f);
    renderer_draw_rect(px,          py,          2.0f, ph,   0.6f, 0.1f, 0.1f, 1.0f);
    renderer_draw_rect(px + pw - 2, py,          2.0f, ph,   0.6f, 0.1f, 0.1f, 1.0f);

    // "YOU DIED" title
    renderer_draw_text(cx - 38, py + 16, "YOU DIED");

    // Animated "Respawning..." dots based on death_timer
    int dots = (int)(game->playing->death_timer * 2.0f) % 4;
    static const char* dot_labels[] = { "Respawning", "Respawning.", "Respawning..", "Respawning..." };
    renderer_draw_text(cx - 52, py + 52, dot_labels[dots]);
}

void playing_render_level_up(GameState* game) {
    if (!game->playing->show_level_up) return;

    float cx = (float)game->camera.viewport_width / 2.0f;
    char msg[64];
    snprintf(msg, sizeof(msg), "LEVEL UP! You are now level %d!", game->playing->level_up_new_level);
    renderer_draw_rect(cx - 150, 100, 300, 40, 0.1f, 0.1f, 0.3f, 0.8f);
    renderer_draw_text(cx - 140, 126, msg);
}

/**
 * Draw timed experience and coin reward notifications.
 */
void playing_render_reward_notifications(GameState* game) {
    // Notifications stack upward from center-right
    float base_x = (float)game->camera.viewport_width * 0.5f + 60.0f;
    float base_y = (float)game->camera.viewport_height * 0.5f - 60.0f;

    for (int i = 0; i < MAX_REWARD_POPUPS; i++) {
        RewardNotification* notif = &game->playing->reward_notifications[i];
        if (!notif->active) 
            continue;

        float t     = notif->age / 2.5f;          // 0..1 over lifetime
        float alpha = 1.0f - t;
        if (alpha < 0.0f) alpha = 0.0f;

        // Float up 80 px over lifetime
        float y = base_y - notif->age * 32.0f + (float)i * 44.0f;
        float x = base_x;

        // Panel background with slight border
        float pw = 190.0f, ph = 36.0f;
        renderer_draw_rect(x - 2, y - 2, pw + 4, ph + 4,
                           0.6f, 0.5f, 0.1f, alpha * 0.4f);      // gold glow border
        renderer_draw_rect(x, y, pw, ph,
                           0.06f, 0.05f, 0.10f, alpha * 0.82f);  // dark bg

        float text_y = y + ph - 8.0f;

        // XP pill
        if (notif->xp_gained > 0) {
            renderer_draw_rect(x + 6, y + 8, 14, 14, 0.35f, 0.55f, 1.0f, alpha);
            char xp_text[32];
            snprintf(xp_text, sizeof(xp_text), "+%u XP", notif->xp_gained);
            renderer_draw_text(x + 24, text_y, xp_text);
        }

        // Coin pill, labelled with the paying kingdom rather than a bare "g"
        if (notif->currency_gained > 0) {
            renderer_draw_rect(x + 100, y + 8, 14, 14, 1.0f, 0.80f, 0.05f, alpha);
            char coin_text[48];
            snprintf(coin_text, sizeof(coin_text), "+%u %s",
                     notif->currency_gained,
                     world_currency_name(notif->currency_id));
            renderer_draw_text(x + 118, text_y, coin_text);
        }
    }
}

// Hovered buff icon index (-1 = none), updated each input frame
/**
 * Draw a bounded tooltip for the currently hovered status effect.
 */
void playing_render_buff_tooltip(const GameState* game) {
    if (game->playing->hovered_effect < 0 || game->playing->hovered_effect >= MAX_CLIENT_EFFECTS) return;
    const ClientStatusEffect* e = &game->playing->ability_bar.effects[game->playing->hovered_effect];
    if (!e->active) return;

    static const char* effect_names[] = {
        "Unknown", "Damage Over Time", "Heal Over Time", "Stunned",
        "Slowed", "Buff", "Stealth", "Knock Up", "Linked", "Cleanse"
    };
    const char* name = (e->effect_type < 10) ? effect_names[e->effect_type] : "Unknown";

    float tw = 170.0f, th = 54.0f;
    float tx = game->input.mouse_x + 12.0f;
    float ty = game->input.mouse_y - th - 4.0f;
    if (tx + tw > (float)game->camera.viewport_width)  
        tx = game->input.mouse_x - tw - 4.0f;
    if (ty < 0) 
        ty = game->input.mouse_y + 12.0f;

    renderer_draw_rect(tx, ty, tw, th, 0.06f, 0.06f, 0.12f, 0.96f);
    renderer_draw_rect(tx, ty, tw, 1.5f, 0.45f,0.45f,0.75f,1.0f);
    renderer_draw_rect(tx, ty+th-1.5f, tw, 1.5f, 0.45f,0.45f,0.75f,1.0f);
    renderer_draw_rect(tx, ty, 1.5f, th, 0.45f,0.45f,0.75f,1.0f);
    renderer_draw_rect(tx+tw-1.5f, ty, 1.5f, th, 0.45f,0.45f,0.75f,1.0f);

    renderer_draw_text(tx + 8.0f, ty + 18.0f, name);

    char line2[48];
    snprintf(line2, sizeof(line2), "%.1fs remaining", e->duration_remaining);
    renderer_draw_text(tx + 8.0f, ty + 36.0f, line2);

    if (e->value != 0) {
        char line3[32];
        snprintf(line3, sizeof(line3), "Value: %d", e->value);
        renderer_draw_text(tx + 8.0f, ty + 48.0f, line3);
    }
}

/**
 * Draw the in-game settings menu over the world.
 */
void playing_render_settings_overlay(GameState* game) {
	if(!game->show_settings)
		return;

	renderer_draw_rect(0, 0, (float)game->camera.viewport_width,
	                   (float)game->camera.viewport_height, 0.0f, 0.0f, 0.0f, 0.6f);
	sm_draw(game);
}

/**
 * Draw the full map screen.
 */
void playing_render_big_map(GameState* game) {
	map_view_render(game);
}

/**
 * Draw the pause menu over the world.
 */
void playing_render_pause_overlay(GameState* game) {
	if(!game->playing->is_paused)
		return;

	pm_draw(game);
}
