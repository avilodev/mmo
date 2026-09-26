/**
 * @file
 * The shop counter: opening a shop, buying from it, and selling into it.
 *
 * Split out of shop.c, which now does nothing but load and validate the
 * content. What is here is the part that spends a player's money, so it is
 * also the part that needs the gates: a shop the character actually has open
 * (shop_session.h), a merchant they are actually standing next to, and a
 * player who is actually alive (routes.c).
 */

#include "shop.h"
#include "shop_session.h"
#include "log.h"
#include "player_data.h"
#include "items_database.h"
#include "npc_world.h"
#include "utils.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>

extern NPCWorld g_npc_world;

/**
 * Confirm the character still has this shop open and is still standing at it.
 *
 * Both halves matter and neither used to exist. Without the session check a
 * client can trade with any shop in the world by naming its id -- the reason
 * cross-kingdom currency arbitrage was possible without travelling, since each
 * kingdom's shops price in that kingdom's coin. Without the range re-check the
 * session outlives the visit: open a shop, walk to the far side of the map,
 * keep trading.
 *
 * Range is measured against the merchant's *live* position, not the position
 * recorded when the shop opened, so a merchant that wanders takes its counter
 * with it. A merchant that has since died or despawned closes the shop.
 *
 * @param message_out  Receives the reason on refusal.
 * @return             1 when the trade may proceed, otherwise 0.
 */
static int shop_session_valid(uint32_t character_id, uint32_t shop_id,
                              char* message_out, size_t message_size) {
    ShopSession session;
    if (!shop_session_snapshot(character_id, &session) ||
        session.shop_id != shop_id) {
        snprintf(message_out, message_size, "Shop is not open");
        return 0;
    }

    float npc_x = session.npc_x, npc_y = session.npc_y;
    if (session.npc_id != 0) {
        NPCEntity* npc = npc_world_acquire(&g_npc_world, session.npc_id);
        if (!npc || !npc->is_alive) {
            if (npc) npc_world_release(&g_npc_world, npc);
            shop_session_close(character_id);
            snprintf(message_out, message_size, "The merchant is gone");
            return 0;
        }
        npc_x = npc->pos_x;
        npc_y = npc->pos_y;
        npc_world_release(&g_npc_world, npc);
    }

    float px = 0.0f, py = 0.0f;
    ActivePlayer* p = player_acquire(character_id);
    if (!p) {
        snprintf(message_out, message_size, "Player state changed");
        return 0;
    }
    px = p->pos_x;
    py = p->pos_y;
    player_release(p);

    float dx = px - npc_x, dy = py - npc_y;
    if (sqrtf(dx * dx + dy * dy) > SHOP_INTERACT_RANGE) {
        shop_session_close(character_id);
        snprintf(message_out, message_size, "Too far from the merchant");
        return 0;
    }

    shop_session_touch(character_id);
    return 1;
}

/**
 * Send a shop's inventory and prices to a character.
 */
void shop_open(uint32_t character_id, int client_fd, uint32_t shop_id,
               uint32_t npc_id, float npc_x, float npc_y) {
    ShopDef* s = shop_find(shop_id);
    if (!s) {
        LOG_DEBUG("[SHOP] shop_open: unknown shop %u", shop_id);
        return;
    }

    /* Opening the shop is what makes buying and selling from it legal. The
     * merchant that offered it is recorded with it, because that is what the
     * range check on every subsequent trade is measured against. */
    if (!shop_session_open(character_id, shop_id, npc_id, npc_x, npc_y)) return;

    ShopOpenPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_SHOP_OPEN;
    pkt.header.player_id = htonl(character_id);
    pkt.header.payload_size = htons(sizeof(pkt) - sizeof(PacketHeader));
    pkt.shop_id = htonl(s->shop_id);
    /* snprintf rather than strncpy: the two fields are the same width, so a
     * name that filled its own array left the copy unterminated. */
    snprintf(pkt.shop_name, sizeof(pkt.shop_name), "%s", s->name);
    pkt.item_count = s->item_count;
    for (int i = 0; i < s->item_count; i++) {
        pkt.items[i].item_id   = htonl(s->items[i].item_id);
        pkt.items[i].buy_price = htonl(s->items[i].buy_price);
    }
    server_send(client_fd, &pkt, sizeof(pkt));
    LOG_DEBUG("[SHOP] Opened shop %u for player %u", shop_id, character_id);
}

/**
 * Validate a shop purchase packet and update inventory and the shop's currency.
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

    if (!shop_session_valid(character_id, shop_id,
                            resp.message, sizeof(resp.message))) {
        resp.success = 0;
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

    const uint8_t currency = s->currency_id;
    const char*   coin_name = world_currency_name(currency);
    resp.currency_id = currency;

    ActivePlayer* p = player_acquire(character_id);
    if (!p) return;

    if (p->currency[currency] < price) {
        player_release(p);
        resp.success = 0;
        snprintf(resp.message, sizeof(resp.message), "Not enough %s", coin_name);
        server_send(client_fd, &resp, sizeof(resp));
        return;
    }

    /* Spend coin only after inventory placement succeeds.
     *
     * The slot the purchase landed in comes from the add itself, not from a
     * search for the item id afterwards. That search returned the *first* slot
     * holding the item, which is the right answer only while the item sits in
     * exactly one stack: buy a potion while holding two part-filled stacks and
     * the response named the wrong one, so the client redrew a slot that had
     * not changed and left the one that had. */
    const ItemDefinition* buy_def = item_get(item_id);

    uint16_t changed[MAX_SLOT_UPDATES];
    int      changed_count = 0;

    uint16_t unplaced = inventory_add_tracked(p->inventory, item_id, 1,
                                              buy_def ? buy_def->max_stack : 1,
                                              buy_def ? buy_def->bind_on_pickup : 0,
                                              changed, MAX_SLOT_UPDATES,
                                              &changed_count);
    if (unplaced > 0) {
        player_release(p);
        resp.success = 0;
        strncpy(resp.message, "Inventory full", sizeof(resp.message) - 1);
        server_send(client_fd, &resp, sizeof(resp));
        return;
    }

    int slot = changed_count > 0 ? (int)changed[0] : 0;

    /* The balance was checked under this same lock, so the debit cannot fail;
     * routing it through the guarded helper keeps that guarantee in one place. */
    if (!world_currency_debit(p->currency, currency, price)) {
        player_release(p);
        resp.success = 0;
        snprintf(resp.message, sizeof(resp.message), "Not enough %s", coin_name);
        server_send(client_fd, &resp, sizeof(resp));
        return;
    }
    /* Coin left the player and an item arrived. Losing half of that pair to a
     * crash is the worst shape this bug takes, so a purchase does not wait for
     * the sweep. */
    player_mark_critical(p);
    uint32_t new_balance = p->currency[currency];
    player_release(p);

    resp.success        = 1;
    resp.new_balance    = htonl(new_balance);
    resp.inventory_slot = (uint8_t)slot;
    snprintf(resp.message, sizeof(resp.message), "Purchased for %u %s",
             price, coin_name);
    server_send(client_fd, &resp, sizeof(resp));

    // send the resulting slot quantity after stack merging
    if (changed_count > 0)
        player_send_slot_updates(client_fd, character_id, changed, changed_count);

    LOG_DEBUG("[SHOP] Player %u bought item %u for %u %s (slot %d)",
              character_id, item_id, price, coin_name, slot);
}

/**
 * Validate a shop sale packet and pay the seller in the shop's currency.
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

    const ShopDef* s = shop_find(shop_id);
    if (!s) {
        resp.success = 0;
        strncpy(resp.message, "Shop not found", sizeof(resp.message) - 1);
        server_send(client_fd, &resp, sizeof(resp));
        return;
    }

    if (!shop_session_valid(character_id, shop_id,
                            resp.message, sizeof(resp.message))) {
        resp.success = 0;
        server_send(client_fd, &resp, sizeof(resp));
        return;
    }

    const uint8_t currency  = s->currency_id;
    const char*   coin_name = world_currency_name(currency);
    resp.currency_id = currency;

    if (slot >= INVENTORY_SLOTS) {
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
    uint32_t new_balance = world_currency_credit(p->currency, currency, sell_price);
    player_mark_critical(p);   /* the same trade, run the other way */
    player_release(p);

    resp.success     = 1;
    resp.item_id     = htonl(item_id);
    resp.sell_price  = htonl(sell_price);
    resp.new_balance = htonl(new_balance);
    snprintf(resp.message, sizeof(resp.message), "Sold for %u %s",
             sell_price, coin_name);
    server_send(client_fd, &resp, sizeof(resp));

    {
        uint16_t changed[1] = { (uint16_t)slot };
        player_send_slot_updates(client_fd, character_id, changed, 1);
    }

    LOG_DEBUG("[SHOP] Player %u sold item %u for %u %s",
              character_id, item_id, sell_price, coin_name);
}
