// The client item database: the generated items.json, its parser and the tooltips.

#include "player/item_db.h"
#include "core/race_registry.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define TEMP_PATH "/tmp/mmo_item_db_test_items.json"

static void write_temp(const char* json) {
	FILE* f = fopen(TEMP_PATH, "wb");
	assert(f);
	fputs(json, f);
	fclose(f);
}

// True when some line of the tooltip equals text
static int has_line(char lines[][ITEM_TOOLTIP_LINE_LEN], int count, const char* text) {
	for(int i = 0; i < count; i++) {
		if(strcmp(lines[i], text) == 0)
			return 1;
	}
	return 0;
}

static void test_real_file(void) {
	assert(item_db_load(ITEM_DB_PATH) == 7);

	const ItemTemplate* vest = item_db_get(1003);
	assert(vest);
	assert(strcmp(vest->name, "Bearhide Vest") == 0);
	assert(vest->description[0] != '\0');
	assert(vest->type == ITEM_TYPE_EQUIPMENT);
	assert(vest->equip_slot == EQUIP_SLOT_CHEST);
	assert(vest->rarity == ITEM_RARITY_RARE);
	assert(vest->defense == 8);
	assert(vest->level_req == 5);
	assert(vest->bind_on_equip == 1 && vest->bind_on_pickup == 0);
	assert(vest->bonus_stats[STAT_VITALITY] == 5);
	assert(vest->bonus_stats[STAT_ENDURANCE] == 2);
	assert(vest->bonus_stats[STAT_STRENGTH] == 0);
	assert(vest->vendor_price == 120);

	const ItemTemplate* totem = item_db_get(20000);
	assert(totem && totem->bind_on_pickup == 1);
	assert(totem->equip_slot == EQUIP_SLOT_SECOND_HAND);
	assert(totem->bonus_stats[STAT_FERALITY] == 6);
	assert(totem->bonus_stats[STAT_FEROCITY] == 3);

	const ItemTemplate* tonic = item_db_get(50000);
	assert(tonic && tonic->type == ITEM_TYPE_CONSUMABLE);
	assert(tonic->hp_restore == 40 && tonic->mp_restore == 0);
	assert(tonic->use_cooldown == 10.0f);
	assert(tonic->max_stack == 20);
	assert(tonic->vendor_price == 5);

	const ItemTemplate* cap = item_db_get(1001);
	assert(cap && cap->max_stack == 1 && cap->level_req == 0);

	for(uint32_t id = 0; id < 60000; id++) {
		const ItemTemplate* item = item_db_get(id);
		if(item)
			assert(item->description[0] != '\0');
	}
}

static void test_missing_and_bad_files(void) {
	assert(item_db_load("/tmp/mmo_item_db_test_does_not_exist.json") == 0);
	assert(item_db_get(1003) == NULL);
	assert(strcmp(item_db_get_name(1003), "Unknown Item") == 0);

	write_temp("{ \"no_items_here\": [] }");
	assert(item_db_load(TEMP_PATH) == 0);

	write_temp("{ \"items\": [] }");
	assert(item_db_load(TEMP_PATH) == 0);
}

static void test_parser_defaults(void) {
	write_temp("{ \"items\": [\n"
	           "  { \"id\": 7, \"name\": \"Plain\", \"rarity\": \"epic\" },\n"
	           "  { \"id\": 8, \"name\": \"Both\", \"type\": \"consumable\", \"bind_on_pickup\": true,\n"
	           "    \"use_effect\": \"restore_both\", \"use_value\": 25, \"value\": 0 },\n"
	           "  { \"id\": 9, \"name\": \"Mana\", \"use_effect\": \"restore_mana\", \"use_value\": 9,\n"
	           "    \"bonus_armor\": -2 }\n"
	           "] }");
	assert(item_db_load(TEMP_PATH) == 3);
	assert(strcmp(item_db_get_name(8), "Both") == 0);

	const ItemTemplate* plain = item_db_get(7);
	assert(plain->max_stack == 1);
	assert(plain->vendor_price == 40); // no value: derived from the epic rarity
	assert(plain->description[0] == '\0');

	const ItemTemplate* both = item_db_get(8);
	assert(both->bind_on_pickup == 1);
	assert(both->hp_restore == 25 && both->mp_restore == 25);
	assert(both->vendor_price == 0); // an explicit zero is kept

	const ItemTemplate* mana = item_db_get(9);
	assert(mana->hp_restore == 0 && mana->mp_restore == 9);
	assert(mana->bonus_stats[STAT_ARMOR] == -2);
}

static void test_tooltip(void) {
	char lines[ITEM_TOOLTIP_MAX_LINES][ITEM_TOOLTIP_LINE_LEN];

	assert(item_db_load(ITEM_DB_PATH) == 7);
	assert(item_tooltip_lines(NULL, lines, ITEM_TOOLTIP_MAX_LINES) == 0);

	const ItemTemplate* vest = item_db_get(1003);
	int n = item_tooltip_lines(vest, lines, ITEM_TOOLTIP_MAX_LINES);
	assert(has_line(lines, n, "Chest"));
	assert(has_line(lines, n, "Binds when equipped"));
	assert(has_line(lines, n, "Requires level 5"));
	assert(has_line(lines, n, "+8 Defense"));
	assert(has_line(lines, n, "+5 Vitality"));
	assert(has_line(lines, n, "+2 Endurance"));
	assert(has_line(lines, n, "Value: 120g"));
	assert(!has_line(lines, n, "Binds when picked up"));

	const ItemTemplate* tonic = item_db_get(50000);
	n = item_tooltip_lines(tonic, lines, ITEM_TOOLTIP_MAX_LINES);
	assert(has_line(lines, n, "Consumable"));
	assert(has_line(lines, n, "Restores 40 health"));
	assert(has_line(lines, n, "Cooldown: 10 seconds"));

	const ItemTemplate* totem = item_db_get(20000);
	n = item_tooltip_lines(totem, lines, ITEM_TOOLTIP_MAX_LINES);
	assert(has_line(lines, n, "Off Hand"));
	assert(has_line(lines, n, "Binds when picked up"));
	assert(has_line(lines, n, "+6 Ferality"));

	// A short buffer is filled and never overrun
	assert(item_tooltip_lines(vest, lines, 2) == 2);
}

// Wire form of a race id: big-endian, as the server sends it
static uint32_t wire(uint32_t host) {
	return ((host & 0xFFu) << 24) | ((host & 0xFF00u) << 8) |
	       ((host >> 8) & 0xFF00u) | (host >> 24);
}

static void test_race_requirements(void) {
	char lines[ITEM_TOOLTIP_MAX_LINES][ITEM_TOOLTIP_LINE_LEN];
	RaceListResponsePacket packet;

	memset(&packet, 0, sizeof(packet));
	packet.count = 2;
	packet.races[0].race_id = wire(3);
	snprintf(packet.races[0].name, sizeof(packet.races[0].name), "Wolf");
	packet.races[1].race_id = wire(5);
	snprintf(packet.races[1].name, sizeof(packet.races[1].name), "Bear");
	client_races_store(&packet);

	write_temp("{ \"items\": [\n"
	           "  { \"id\": 1, \"name\": \"Mantle\", \"race_req\": [3, 5] },\n"
	           "  { \"id\": 2, \"name\": \"Odd\", \"race_req\": [3,9] },\n"
	           "  { \"id\": 3, \"name\": \"Open\" },\n"
	           "  { \"id\": 4, \"name\": \"Long\", \"race_req\": [1,2,3,4,5,6] }\n"
	           "] }");
	assert(item_db_load(TEMP_PATH) == 4);

	const ItemTemplate* mantle = item_db_get(1);
	assert(mantle->race_req[0] == 3 && mantle->race_req[1] == 5 && mantle->race_req[2] == 0);
	assert(item_db_get(3)->race_req[0] == 0);
	assert(item_db_get(4)->race_req[3] == 4);

	int n = item_tooltip_lines(mantle, lines, ITEM_TOOLTIP_MAX_LINES);
	assert(has_line(lines, n, "Races: Wolf, Bear"));

	// A race the server never listed shows as its number rather than vanishing
	n = item_tooltip_lines(item_db_get(2), lines, ITEM_TOOLTIP_MAX_LINES);
	assert(has_line(lines, n, "Races: Wolf, #9"));

	n = item_tooltip_lines(item_db_get(3), lines, ITEM_TOOLTIP_MAX_LINES);
	for(int i = 0; i < n; i++)
		assert(strncmp(lines[i], "Races:", 6) != 0);

	// With no race list at all every id shows as a number, and the line still fits
	client_races_store(NULL);
	n = item_tooltip_lines(item_db_get(4), lines, ITEM_TOOLTIP_MAX_LINES);
	assert(has_line(lines, n, "Races: #1, #2, #3, #4"));
}

int main(void) {
	test_real_file();
	test_missing_and_bad_files();
	test_parser_defaults();
	test_tooltip();
	test_race_requirements();
	remove(TEMP_PATH);
	printf("item_db_test: OK\n");
	return 0;
}
