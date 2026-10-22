// Emotes as the local player lives them: ask, predict, and follow the server.

#include "states/emote_client.h"
#include "player/emote_clips.h"
#include "states/chat_log.h"
#include "network/network.h"
#include "render/character_renderer.h"

#include <stdio.h>
#include <string.h>

void emote_client_durations(const emote_state* s, float out[CHARACTER_ANIM_COUNT]) {
	for(int a = 0; a < CHARACTER_ANIM_COUNT; a++)
		out[a] = character_renderer_clip_seconds((CharacterAnim)a);

	if(s && s->id == EMOTE_CUSTOM) {
		out[CHARACTER_ANIM_CUSTOM_ENTER] =
				character_renderer_table_clip_seconds(emote_clip_enter(s->arg));
		out[CHARACTER_ANIM_CUSTOM_EXIT] =
				character_renderer_table_clip_seconds(emote_clip_exit(s->arg));
	}
}

static const char* player_name(const GameState* game) {
	if(game->player.info_loaded && game->player.info.name[0] != '\0')
		return game->player.info.name;

	return "You";
}

// An error is shown in red, an emote as plain text.
static void say(GameState* game, emote_say kind, const char* text) {
	chat_style style = kind == EMOTE_SAY_ERROR ? CHAT_STYLE_ERROR : CHAT_STYLE_EMOTE;

	if(kind == EMOTE_SAY_NONE || !game->playing)
		return;

	chat_log_add(&game->playing->chat, CHAT_CHANNEL_LOCAL, style, "", text);
}

// What the visible players are in, so a seat someone is on is not offered.
static int gather_occupants(const GameState* game, emote_occupant* out, int max) {
	int n = 0;
	int count = game->playing->nearby_player_count;

	if(count > MAX_NEARBY_PLAYERS)
		count = MAX_NEARBY_PLAYERS;

	for(int i = 0; i < count && n < max; i++) {
		const NearbyPlayer* p = &game->playing->nearby_players[i];

		if(p->is_dead || p->emote.phase == EMOTE_PHASE_NONE)
			continue;

		out[n].id = p->emote.id;
		out[n].arg = p->emote.arg;
		n++;
	}

	return n;
}

// Starts an emote; a clip of 0 or more is the one an EMOTE_CUSTOM plays.
static void start(GameState* game, const emote_def* def, int clip) {
	emote_state* me = &game->player.emote;
	emote_occupant others[MAX_NEARBY_PLAYERS];
	uint8_t arg = 0;
	char text[EMOTE_TEXT_MAX];
	int n;

	if(!game->playing || !def)
		return;

	// already in it: asking again would only say it twice
	if((me->phase == EMOTE_PHASE_ENTERING || me->phase == EMOTE_PHASE_HELD) &&
	   me->id == def->id && (clip < 0 || me->arg == clip))
		return;

	n = gather_occupants(game, others, MAX_NEARBY_PLAYERS);
	if(emote_check(def, game->player.x, game->player.y, game->playing->is_dead,
	               others, n, &arg) != EMOTE_RESULT_STARTED) {
		snprintf(text, sizeof(text), "Unable to %s", def->command);
		say(game, EMOTE_SAY_ERROR, text);
		return;
	}

	if(clip >= 0)
		arg = (uint8_t)clip;

	// the body responds at once; the server's answer confirms or takes it back
	emote_begin(me, def->id, arg);
	network_send_emote_request(def->id, arg);
}

void emote_client_request_slot(GameState* game, int slot) {
	int clip = emote_slots_get(slot);
	const emote_def* def = emote_lookup(emote_clip_emote(clip));

	// a whole emote (a lean) needs no clip; it plays its own enter, idle and exit
	start(game, def, def && def->id == EMOTE_CUSTOM ? clip : -1);
}

void emote_client_request(GameState* game, const emote_def* def) {
	if(def && def->id == EMOTE_CUSTOM)
		emote_client_request_slot(game, EMOTE_SLOT_EMOTE_1);
	else
		start(game, def, -1);
}

void emote_client_stop(GameState* game) {
	emote_state* me = &game->player.emote;
	char text[EMOTE_TEXT_MAX];

	if(!game->playing)
		return;
	if(me->phase != EMOTE_PHASE_ENTERING && me->phase != EMOTE_PHASE_HELD)
		return;

	emote_say_stop(text, sizeof(text), player_name(game), emote_lookup(me->id));
	emote_end(me);
	say(game, EMOTE_SAY_EMOTE, text);
	network_send_emote_request(EMOTE_NONE, 0);
}

void emote_client_poll(GameState* game) {
	emote_event ev;

	if(!game->playing)
		return;

	while(network_get_emote_state(&ev.emote, &ev.arg, &ev.result, &ev.requested)) {
		emote_reaction r = emote_react(&ev, player_name(game), &game->player.emote);

		if(r.end_local)
			emote_end(&game->player.emote);
		say(game, r.say, r.text);
	}
}

void emote_client_update(GameState* game, float dt) {
	float durations[CHARACTER_ANIM_COUNT];
	int count;

	emote_client_durations(&game->player.emote, durations);
	emote_update(&game->player.emote, dt, durations);
	if(!game->playing)
		return;

	count = game->playing->nearby_player_count;
	if(count > MAX_NEARBY_PLAYERS)
		count = MAX_NEARBY_PLAYERS;

	for(int i = 0; i < count; i++) {
		emote_state* other = &game->playing->nearby_players[i].emote;

		emote_client_durations(other, durations);
		emote_update(other, dt, durations);
	}
}
