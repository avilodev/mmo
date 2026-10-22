#ifndef CHARACTER_RECORD_H
#define CHARACTER_RECORD_H

// One character's saved scalars, as storage reads and writes them.
//
// This is not a packet. The wire layout of a character lives in protocol.h and
// can change with the client; this record changes with the database.

#include "protocol.h"

#include <stdint.h>

typedef struct {
	uint32_t character_id;
	char name[32];
	uint32_t level;
	float pos_x;
	float pos_y;
	float pos_z;
	uint32_t health;
	uint32_t max_health;
	int32_t resource;
	int32_t max_resource;
	uint64_t experience;

	// balance per kingdom, indexed by CurrencyId
	uint32_t currency[CURRENCY_COUNT];

	// fused race and class identifier
	uint32_t race_id;

	// body and fur colour, packed by appearance_pack
	uint32_t appearance;
} character_record;

#endif /* CHARACTER_RECORD_H */
