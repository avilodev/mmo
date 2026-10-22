#ifndef CHARACTER_VIEW_H
#define CHARACTER_VIEW_H

// What a character panel shows: yourself, or a player you inspected.

#include "protocol.h"

typedef struct {
	int loaded; // 0 until there is something to show
	char name[32];
	uint32_t level;
	uint32_t race_id;
	uint8_t form; // PlayerForm
	uint32_t equipment[EQUIP_SLOTS]; // item ids, indexed by EquipSlotIndex
	int32_t stats[STAT_COUNT]; // indexed by StatId
	int32_t weapon_damage;
} CharacterView;

// The inspect window: who was asked about, and what came back.
typedef struct {
	CharacterView view;
	uint32_t target_id; // character the last request named
	uint8_t pending; // asked and not yet answered
} InspectData;

// Fill a view from an inspect reply. Returns 0, leaving *out untouched, for any
// reply that is not INSPECT_OK or that carries a value the client cannot show.
int character_view_from_inspect(const InspectResponsePacket* pkt, CharacterView* out);

#endif /* CHARACTER_VIEW_H */
