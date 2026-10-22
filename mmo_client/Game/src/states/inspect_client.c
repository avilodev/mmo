// Asks the server about another player's gear and stats, and opens the window.

#include "states/inspect_client.h"
#include "states/chat_log.h"
#include "network/network.h"
#include "ui/character_screen.h"

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

// Space between the character window and the inspect window opened beside it.
#define INSPECT_WINDOW_GAP 20.0f

static void say(GameState* game, const char* text) {
	chat_log_add(&game->playing->chat, CHAT_CHANNEL_SYSTEM, CHAT_STYLE_ERROR, "", text);
}

void inspect_client_request_target(GameState* game) {
	InspectData* inspect;

	if(!game->playing)
		return;

	if(game->playing->target_player_id == 0) {
		say(game, "Target a player to inspect them.");
		return;
	}

	inspect = &game->playing->inspect;
	inspect->target_id = game->playing->target_player_id;
	inspect->pending = 1;
	network_send_inspect_request(inspect->target_id);
}

// Beside the character window when that is open, else where the window starts.
static void open_window(GameState* game) {
	CharacterScreenState* window = game->inspect_screen;
	const CharacterScreenState* own = game->character_screen;

	if(!window)
		return;
	if(window->is_open)
		return;

	if(own && own->is_open) {
		character_screen_place(window, own->window_x + own->window_width + INSPECT_WINDOW_GAP,
		                       own->window_y);
	}
	character_screen_toggle(window);
}

static const char* refusal_text(uint8_t result) {
	if(result == INSPECT_TOO_FAR)
		return "That player is too far away to inspect.";

	return "You can't inspect that player.";
}

void inspect_client_poll(GameState* game) {
	InspectResponsePacket pkt;
	InspectData* inspect;

	if(!game->playing)
		return;

	inspect = &game->playing->inspect;
	while(network_get_inspect_response(&pkt)) {
		// an answer to a question this window did not ask is stale
		if(!inspect->pending || ntohl(pkt.target_id) != inspect->target_id)
			continue;

		inspect->pending = 0;
		if(character_view_from_inspect(&pkt, &inspect->view))
			open_window(game);
		else
			say(game, refusal_text(pkt.result));
	}
}

void inspect_client_close(GameState* game) {
	if(!game->playing)
		return;

	game->playing->inspect.pending = 0;
	if(game->inspect_screen && game->inspect_screen->is_open)
		character_screen_toggle(game->inspect_screen);
}
