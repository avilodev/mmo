/**
 * @file
 * Load and validate NPC shop inventories.
 *
 * Content only: reading shops.json, holding the registry, and checking what it
 * says for internal consistency. The packet handlers that spend a player's coin
 * against it live in shop_trade.c -- they need the player registry, the NPC
 * pool and the open-shop table, and none of that belongs behind a JSON parser.
 * Keeping them apart is also what lets the content tests link shop_validate()
 * without dragging the whole world in.
 */

#include "shop.h"
#include "json_util.h"
#include "log.h"
#include "items_database.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ShopDef g_shops[MAX_SHOPS];
static int     g_shop_count = 0;

ShopDef* shop_find(uint32_t shop_id) {
    for (int i = 0; i < g_shop_count; i++)
        if (g_shops[i].shop_id == shop_id) return &g_shops[i];
    return NULL;
}

/**
 * Read one shop object into a definition.
 *
 * @return 1 when the object carried a usable shop, otherwise 0.
 */
static int shop_read_one(const JsonValue* obj, ShopDef* s) {
    memset(s, 0, sizeof(*s));

    s->shop_id = (uint32_t)json_get_int(obj, "shop_id", 0);
    if (s->shop_id == 0) return 0;

    snprintf(s->name, sizeof(s->name), "%s", json_get_string(obj, "name", ""));

    /* "currency" names the kingdom whose coin this shop deals in, by
     * CurrencyId. A shop that omits it, or names one that does not exist,
     * trades in Ennara's coin. */
    int currency = json_get_int(obj, "currency", (int)CURRENCY_ENNARA);
    if (!world_currency_valid(currency)) {
        LOG_WARN("[SHOP] shop %u names currency %d, which does not exist — "
                 "trading in Ennara's coin", s->shop_id, currency);
        currency = (int)CURRENCY_ENNARA;
    }
    s->currency_id = (uint8_t)currency;

    const JsonValue* items = json_get(obj, "items");
    int declared = json_count(items);
    for (int i = 0; i < declared && s->item_count < MAX_SHOP_ITEMS; i++) {
        const JsonValue* it = json_at(items, i);
        ShopItemEntry* e = &s->items[s->item_count];

        e->item_id   = (uint32_t)json_get_int(it, "item_id", 0);
        e->buy_price = (uint32_t)json_get_int(it, "buy_price", 0);

        /* An omitted buy_price falls back to twice the item's value, and never
         * to zero: a zero price is a free item, and shop_validate() would
         * reject it as mintable anyway. */
        if (e->buy_price == 0) {
            const ItemDefinition* def = item_get(e->item_id);
            if (def) e->buy_price = def->value * 2;
            if (e->buy_price == 0) e->buy_price = 1;
        }
        s->item_count++;
    }

    if (declared > s->item_count)
        LOG_WARN("[SHOP] shop %u '%s' lists %d items; %d fit and %d were dropped "
                 "(MAX_SHOP_ITEMS is %d)",
                 s->shop_id, s->name, declared, s->item_count,
                 declared - s->item_count, MAX_SHOP_ITEMS);

    return 1;
}

/**
 * Initialize shop definitions from a JSON file.
 *
 * A missing file or shops array is treated as a nonfatal empty registry: a
 * deployment with no merchants is a deployment with no merchants, not a broken
 * one. A file that is present but malformed is *not* treated that way, because
 * that is content someone wrote and expects to be live.
 *
 * Built on the shared JSON tree parser. What it replaced looked a key up with
 * strstr() from the start of the object it was reading, which does not stop at
 * that object's closing brace -- so a shop that omitted "currency" silently
 * took the next shop's, and a shop that omitted "items" took the next shop's
 * entire stock and sold it at that shop's prices. Both are content bugs with
 * no symptom until someone notices the wrong things on a merchant's list. It
 * is the same defect the dialogue loader was carrying, fixed the same way; see
 * the note at the top of dialogue_loader.c.
 *
 * @return 1 after loading or a nonfatal absence, or 0 when the file is present
 *         and unreadable as JSON.
 */
int shop_init(const char* json_path) {
    g_shop_count = 0;

    const char* err = NULL;
    JsonValue* doc = json_parse_file(json_path, &err);
    if (!doc) {
        LOG_ERROR("[SHOP] Cannot load %s: %s", json_path, err ? err : "unknown error");
        return 1;  // non-fatal: no merchants rather than no world
    }

    const JsonValue* shops = json_get(doc, "shops");
    int declared = json_count(shops);

    for (int i = 0; i < declared && g_shop_count < MAX_SHOPS; i++) {
        ShopDef* s = &g_shops[g_shop_count];
        if (!shop_read_one(json_at(shops, i), s)) {
            LOG_WARN("[SHOP] entry %d of %s has no usable shop_id — skipped", i, json_path);
            continue;
        }
        LOG_INFO("[SHOP] Loaded shop %u '%s' (%u items)", s->shop_id, s->name, s->item_count);
        g_shop_count++;
    }

    if (declared > g_shop_count)
        LOG_WARN("[SHOP] %s declares %d shops; %d were loaded and %d dropped "
                 "(MAX_SHOPS is %d)",
                 json_path, declared, g_shop_count, declared - g_shop_count, MAX_SHOPS);

    json_free(doc);
    LOG_INFO("[SHOP] %d shops loaded", g_shop_count);
    return 1;
}

/**
 * Validate shop item references and buy-versus-resale prices.
 *
 * @return The number of invalid or exploitable entries, or 0 for valid content.
 */
int shop_validate(void) {
    int problems = 0;

    for (int s = 0; s < g_shop_count; s++) {
        ShopDef* shop = &g_shops[s];
        for (int i = 0; i < shop->item_count; i++) {
            const ShopItemEntry* entry = &shop->items[i];
            const ItemDefinition* def = item_get(entry->item_id);

            if (!def) {
                LOG_ERROR("[SHOP] shop %u '%s' lists item %u, which is not in items.json",
                          shop->shop_id, shop->name, entry->item_id);
                problems++;
                continue;
            }

            // reject entries permitting profitable immediate resale
            if (entry->buy_price <= def->value) {
                LOG_ERROR("[SHOP] shop %u sells '%s' for %u but it resells for %u "
                          "— buying and reselling would mint coin",
                          shop->shop_id, def->name, entry->buy_price, def->value);
                problems++;
            }
        }
    }

    if (problems == 0)
        LOG_INFO("[SHOP] validated %d shops, no content problems", g_shop_count);
    return problems;
}

/**
 * Clear the shop registry.
 */
void shop_cleanup(void) { g_shop_count = 0; }

