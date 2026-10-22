// Turn an inspect reply into something a character panel can draw.

#include "ui/character_view.h"

#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

int character_view_from_inspect(const InspectResponsePacket* pkt, CharacterView* out) {
	CharacterView view;

	if(pkt->result != INSPECT_OK)
		return 0;
	if(pkt->form >= FORM_COUNT)
		return 0;

	memset(&view, 0, sizeof(view));
	view.loaded = 1;
	memcpy(view.name, pkt->name, sizeof(view.name) - 1);
	view.level = ntohl(pkt->level);
	view.race_id = ntohl(pkt->race_id);
	view.form = pkt->form;
	view.weapon_damage = (int32_t)ntohl((uint32_t)pkt->weapon_damage);

	for(int i = 0; i < EQUIP_SLOTS; i++)
		view.equipment[i] = ntohl(pkt->equipment[i]);
	for(int i = 0; i < STAT_COUNT; i++)
		view.stats[i] = (int32_t)ntohl((uint32_t)pkt->stats[i]);

	*out = view;

	return 1;
}
