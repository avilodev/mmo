// ============================================================================
// shop.c — NPC vendor / shop system
// ============================================================================

#include "shop.h"
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

// ---------------------------------------------------------------------------
// Minimal JSON helpers
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

static ShopDef* shop_find(uint32_t shop_id) {
    for (int i = 0; i < g_shop_count; i++)
        if (g_shops[i].shop_id == shop_id) return &g_shops[i];
    return NULL;
}

int shop_init(const char* json_path) {
    g_shop_count = 0;

    FILE* f = fopen(json_path, "r");
    if (!f) {
        fprintf(stderr, "[SHOP] Cannot open %s: %s\n", json_path, strerror(errno));
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

    const char* obj = shops_arr;
    while ((obj = sh_next_element(obj)) != NULL && g_shop_count < MAX_SHOPS) {
        ShopDef* s = &g_shops[g_shop_count];
        memset(s, 0, sizeof(*s));

        const char* v;
        v = sh_find_value(obj, "shop_id"); if (v) s->shop_id = sh_parse_int(v);
        v = sh_find_value(obj, "name");    if (v) sh_parse_string(v, s->name, sizeof(s->name));

        const char* items_arr = sh_find_array(obj, "items");
        if (items_arr) {
            const char* it = items_arr;
            while ((it = sh_next_element(it)) != NULL && s->item_count < MAX_SHOP_ITEMS) {
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
            printf("[SHOP] Loaded shop %u '%s' (%u items)\n",
                   s->shop_id, s->name, s->item_count);
            g_shop_count++;
        }
    }

    free(buf);
    printf("[SHOP] %d shops loaded\n", g_shop_count);
    return 1;
}

void shop_cleanup(void) { g_shop_count = 0; }

// ---------------------------------------------------------------------------
// Open shop — send PACKET_SHOP_OPEN
// ---------------------------------------------------------------------------

void shop_open(uint32_t character_id, int client_fd, uint32_t shop_id) {
    ShopDef* s = shop_find(shop_id);
    if (!s) {
        printf("[SHOP] shop_open: unknown shop %u\n", shop_id);
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
    printf("[SHOP] Opened shop %u for player %u\n", shop_id, character_id);
}

// ---------------------------------------------------------------------------
// Buy
// ---------------------------------------------------------------------------

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

    ActivePlayer* p = player_acquire(character_id);
    if (!p) return;

    if (p->gold < price) {
        player_release(p);
        resp.success = 0;
        strncpy(resp.message, "Not enough gold", sizeof(resp.message) - 1);
        server_send(client_fd, &resp, sizeof(resp));
        return;
    }

    // Find empty inventory slot
    int slot = -1;
    for (int i = 0; i < 150; i++) {
        if (p->inventory[i] == 0) { slot = i; break; }
    }
    if (slot < 0) {
        player_release(p);
        resp.success = 0;
        strncpy(resp.message, "Inventory full", sizeof(resp.message) - 1);
        server_send(client_fd, &resp, sizeof(resp));
        return;
    }

    p->gold -= price;
    p->inventory[slot] = item_id;
    p->is_dirty = 1;
    uint32_t new_gold = p->gold;
    player_release(p);

    resp.success        = 1;
    resp.new_gold       = htonl(new_gold);
    resp.inventory_slot = (uint8_t)slot;
    snprintf(resp.message, sizeof(resp.message), "Purchased for %u gold", price);
    server_send(client_fd, &resp, sizeof(resp));
    printf("[SHOP] Player %u bought item %u for %u gold (slot %d)\n",
           character_id, item_id, price, slot);
}

// ---------------------------------------------------------------------------
// Sell
// ---------------------------------------------------------------------------

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

    uint32_t item_id = p->inventory[slot];
    if (item_id == 0) {
        player_release(p);
        resp.success = 0;
        strncpy(resp.message, "No item in slot", sizeof(resp.message) - 1);
        server_send(client_fd, &resp, sizeof(resp));
        return;
    }

    const ItemDefinition* def = item_get(item_id);
    uint32_t sell_price = def ? (def->value > 0 ? def->value : 1) : 1;

    p->inventory[slot] = 0;
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
    printf("[SHOP] Player %u sold item %u for %u gold\n", character_id, item_id, sell_price);
}
