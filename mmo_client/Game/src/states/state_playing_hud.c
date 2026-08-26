/**
 * @file
 * Draw the gameplay overlays: chat, party, death, map, pause and settings.
 *
 * Screen-space, all of it: nothing here is inside the camera transform, and
 * nothing here changes any state -- these functions read PlayingState and
 * draw. Where each one sits in the frame is decided by playing_render() in
 * state_playing_render.c.
 *
 * The chat box's geometry is in states/state_playing_internal.h rather than
 * here, because the input router hit-tests the same rectangle and the two have
 * to agree about where it is.
 */
#include "states/state_playing_internal.h"
#include "renderer.h"
#include "world/world_overview.h"
#include "ui/quest_tracker.h"
#include "ui/settings_panel.h"

#include <math.h>

/**
 * Draw chat history, channel state, and the active input buffer.
 */
void playing_render_chat(GameState* game) {
    ChatState* chat = &game->playing->chat;

    float chat_h  = (float)CHAT_LINES * CHAT_LINE_H + CHAT_PAD * 2.0f;
    float chat_y  = (float)game->camera.viewport_height - chat_h - CHAT_INBOX_H - 20.0f;

    int has_messages = (chat->line_count > 0);

    // Message panel: always show when typing, show when there are messages,
    // and show a very faint panel otherwise so the user knows chat is here.
    {
        float bg_alpha;
        if (chat->is_typing)   bg_alpha = 0.65f;
        else if (has_messages) bg_alpha = 0.35f;
        else                   bg_alpha = 0.10f;  // faint "empty" state

        renderer_draw_rect(CHAT_X, chat_y, CHAT_W, chat_h, 0.04f, 0.04f, 0.08f, bg_alpha);

        float border_a = chat->is_typing ? 0.6f : (has_messages ? 0.4f : 0.15f);
        // Top border
        renderer_draw_rect(CHAT_X, chat_y, CHAT_W, 1.5f, 0.4f, 0.5f, 0.7f, border_a);
        // Left border
        renderer_draw_rect(CHAT_X, chat_y, 1.5f, chat_h, 0.4f, 0.5f, 0.7f, border_a);
    }

    // Messages (newest at the bottom)
    int start = chat->line_count - CHAT_LINES;
    if (start < 0) start = 0;
    for (int i = start; i < chat->line_count; i++) {
        const ChatLine* ln = &chat->lines[i];

        const char* prefix = "";
        switch (ln->channel) {
            case 0: prefix = "[L] "; break;
            case 1: prefix = "[G] "; break;
            case 2: prefix = "[W] "; break;
            case 3: prefix = "[P] "; break;
        }

        char line[320];
        snprintf(line, sizeof(line), "%s%s: %s", prefix, ln->sender, ln->text);

        float line_y = chat_y + CHAT_PAD + (float)(i - start) * CHAT_LINE_H + CHAT_LINE_H;
        renderer_draw_text(CHAT_X + CHAT_PAD, line_y, line);
    }

    // Input box
    float input_y = chat_y + chat_h;

    if (chat->is_typing) {
        static const char* ch_names[] = {"Local", "Global", "Whisper", "Party"};
        int ch = (int)chat->active_channel;
        if (ch < 0 || ch > 3) ch = 0;

        // Input box background
        renderer_draw_rect(CHAT_X, input_y, CHAT_W, CHAT_INBOX_H, 0.08f, 0.08f, 0.14f, 0.90f);
        // Bottom and left borders
        renderer_draw_rect(CHAT_X, input_y + CHAT_INBOX_H - 1.5f, CHAT_W, 1.5f, 0.4f, 0.5f, 0.7f, 0.6f);
        renderer_draw_rect(CHAT_X, input_y, 1.5f, CHAT_INBOX_H, 0.4f, 0.5f, 0.7f, 0.6f);

        // Channel badge e.g. "[Local]" or "[-> Name]" for whisper with a known target
        char badge[48];
        if (ch == 2 && chat->whisper_reply_target[0] != '\0') {
            snprintf(badge, sizeof(badge), "[-> %s] ", chat->whisper_reply_target);
        } else {
            snprintf(badge, sizeof(badge), "[%s] ", ch_names[ch]);
        }
        renderer_draw_text(CHAT_X + CHAT_PAD, input_y + CHAT_INBOX_H - 8.0f, badge);

        // Typed text with blinking cursor
        char input_display[270];
        snprintf(input_display, sizeof(input_display), "%s_", chat->input_buf);
        renderer_draw_text(CHAT_X + CHAT_PAD + 72.0f, input_y + CHAT_INBOX_H - 8.0f, input_display);
    } else {
        // Idle input area — always show a faint clickable hint
        float a = has_messages ? 0.25f : 0.15f;
        renderer_draw_rect(CHAT_X, input_y, CHAT_W, CHAT_INBOX_H, 0.04f, 0.04f, 0.08f, a);
        renderer_draw_rect(CHAT_X, input_y + CHAT_INBOX_H - 1.5f, CHAT_W, 1.5f, 0.4f, 0.5f, 0.7f, a * 1.2f);
        renderer_draw_rect(CHAT_X, input_y, 1.5f, CHAT_INBOX_H, 0.4f, 0.5f, 0.7f, a * 1.2f);
        renderer_draw_text(CHAT_X + CHAT_PAD, input_y + CHAT_INBOX_H - 8.0f, "Press T to chat");
    }
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
        if (!notif->active) continue;

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
    if (tx + tw > (float)game->camera.viewport_width)  tx = game->input.mouse_x - tw - 4.0f;
    if (ty < 0) ty = game->input.mouse_y + 12.0f;

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
 * Draw the scaled in-game settings panel over the world.
 */
void playing_render_settings_overlay(GameState* game) {
    if (!game->show_settings) return;

    float vw = (float)game->camera.viewport_width;
    float vh = (float)game->camera.viewport_height;

    // Semi-transparent dim (lighter than pause so world is visible)
    renderer_draw_rect(0, 0, vw, vh, 0.0f, 0.0f, 0.0f, 0.40f);

    float scale = game->settings.ui_scale;
    float pw = SP_PW * scale;
    float ph = SP_PH * scale;
    float px = (vw - pw) * 0.5f;
    float py = (vh - ph) * 0.5f;

    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glTranslatef(px, py, 0.0f);
    glScalef(scale, scale, 1.0f);

    // Panel background (in scaled space at 0,0)
    renderer_draw_rect(0, 0, SP_PW, SP_PH, 0.10f, 0.10f, 0.16f, 0.97f);
    // Panel border
    renderer_draw_rect(0,         0,         SP_PW, 2.0f, 0.35f,0.50f,0.70f,1.0f);
    renderer_draw_rect(0,         SP_PH-2,   SP_PW, 2.0f, 0.35f,0.50f,0.70f,1.0f);
    renderer_draw_rect(0,         0,         2.0f,  SP_PH, 0.35f,0.50f,0.70f,1.0f);
    renderer_draw_rect(SP_PW-2,   0,         2.0f,  SP_PH, 0.35f,0.50f,0.70f,1.0f);

    sp_draw_content(0, 0, &game->settings);

    // Close button
    float btn_w = 140.0f, btn_h = 36.0f;
    float btn_x = (SP_PW - btn_w) * 0.5f;
    float btn_y = SP_PH - 54.0f;
    // Inverse-transform mouse for hover detection
    float rel_mx = (game->input.mouse_x - px) / scale;
    float rel_my = (game->input.mouse_y - py) / scale;
    int hov = (rel_mx >= btn_x && rel_mx <= btn_x + btn_w &&
               rel_my >= btn_y && rel_my <= btn_y + btn_h);
    float bc = hov ? 0.30f : 0.18f;
    renderer_draw_rect(btn_x, btn_y, btn_w, btn_h, bc, bc, bc+0.12f, 0.95f);
    renderer_draw_rect(btn_x,            btn_y,             btn_w, 1.5f, 0.35f,0.50f,0.70f,0.8f);
    renderer_draw_rect(btn_x,            btn_y+btn_h-1.5f,  btn_w, 1.5f, 0.35f,0.50f,0.70f,0.8f);
    renderer_draw_rect(btn_x,            btn_y,             1.5f, btn_h, 0.35f,0.50f,0.70f,0.8f);
    renderer_draw_rect(btn_x+btn_w-1.5f, btn_y,             1.5f, btn_h, 0.35f,0.50f,0.70f,0.8f);
    renderer_draw_text(btn_x + btn_w*0.5f - 22.0f, btn_y+btn_h-10.0f, "Close");

    glPopMatrix();
}

/**
 * Draw the centered, zoomable map with entity markers.
 */
void playing_render_big_map(GameState* game) {
    if (!game->playing->show_map) return;

    float vw = (float)game->camera.viewport_width;
    float vh = (float)game->camera.viewport_height;

    // Screen space is already active for the UI phase, but this panel is drawn
    // before the HUD, so it guards its own coordinate system rather than relying
    // on draw order.
    renderer_begin_screen_space();

    // Dim world behind the map
    renderer_draw_rect(0, 0, vw, vh, 0.0f, 0.0f, 0.0f, 0.65f);

    // Panel
    float pw = 620.0f, ph = 640.0f;
    float px = floorf((vw - pw) * 0.5f);
    float py = floorf((vh - ph) * 0.5f);

    renderer_draw_rect(px, py, pw, ph, 0.07f, 0.07f, 0.11f, 0.97f);

    // Border
    renderer_draw_rect(px,        py,        pw,   2.0f, 0.40f, 0.50f, 0.65f, 1.0f);
    renderer_draw_rect(px,        py+ph-2,   pw,   2.0f, 0.40f, 0.50f, 0.65f, 1.0f);
    renderer_draw_rect(px,        py,        2.0f, ph,   0.40f, 0.50f, 0.65f, 1.0f);
    renderer_draw_rect(px+pw-2,   py,        2.0f, ph,   0.40f, 0.50f, 0.65f, 1.0f);

    // Title bar
    renderer_draw_rect(px, py, pw, 32.0f, 0.10f, 0.12f, 0.18f, 1.0f);
    renderer_draw_text(px + 12.0f, py + 22.0f, "Map");
    renderer_draw_text(px + pw - 130.0f, py + 22.0f, "M / ESC to close");

    // Map drawing area (inset from the panel)
    float mx = px + 10.0f;
    float my = py + 38.0f;
    float mw = pw - 20.0f;
    float mh = ph - 60.0f;
    float mcx = mx + mw * 0.5f;
    float mcy = my + mh * 0.5f;

    // Clip: dark map background
    renderer_draw_rect(mx, my, mw, mh, 0.04f, 0.05f, 0.07f, 1.0f);

    float player_wx = game->player.x;
    float player_wy = game->player.y;

    // view_radius in world units visible to the map edge
    float base_radius = 1050.0f;
    float view_radius = base_radius / game->playing->map_zoom;
    float scale = (mw * 0.5f) / view_radius;

    // Terrain: the generator's downscaled overview, drawn as the map backdrop.
    // The streamed world file is far too large to sample here, so the map shows
    // this instead; without it the map falls back to grid lines only.
    if (world_overview_ready()) {
        int ow = 0, oh = 0, ocell = 0;
        world_overview_dims(&ow, &oh, &ocell);

        float tile_px = (float)game->world.tile_size;
        float world_px_w = (float)ow * (float)ocell * tile_px;
        float world_px_h = (float)oh * (float)ocell * tile_px;

        if (world_px_w > 0.0f && world_px_h > 0.0f) {
            float half_h = (mh * 0.5f) / scale;
            float u0 = (player_wx - view_radius) / world_px_w;
            float u1 = (player_wx + view_radius) / world_px_w;
            float v0 = (player_wy - half_h)      / world_px_h;
            float v1 = (player_wy + half_h)      / world_px_h;

            renderer_draw_sprite_uv(mx, my, mw, mh,
                                    world_overview_texture(),
                                    u0, v0, u1, v1);
        }
    }

    // Grid lines. The spacing steps up as the view widens so the grid stays a
    // reference instead of collapsing into a solid block at continent scale.
    float grid_spacing = 200.0f;
    while (view_radius / grid_spacing > 12.0f) grid_spacing *= 5.0f;
    float grid_start_x = player_wx - view_radius;
    float grid_start_y = player_wy - view_radius;
    // Snap to grid
    grid_start_x = floorf(grid_start_x / grid_spacing) * grid_spacing;
    grid_start_y = floorf(grid_start_y / grid_spacing) * grid_spacing;

    for (float wx = grid_start_x; wx < player_wx + view_radius; wx += grid_spacing) {
        float sx = mcx + (wx - player_wx) * scale;
        if (sx < mx || sx > mx + mw) continue;
        renderer_draw_rect(sx, my, 1.0f, mh, 0.15f, 0.15f, 0.22f, 1.0f);
    }
    for (float wy = grid_start_y; wy < player_wy + view_radius; wy += grid_spacing) {
        float sy = mcy + (wy - player_wy) * scale;
        if (sy < my || sy > my + mh) continue;
        renderer_draw_rect(mx, sy, mw, 1.0f, 0.15f, 0.15f, 0.22f, 1.0f);
    }

    // NPC dots
    for (int i = 0; i < game->playing->visible_npc_count; i++) {
        const VisibleNPC* npc = &game->playing->visible_npcs[i];
        if (!npc->is_alive) continue;
        float sx = mcx + (npc->pos_x - player_wx) * scale;
        float sy = mcy + (npc->pos_y - player_wy) * scale;
        if (sx < mx || sx > mx+mw-7 || sy < my || sy > my+mh-7) continue;
        float dr, dg, db;
        switch (npc->category) {
            case 1:  dr=0.90f; dg=0.22f; db=0.22f; break; // hostile - red
            case 2:  dr=0.90f; dg=0.78f; db=0.10f; break; // quest   - gold
            default: dr=0.20f; dg=0.55f; db=0.90f; break; // passive - blue
        }
        renderer_draw_rect(sx - 3.5f, sy - 3.5f, 7.0f, 7.0f, dr, dg, db, 0.95f);
    }

    // Nearby player dots
    for (int i = 0; i < game->playing->nearby_player_count; i++) {
        const NearbyPlayer* p = &game->playing->nearby_players[i];
        if (p->is_dead) continue;
        float sx = mcx + (p->pos_x - player_wx) * scale;
        float sy = mcy + (p->pos_y - player_wy) * scale;
        if (sx < mx || sx > mx+mw-7 || sy < my || sy > my+mh-7) continue;
        renderer_draw_rect(sx - 3.5f, sy - 3.5f, 7.0f, 7.0f, 0.25f, 0.90f, 0.45f, 1.0f);
    }

    // Tracked objective marker, above the dots and clamped into view
    quest_tracker_render_map_marker(&game->playing->quest_log,
                                    mx, my, mw, mh, mcx, mcy,
                                    player_wx, player_wy, scale);

    // Own player dot — always at center
    renderer_draw_rect(mcx - 6.0f, mcy - 6.0f, 12.0f, 12.0f, 1.0f, 1.0f, 0.25f, 1.0f);

    // Bottom bar: coords + zoom
    float bar_y = py + ph - 22.0f;
    char info[64];
    snprintf(info, sizeof(info), "X: %.0f  Y: %.0f    Zoom: %.1fx",
             player_wx, player_wy, game->playing->map_zoom);
    renderer_draw_text(px + 10.0f, bar_y + 14.0f, info);

    renderer_end_screen_space();
}

/**
 * Draw the pause menu and its navigation controls.
 */
void playing_render_pause_overlay(GameState* game) {
    if (!game->playing->is_paused) return;

    float vw = (float)game->camera.viewport_width;
    float vh = (float)game->camera.viewport_height;

    // Dim the world
    renderer_draw_rect(0, 0, vw, vh, 0.0f, 0.0f, 0.0f, 0.55f);

    // Panel dimensions
    float pw = 340.0f, ph = 230.0f;
    float px = (vw - pw) * 0.5f;
    float py = (vh - ph) * 0.5f;

    // Panel background
    renderer_draw_rect(px, py, pw, ph, 0.07f, 0.07f, 0.12f, 0.96f);

    // Panel border (top/bottom/left/right)
    renderer_draw_rect(px,          py,          pw,   2.0f, 0.45f, 0.45f, 0.75f, 1.0f);
    renderer_draw_rect(px,          py + ph - 2, pw,   2.0f, 0.45f, 0.45f, 0.75f, 1.0f);
    renderer_draw_rect(px,          py,          2.0f, ph,   0.45f, 0.45f, 0.75f, 1.0f);
    renderer_draw_rect(px + pw - 2, py,          2.0f, ph,   0.45f, 0.45f, 0.75f, 1.0f);

    // Title
    renderer_draw_text(px + pw * 0.5f - 32.0f, py + 36.0f, "PAUSED");

    // Divider below title
    renderer_draw_rect(px + 20, py + 48, pw - 40, 1.5f, 0.35f, 0.35f, 0.55f, 0.8f);

    // Button layout
    float bw = 220.0f, bh = 38.0f;
    float bx = px + (pw - bw) * 0.5f;

    // Resume
    renderer_draw_rect(bx, py + 66,  bw, bh, 0.12f, 0.38f, 0.12f, 0.92f);
    renderer_draw_text(bx + bw * 0.5f - 28.0f, py + 91, "Resume");

    // Settings (placeholder)
    renderer_draw_rect(bx, py + 116, bw, bh, 0.18f, 0.18f, 0.32f, 0.92f);
    renderer_draw_text(bx + bw * 0.5f - 32.0f, py + 141, "Settings");

    // Quit to Menu
    renderer_draw_rect(bx, py + 166, bw, bh, 0.32f, 0.08f, 0.08f, 0.92f);
    renderer_draw_text(bx + bw * 0.5f - 50.0f, py + 191, "Quit to Menu");
}
