/**
 * @file
 * Load NPC shop inventories and handle shop-open, purchase, and sale requests.
 */

#include "shop.h"
#include "log.h"
#include "player_data.h"
#include "items_database.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>

static ShopDef g_shops[MAX_SHOPS];
static int     g_shop_count = 0;

static const char* sh_skip_ws(const char* s) {
    while (*s && isspace((unsigned char)*s)) s++;
    return s;
}
static const char* sh_find_value(const char* json, const char* key) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char* pos = strstr(json, search);
    if (!pos) return NULL;
    pos = strchr(pos, ':');
    if (!pos) return NULL;
    return sh_skip_ws(pos + 1);
}
static int sh_parse_string(const char* val, char* out, int sz) {
    val = sh_skip_ws(val);
    if (*val != '"') return 0;
    val++;
    int i = 0;
    while (*val && *val != '"' && i < sz - 1) {
        if (*val == '\\' && *(val+1)) { val++; out[i++] = *val++; }
        else out[i++] = *val++;
    }
    out[i] = '\0';
    return 1;
}
static int sh_parse_int(const char* val) { return atoi(sh_skip_ws(val)); }
static const char* sh_find_array(const char* json, const char* key) {
    const char* val = sh_find_value(json, key);
    if (!val) return NULL;
    val = sh_skip_ws(val);
    if (*val != '[') return NULL;
    return val + 1;
}
static const char* sh_first_element(const char* pos) {
    pos = sh_skip_ws(pos);
    return (*pos == '{') ? pos : NULL;
}

static const char* sh_next_element(const char* pos) {
    pos = sh_skip_ws(pos);
    if (*pos == '{') {
        int d = 1; pos++;
        while (*pos && d > 0) { if (*pos == '{') d++; else if (*pos == '}') d--; pos++; }
    }
    pos = sh_skip_ws(pos);
    if (*pos == ',') pos = sh_skip_ws(pos + 1);
    if (*pos == ']' || *pos == '\0') return NULL;
    return (*pos == '{') ? pos : NULL;
}

static ShopDef* shop_find(uint32_t shop_id) {
    for (int i = 0; i < g_shop_count; i++)
        if (g_shops[i].shop_id == shop_id) return &g_shops[i];
    return NULL;
}

/**
 * Initialize shop definitions from a JSON file.
 *
 * A missing file or shops array is treated as a nonfatal empty registry.
 *
 * @return 1 after loading or a nonfatal absence, or 0 on allocation failure.
 */
int shop_init(const char* json_path) {
    g_shop_count = 0;

    FILE* f = fopen(json_path, "r");
    if (!f) {
        LOG_ERROR("[SHOP] Cannot open %s: %s", json_path, strerror(errno));
        return 1;  // non-fatal
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char* buf = malloc(sz + 1);
    if (!buf) { fclose(f); return 0; }
    fread(buf, 1, sz, f);
    buf[sz] = '\0';
    fclose(f);

    const char* shops_arr = sh_find_array(buf, "shops");
    if (!shops_arr) { free(buf); return 1; }

    const char* obj = sh_first_element(shops_arr);
    for (; obj != NULL && g_shop_count < MAX_SHOPS; obj = sh_next_element(obj)) {
        ShopDef* s = &g_shops[g_shop_count];
        memset(s, 0, sizeof(*s));

        const char* v;
        v = sh_find_value(obj, "shop_id"); if (v) s->shop_id = sh_parse_int(v);
        v = sh_find_value(obj, "name");    if (v) sh_parse_string(v, s->name, sizeof(s->name));

        const char* items_arr = sh_find_array(obj, "items");
        if (items_arr) {
            const char* it = sh_first_element(items_arr);
            for (; it != NULL && s->item_count < MAX_SHOP_ITEMS; it = sh_next_element(it)) {
                ShopItemEntry* e = &s->items[s->item_count];
                v = sh_find_value(it, "item_id");   if (v) e->item_id   = sh_parse_int(v);
                v = sh_find_value(it, "buy_price");  if (v) e->buy_price = sh_parse_int(v);
                // If buy_price not set, use item value * 2 as default
                if (e->buy_price == 0) {
                    const ItemDefinition* def = item_get(e->item_id);
                    if (def) e->buy_price = def->value * 2;
                    if (e->buy_price == 0) e->buy_price = 1;
                }
                s->item_count++;
            }
        }

        if (s->shop_id > 0) {
            LOG_INFO("[SHOP] Loaded shop %u '%s' (%u items)", s->shop_id, s->name, s->item_count);
            g_shop_count++;
        }
    }

    free(buf);
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
                          "— buying and reselling would mint gold",
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

/**
 * Send a shop's inventory and prices to a character.
 */
void shop_open(uint32_t character_id, int client_fd, uint32_t shop_id) {
    ShopDef* s = shop_find(shop_id);
    if (!s) {
        LOG_DEBUG("[SHOP] shop_open: unknown shop %u", shop_id);
        return;
    }

    ShopOpenPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_SHOP_OPEN;
    pkt.header.player_id = htonl(character_id);
    pkt.header.payload_size = htons(sizeof(pkt) - sizeof(PacketHeader));
    pkt.shop_id = htonl(s->shop_id);
    strncpy(pkt.shop_name, s->name, sizeof(pkt.shop_name) - 1);
    pkt.item_count = s->item_count;
    for (int i = 0; i < s->item_count; i++) {
        pkt.items[i].item_id   = htonl(s->items[i].item_id);
        pkt.items[i].buy_price = htonl(s->items[i].buy_price);
    }
    server_send(client_fd, &pkt, sizeof(pkt));
    LOG_DEBUG("[SHOP] Opened shop %u for player %u", shop_id, character_id);
}

/**
 * Validate a shop purchase packet and update inventory and gold.
 *
 * @param buffer  Packet buffer containing ShopBuyPacket.
 * @param bytes   Available packet bytes.
 */
void shop_handle_buy(uint32_t character_id, int client_fd, uint8_t* buffer, int bytes) {
    if (bytes < (int)sizeof(ShopBuyPacket)) return;
    ShopBuyPacket* req = (ShopBuyPacket*)buffer;
    uint32_t shop_id = ntohl(req->shop_id);
    uint32_t item_id = ntohl(req->item_id);

    ShopBuyResponsePacket resp;
    memset(&resp, 0, sizeof(resp));
    resp.header.type = PACKET_SHOP_BUY_RESPONSE;
    resp.header.player_id = htonl(character_id);
    resp.header.payload_size = htons(sizeof(resp) - sizeof(PacketHeader));
    resp.item_id = htonl(item_id);

    ShopDef* s = shop_find(shop_id);
    if (!s) {
        resp.success = 0;
        strncpy(resp.message, "Shop not found", sizeof(resp.message) - 1);
        server_send(client_fd, &resp, sizeof(resp));
        return;
    }

    // Find item in shop
    uint32_t price = 0;
    int found = 0;
    for (int i = 0; i < s->item_count; i++) {
        if (s->items[i].item_id == item_id) { price = s->items[i].buy_price; found = 1; break; }
    }
    if (!found) {
        resp.success = 0;
        strncpy(resp.message, "Item not sold here", sizeof(resp.message) - 1);
        server_send(client_fd, &resp, sizeof(resp));
        return;
    }

    // refuse unresolved item definitions
    if (!item_get(item_id)) {
        LOG_ERROR("[SHOP] shop %u lists item %u, which does not exist", shop_id, item_id);
        resp.success = 0;
        strncpy(resp.message, "Item unavailable", sizeof(resp.message) - 1);
        server_send(client_fd, &resp, sizeof(resp));
        return;
    }

    ActivePlayer* p = player_acquire(character_id);
    if (!p) return;

    if (p->gold < price) {
        player_release(p);
        resp.success = 0;
        strncpy(resp.message, "Not enough gold", sizeof(resp.message) - 1);
        server_send(client_fd, &resp, sizeof(resp));
        return;
    }

    // spend gold only after inventory placement succeeds
    const ItemDefinition* buy_def = item_get(item_id);
    int slot_before = inventory_first_free(p->inventory);
    uint16_t unplaced = inventory_add(p->inventory, item_id, 1,
                                      buy_def ? buy_def->max_stack : 1,
                                      buy_def ? buy_def->bind_on_pickup : 0);
    if (unplaced > 0) {
        player_release(p);
        resp.success = 0;
        strncpy(resp.message, "Inventory full", sizeof(resp.message) - 1);
        server_send(client_fd, &resp, sizeof(resp));
        return;
    }

    // report the existing slot used by a stack merge
    int slot = -1;
    for (int i = 0; i < INVENTORY_SLOTS; i++) {
        if (p->inventory[i].instance_id != 0 && p->inventory[i].item_id == item_id) {
            slot = i;
            break;
        }
    }
    if (slot < 0) slot = slot_before;

    p->gold -= price;
    p->is_dirty = 1;
    uint32_t new_gold = p->gold;
    player_release(p);

    resp.success        = 1;
    resp.new_gold       = htonl(new_gold);
    resp.inventory_slot = (uint8_t)slot;
    snprintf(resp.message, sizeof(resp.message), "Purchased for %u gold", price);
    server_send(client_fd, &resp, sizeof(resp));

    // send the resulting slot quantity after stack merging
    {
        uint16_t changed[1] = { (uint16_t)slot };
        player_send_slot_updates(client_fd, character_id, changed, 1);
    }

    LOG_DEBUG("[SHOP] Player %u bought item %u for %u gold (slot %d)", character_id, item_id, price, slot);
}

/**
 * Validate a shop sale packet and update inventory and gold.
 *
 * @param buffer  Packet buffer containing ShopSellPacket.
 * @param bytes   Available packet bytes.
 */
void shop_handle_sell(uint32_t character_id, int client_fd, uint8_t* buffer, int bytes) {
    if (bytes < (int)sizeof(ShopSellPacket)) return;
    ShopSellPacket* req = (ShopSellPacket*)buffer;
    uint32_t shop_id = ntohl(req->shop_id);
    uint8_t  slot    = req->inventory_slot;

    ShopSellResponsePacket resp;
    memset(&resp, 0, sizeof(resp));
    resp.header.type = PACKET_SHOP_SELL_RESPONSE;
    resp.header.player_id = htonl(character_id);
    resp.header.payload_size = htons(sizeof(resp) - sizeof(PacketHeader));
    resp.inventory_slot = slot;

    if (!shop_find(shop_id)) {
        resp.success = 0;
        strncpy(resp.message, "Shop not found", sizeof(resp.message) - 1);
        server_send(client_fd, &resp, sizeof(resp));
        return;
    }

    if (slot >= 150) {
        resp.success = 0;
        strncpy(resp.message, "Invalid slot", sizeof(resp.message) - 1);
        server_send(client_fd, &resp, sizeof(resp));
        return;
    }

    ActivePlayer* p = player_acquire(character_id);
    if (!p) return;

    uint32_t item_id = p->inventory[slot].item_id;
    if (p->inventory[slot].instance_id == 0) {
        player_release(p);
        resp.success = 0;
        strncpy(resp.message, "No item in slot", sizeof(resp.message) - 1);
        server_send(client_fd, &resp, sizeof(resp));
        return;
    }

    const ItemDefinition* def = item_get(item_id);
    uint32_t sell_price = def ? (def->value > 0 ? def->value : 1) : 1;

    // sell one unit from the selected stack
    inventory_remove_at(p->inventory, slot, 1);
    p->gold += sell_price;
    p->is_dirty = 1;
    uint32_t new_gold = p->gold;
    player_release(p);

    resp.success     = 1;
    resp.item_id     = htonl(item_id);
    resp.sell_price  = htonl(sell_price);
    resp.new_gold    = htonl(new_gold);
    snprintf(resp.message, sizeof(resp.message), "Sold for %u gold", sell_price);
    server_send(client_fd, &resp, sizeof(resp));

    {
        uint16_t changed[1] = { (uint16_t)slot };
        player_send_slot_updates(client_fd, character_id, changed, 1);
    }

    LOG_DEBUG("[SHOP] Player %u sold item %u for %u gold", character_id, item_id, sell_price);
}
