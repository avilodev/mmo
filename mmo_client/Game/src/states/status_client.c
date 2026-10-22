// Follows the status the server shows for the player, and says when it changes.

#include "states/status_client.h"
#include "core/session_status.h"
#include "states/chat_log.h"
#include "network/network.h"

#include <stdio.h>

void status_client_poll(GameState* game) {
	uint8_t status;
	uint8_t was;
	char text[64];

	if(!game->playing)
		return;

	while(network_get_status_state(&status)) {
		was = game->playing->own_status;
		game->playing->own_status = status;

		if(status == was)
			continue;

		if(was == SESSION_STATUS_AFK && status == SESSION_STATUS_ONLINE)
			snprintf(text, sizeof(text), "You are no longer AFK.");
		else
			snprintf(text, sizeof(text), "You are now %s.", session_status_label(status));

		chat_log_add(&game->playing->chat, CHAT_CHANNEL_SYSTEM, CHAT_STYLE_SYSTEM, "", text);
	}
}
