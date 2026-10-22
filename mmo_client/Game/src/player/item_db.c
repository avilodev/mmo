/**
 * @file
 * Load the client's item templates and describe them for tooltips.
 *
 * The file is generated from the server's items.json by tools/sync_items.py,
 * so the field names here are the server's own.
 */

#include "player/item_db.h"
#include "core/client_log.h"
#include "core/race_registry.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Most templates the client keeps
#define MAX_ITEM_TEMPLATES 1000

static ItemTemplate g_item_db[MAX_ITEM_TEMPLATES];
static int g_item_db_count = 0;

// JSON key suffix and tooltip label per StatId, in enum order
static const char* const k_stat_keys[STAT_COUNT] = {
	"strength", "dexterity", "vitality", "intelligence", "focus", "endurance",
	"ferocity", "stamina_capacity", "precision", "ferality", "armor"
};

static const char* const k_stat_names[STAT_COUNT] = {
	"Strength", "Dexterity", "Vitality", "Intelligence", "Focus", "Endurance",
	"Ferocity", "Stamina Capacity", "Precision", "Ferality", "Armor"
};

static char* read_file(const char* path) {
	FILE* f = fopen(path, "rb");
	if(!f)
		return NULL;

	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if(size < 0) {
		fclose(f);
		return NULL;
	}

	char* buffer = (char*)malloc((size_t)size + 1);
	if(!buffer) {
		fclose(f);
		return NULL;
	}

	size_t got = fread(buffer, 1, (size_t)size, f);
	buffer[got] = '\0';
	fclose(f);
	return buffer;
}

// Start of a key's value inside one object, or NULL when the key is absent
static const char* find_value(const char* obj, const char* key) {
	char search[64];
	snprintf(search, sizeof(search), "\"%s\"", key);

	const char* pos = strstr(obj, search);
	if(!pos)
		return NULL;

	pos = strchr(pos, ':');
	if(!pos)
		return NULL;

	pos++;
	while(*pos && isspace((unsigned char)*pos))
		pos++;

	return pos;
}

static int read_int(const char* obj, const char* key, int fallback) {
	const char* value = find_value(obj, key);
	return value ? atoi(value) : fallback;
}

// The server accepts true or 1 for a flag
static uint8_t read_flag(const char* obj, const char* key) {
	const char* value = find_value(obj, key);
	return (value && (*value == 't' || *value == '1')) ? 1 : 0;
}

// Copy a string value into out, cut to fit; returns 0 when absent
static int read_string(const char* obj, const char* key, char* out, size_t cap) {
	const char* value = find_value(obj, key);
	if(!value || *value != '"')
		return 0;

	const char* end = strchr(value + 1, '"');
	if(!end)
		return 0;

	size_t len = (size_t)(end - (value + 1));
	if(len >= cap)
		len = cap - 1;

	memcpy(out, value + 1, len);
	out[len] = '\0';
	return 1;
}

static EquipSlot parse_slot(const char* slot) {
	if(strcmp(slot, "helmet") == 0)
		return EQUIP_SLOT_HELMET;
	if(strcmp(slot, "gloves") == 0)
		return EQUIP_SLOT_GLOVES;
	if(strcmp(slot, "chest") == 0)
		return EQUIP_SLOT_CHEST;
	if(strcmp(slot, "leggings") == 0)
		return EQUIP_SLOT_LEGGINGS;
	if(strcmp(slot, "boots") == 0)
		return EQUIP_SLOT_BOOTS;
	if(strcmp(slot, "main_hand") == 0)
		return EQUIP_SLOT_MAIN_HAND;
	if(strcmp(slot, "off_hand") == 0)
		return EQUIP_SLOT_SECOND_HAND;
	if(strcmp(slot, "two_handed") == 0)
		return EQUIP_SLOT_MAIN_HAND;
	return EQUIP_SLOT_NONE;
}

static ItemType parse_type(const char* type) {
	if(strcmp(type, "consumable") == 0)
		return ITEM_TYPE_CONSUMABLE;
	if(strcmp(type, "weapon") == 0 || strcmp(type, "shield") == 0 ||
	   strcmp(type, "armor") == 0)
		return ITEM_TYPE_EQUIPMENT;
	if(strcmp(type, "quest") == 0)
		return ITEM_TYPE_QUEST;
	return ITEM_TYPE_MATERIAL;
}

static ItemRarity parse_rarity(const char* rarity) {
	if(strcmp(rarity, "uncommon") == 0)
		return ITEM_RARITY_UNCOMMON;
	if(strcmp(rarity, "rare") == 0)
		return ITEM_RARITY_RARE;
	if(strcmp(rarity, "epic") == 0)
		return ITEM_RARITY_EPIC;
	if(strcmp(rarity, "legendary") == 0)
		return ITEM_RARITY_LEGENDARY;
	return ITEM_RARITY_COMMON;
}

// What a consumable restores, from the server's use_effect and use_value
static void parse_use_effect(const char* obj, ItemTemplate* item) {
	char effect[32];
	if(!read_string(obj, "use_effect", effect, sizeof(effect)))
		return;

	int amount = read_int(obj, "use_value", 0);
	if(strcmp(effect, "restore_health") == 0 || strcmp(effect, "restore_both") == 0)
		item->hp_restore = (int16_t)amount;
	if(strcmp(effect, "restore_mana") == 0 || strcmp(effect, "restore_both") == 0)
		item->mp_restore = (int16_t)amount;
}

// Read a JSON array of small ids into out, stopping when out is full
static void read_id_list(const char* obj, const char* key, uint8_t* out, int max) {
	const char* value = find_value(obj, key);
	if(!value || *value != '[')
		return;

	const char* pos = value + 1;
	int count = 0;
	while(*pos && *pos != ']' && count < max) {
		while(*pos && !isdigit((unsigned char)*pos) && *pos != ']')
			pos++;
		if(*pos == ']')
			break;
		out[count++] = (uint8_t)atoi(pos);
		while(*pos && isdigit((unsigned char)*pos))
			pos++;
	}
}

static void parse_item(const char* obj, ItemTemplate* item) {
	char text[32];

	memset(item, 0, sizeof(*item));
	item->id = (uint32_t)read_int(obj, "id", 0);
	read_string(obj, "name", item->name, sizeof(item->name));
	read_string(obj, "description", item->description, sizeof(item->description));

	if(read_string(obj, "type", text, sizeof(text)))
		item->type = parse_type(text);
	if(read_string(obj, "slot", text, sizeof(text)))
		item->equip_slot = parse_slot(text);
	if(read_string(obj, "rarity", text, sizeof(text)))
		item->rarity = parse_rarity(text);

	item->max_stack = (uint16_t)read_int(obj, "max_stack", 1);
	item->damage = (int16_t)read_int(obj, "damage", 0);
	item->defense = (int16_t)read_int(obj, "defense", 0);
	item->level_req = (uint8_t)read_int(obj, "level_req", 0);
	item->bind_on_pickup = read_flag(obj, "bind_on_pickup");
	item->bind_on_equip = read_flag(obj, "bind_on_equip");
	read_id_list(obj, "race_req", item->race_req, (int)sizeof(item->race_req));

	const char* cooldown = find_value(obj, "use_cooldown");
	if(cooldown)
		item->use_cooldown = (float)atof(cooldown);

	for(int stat = 0; stat < STAT_COUNT; stat++) {
		char key[48];
		snprintf(key, sizeof(key), "bonus_%s", k_stat_keys[stat]);
		item->bonus_stats[stat] = (int16_t)read_int(obj, key, 0);
	}

	parse_use_effect(obj, item);

	// An absent value falls back to what the server derives from rarity
	item->vendor_price = (uint32_t)read_int(obj, "value", ((int)item->rarity + 1) * 10);
	item->sprite_id = (uint16_t)item->id;
}

// Parse every object of the items array; returns 0 when there is no array
static int parse_items_json(const char* json) {
	const char* items_key = strstr(json, "\"items\"");
	if(!items_key) {
		CLOG_WARN("[ITEM_DB] No 'items' array found in JSON");
		return 0;
	}

	const char* pos = strchr(items_key, '[');
	if(!pos)
		return 0;

	pos++;
	while(*pos && *pos != ']') {
		if(*pos != '{') {
			pos++;
			continue;
		}

		int depth = 0;
		const char* end = pos;
		while(*end) {
			if(*end == '{')
				depth++;
			if(*end == '}' && --depth == 0)
				break;
			end++;
		}
		if(depth != 0)
			break;

		if(g_item_db_count >= MAX_ITEM_TEMPLATES) {
			CLOG_WARN("[ITEM_DB] Max items reached (%d)", MAX_ITEM_TEMPLATES);
			break;
		}

		size_t len = (size_t)(end - pos) + 1;
		char* obj = (char*)malloc(len + 1);
		if(!obj) {
			CLOG_ERROR("[ITEM_DB] Out of memory parsing items");
			break;
		}

		memcpy(obj, pos, len);
		obj[len] = '\0';
		ItemTemplate* item = &g_item_db[g_item_db_count++];
		parse_item(obj, item);
		CLOG_DEBUG("[ITEM_DB]   Loaded: [%u] %s", item->id, item->name);
		free(obj);
		pos = end + 1;
	}

	return 1;
}

int item_db_load(const char* path) {
	g_item_db_count = 0;
	memset(g_item_db, 0, sizeof(g_item_db));

	char* json = read_file(path);
	if(!json)
		return 0;

	int ok = parse_items_json(json);
	free(json);
	return ok ? g_item_db_count : 0;
}

void item_db_init(void) {
	int count = item_db_load(ITEM_DB_PATH);
	if(count == 0) {
		CLOG_ERROR("[ITEM_DB] No items loaded from %s; names and tooltips will "
		           "be missing (run tools/sync_items.py)", ITEM_DB_PATH);
		return;
	}

	CLOG_INFO("[ITEM_DB] Loaded %d items from %s", count, ITEM_DB_PATH);
}

const ItemTemplate* item_db_get(uint32_t item_id) {
	for(int i = 0; i < g_item_db_count; i++) {
		if(g_item_db[i].id == item_id)
			return &g_item_db[i];
	}
	return NULL;
}

const char* item_db_get_name(uint32_t item_id) {
	const ItemTemplate* item = item_db_get(item_id);
	return item ? item->name : "Unknown Item";
}

static const char* kind_label(const ItemTemplate* item) {
	static const char* const slots[EQUIP_SLOT_COUNT] = {
		NULL, "Helmet", "Chest", "Gloves", "Leggings", "Boots", "Main Hand", "Off Hand"
	};

	if(item->equip_slot > EQUIP_SLOT_NONE && item->equip_slot < EQUIP_SLOT_COUNT)
		return slots[item->equip_slot];
	if(item->type == ITEM_TYPE_CONSUMABLE)
		return "Consumable";
	if(item->type == ITEM_TYPE_QUEST)
		return "Quest Item";
	return NULL;
}

// Append one formatted line while there is room
static void add_line(char lines[][ITEM_TOOLTIP_LINE_LEN], int* count, int max_lines,
                     const char* format, ...) {
	if(*count >= max_lines)
		return;

	va_list args;
	va_start(args, format);
	vsnprintf(lines[*count], ITEM_TOOLTIP_LINE_LEN, format, args);
	va_end(args);
	(*count)++;
}

// "Races: Wolf, Bear", by the names the server sent; an unknown id shows as a number
static void add_race_line(char lines[][ITEM_TOOLTIP_LINE_LEN], int* count, int max_lines,
                          const ItemTemplate* item) {
	char text[ITEM_TOOLTIP_LINE_LEN] = "Races: ";
	size_t used = strlen(text);

	for(int i = 0; i < (int)sizeof(item->race_req) && item->race_req[i] != 0; i++) {
		const RaceInfo* race = client_race_find(item->race_req[i]);
		char name[32];
		if(race)
			snprintf(name, sizeof(name), "%s", race->name);
		else
			snprintf(name, sizeof(name), "#%u", item->race_req[i]);

		if(used < sizeof(text))
			used += (size_t)snprintf(text + used, sizeof(text) - used, "%s%s",
			                         i > 0 ? ", " : "", name);
	}

	add_line(lines, count, max_lines, "%s", text);
}

int item_tooltip_lines(const ItemTemplate* item,
                       char lines[][ITEM_TOOLTIP_LINE_LEN], int max_lines) {
	int n = 0;
	if(!item)
		return 0;

	const char* kind = kind_label(item);
	if(kind)
		add_line(lines, &n, max_lines, "%s", kind);
	if(item->bind_on_pickup)
		add_line(lines, &n, max_lines, "Binds when picked up");
	if(item->bind_on_equip)
		add_line(lines, &n, max_lines, "Binds when equipped");
	if(item->level_req > 0)
		add_line(lines, &n, max_lines, "Requires level %d", item->level_req);
	if(item->race_req[0] != 0)
		add_race_line(lines, &n, max_lines, item);
	if(item->damage > 0)
		add_line(lines, &n, max_lines, "+%d Damage", item->damage);
	if(item->defense > 0)
		add_line(lines, &n, max_lines, "+%d Defense", item->defense);

	for(int stat = 0; stat < STAT_COUNT; stat++) {
		if(item->bonus_stats[stat] != 0)
			add_line(lines, &n, max_lines, "%+d %s", item->bonus_stats[stat],
			         k_stat_names[stat]);
	}

	if(item->hp_restore > 0)
		add_line(lines, &n, max_lines, "Restores %d health", item->hp_restore);
	if(item->mp_restore > 0)
		add_line(lines, &n, max_lines, "Restores %d mana", item->mp_restore);
	if(item->use_cooldown > 0.0f)
		add_line(lines, &n, max_lines, "Cooldown: %g seconds", item->use_cooldown);
	if(item->vendor_price > 0)
		add_line(lines, &n, max_lines, "Value: %ug", item->vendor_price);

	return n;
}
