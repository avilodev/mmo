// Which status the server shows for a player.

#include "status_rules.h"

uint8_t status_effective(uint8_t choice, double last_activity_at, double now) {
	if(choice == SESSION_STATUS_AFK || choice == SESSION_STATUS_DND ||
	   choice == SESSION_STATUS_OFFLINE)
		return choice;

	if(now - last_activity_at >= AFK_IDLE_SECONDS)
		return SESSION_STATUS_AFK;

	return SESSION_STATUS_ONLINE;
}
