/**
 * @file
 * Handle inventory, equipment, loot, and shop packets.
 *
 * One of the four domain dispatchers split out of network.c's single
 * seventy-case switch. See net_internal.h for how they fit together.
 */

#include "net_internal.h"
#include "ability_bar.h"
#include "combat_system.h"
#include "combat_render.h"
#include "inventory.h"
#include "npc_types.h"
#include "audio/audio.h"
#include "ui/quest_log.h"
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <math.h>

/**
 * Dispatch one inventory packet.
 *
 * @param type  Opcode from the packet header.
 * @param data  Buffer holding one complete packet.
 * @param length  Available packet length in bytes.
 * @return 1 when this domain handled the opcode, otherwise 0.
 */
int net_dispatch_inventory(uint8_t type, const char* data, int length) {
    (void)data; (void)length;

    switch (type) {
        case PACKET_INVENTORY_UPDATE:
            if (length >= (int)(sizeof(PacketHeader) + 4)) {
                InventoryUpdatePacket* pkt = (InventoryUpdatePacket*)data;
                int count = pkt->count;
                if (count > MAX_SLOT_UPDATES) count = MAX_SLOT_UPDATES;

                // Refuse a packet that claims more entries than actually
                // arrived, rather than reading past the end of the buffer.
                int need = (int)(sizeof(PacketHeader) + 4 +
                                 (size_t)count * sizeof(SlotUpdateEntry));
                if (length < need) break;

                if (g_current_game && g_current_game->inventory) {
                    for (int i = 0; i < count; i++) {
                        uint16_t slot = ntohs(pkt->slots[i].slot);
                        const InventorySlotData* d = &pkt->slots[i].data;

                        uint64_t instance_id = mmo_ntohll(d->instance_id);
                        uint32_t item_id     = ntohl(d->item_id);
                        uint16_t quantity    = ntohs(d->quantity);

                        // equipment refreshes through CharacterInfo
                        if (slot >= INVENTORY_SIZE) continue;

                        ItemSlot* dst = &g_current_game->inventory->slots[slot];
                        if (instance_id == 0) {
                            dst->template_id = 0;
                            dst->quantity    = 0;
                            dst->instance_id = 0;
                            dst->is_bound    = 0;
                        } else {
                            dst->instance_id = instance_id;
                            dst->template_id = item_id;
                            dst->quantity    = quantity ? quantity : 1;
                            dst->is_bound    = d->is_bound;
                        }
                    }
                }
            }
            break;

        case PACKET_EQUIP_ITEM_RESPONSE:
            if (length >= (int)sizeof(EquipItemResponsePacket)) {
                EquipItemResponsePacket* pkt = (EquipItemResponsePacket*)data;
                pkt->message[127] = '\0';
                NET_LOG("[NET] Equip response: %s - %s\n",
                       pkt->success ? "OK" : "FAIL", pkt->message);
                if (pkt->success && g_current_game) {
                    network_request_player_stats();
                }
            }
            break;

        case PACKET_UNEQUIP_ITEM_RESPONSE:
            if (length >= (int)sizeof(UnequipItemResponsePacket)) {
                UnequipItemResponsePacket* pkt = (UnequipItemResponsePacket*)data;
                pkt->message[127] = '\0';
                NET_LOG("[NET] Unequip response: %s - %s\n",
                       pkt->success ? "OK" : "FAIL", pkt->message);
                if (pkt->success && g_current_game) {
                    network_request_player_stats();
                }
            }
            break;

        case PACKET_MOVE_ITEM_RESPONSE:
            if (length >= (int)sizeof(MoveItemResponsePacket)) {
                MoveItemResponsePacket* pkt = (MoveItemResponsePacket*)data;
                NET_LOG("[NET] Move item response: %s (from=%u to=%u)\n",
                       pkt->success ? "OK" : "FAIL", pkt->from_slot, pkt->to_slot);

                /* A refusal puts the bag back.
                 *
                 * The click already swapped the two slots locally, and this
                 * handler used to do nothing but log the answer -- so a
                 * refused move left the player looking at an arrangement the
                 * server did not have, and correctness rested entirely on some
                 * later INVENTORY_UPDATE arriving to overwrite it. On the
                 * refusal path in particular, one might not.
                 *
                 * The response names both slots, so the client has everything
                 * it needs to undo exactly what it did. */
                if (!pkt->success && g_current_game && g_current_game->inventory) {
                    if (inventory_revert_move(g_current_game->inventory,
                                              pkt->from_slot, pkt->to_slot)) {
                        NET_LOG("[NET] Reverted the refused move (%u <-> %u)\n",
                                pkt->from_slot, pkt->to_slot);
                    }
                }
            }
            break;

        case PACKET_USE_ITEM_RESPONSE:
            if (length >= (int)sizeof(UseItemResponsePacket)) {
                UseItemResponsePacket* pkt = (UseItemResponsePacket*)data;
                pkt->message[127] = '\0';
                NET_LOG("[NET] Use item response: %s - %s\n",
                       pkt->success ? "OK" : "FAIL", pkt->message);
                if (pkt->success && g_current_game) {
                    int32_t new_hp = (int32_t)ntohl(pkt->new_health);
                    int32_t new_mana = (int32_t)ntohl(pkt->new_mana);
                    if (new_hp > 0) {
                        g_current_game->player.info.health = (uint32_t)new_hp;
                    }
                    if (new_mana >= 0) {
                        ability_bar_on_resource_update(&g_current_game->playing->ability_bar,
                                                       new_mana,
                                                       g_current_game->playing->ability_bar.max_resource);
                    }
                    /* Decrement the slot the server acted on, not the first
                     * slot that happens to hold the same item.
                     *
                     * The old scan matched on template_id, so with the same
                     * potion in two stacks it took the unit from whichever came
                     * first. The authoritative INVENTORY_UPDATE that follows
                     * repairs the slot the server really used and says nothing
                     * about the one this had wrongly decremented, so the second
                     * stack stayed short until the player relogged. */
                    if (g_current_game->inventory) {
                        int used_slot = pkt->inventory_slot;
                        if (used_slot >= 0 && used_slot < INVENTORY_SIZE)
                            inventory_remove_item(g_current_game->inventory, used_slot, 1);
                        else
                            NET_WARN("[NET] Use item response named slot %d, "
                                     "outside the inventory\n", used_slot);
                    }
                }
            }
            break;

        case PACKET_DROP_ITEM_RESPONSE:
            if (length >= (int)sizeof(DropItemResponsePacket)) {
                DropItemResponsePacket* pkt = (DropItemResponsePacket*)data;
                (void)pkt;   // only read by the trace below
                NET_LOG("[NET] Drop item response: %s\n",
                       pkt->success ? "OK" : "FAIL");
            }
            break;

        case PACKET_LOOT_DROP:
            if (length >= (int)sizeof(LootDropPacket)) {
                LootDropPacket* pkt = (LootDropPacket*)data;
                NET_LOG("[NET] Loot drop: ground_id=%u item=%u qty=%u at (%.1f, %.1f)\n",
                       (uint32_t)ntohl(pkt->ground_item_id), (uint32_t)ntohl(pkt->item_id),
                       pkt->quantity, pkt->pos_x, pkt->pos_y);
                if (g_current_game && g_current_game->playing) {
                    for (int i = 0; i < MAX_GROUND_ITEMS; i++) {
                        if (!g_current_game->playing->ground_items[i].active) {
                            g_current_game->playing->ground_items[i].ground_item_id = ntohl(pkt->ground_item_id);
                            g_current_game->playing->ground_items[i].item_id = ntohl(pkt->item_id);
                            g_current_game->playing->ground_items[i].quantity = pkt->quantity;
                            g_current_game->playing->ground_items[i].pos_x = pkt->pos_x;
                            g_current_game->playing->ground_items[i].pos_y = pkt->pos_y;
                            g_current_game->playing->ground_items[i].active = 1;
                            break;
                        }
                    }
                }
            }
            break;

        case PACKET_LOOT_PICKUP_RESPONSE:
            if (length >= (int)sizeof(LootPickupResponsePacket)) {
                LootPickupResponsePacket* pkt = (LootPickupResponsePacket*)data;
                pkt->message[63] = '\0';
                NET_LOG("[NET] Loot pickup: %s - %s\n",
                       pkt->success ? "OK" : "FAIL", pkt->message);
                if (pkt->success && g_current_game) {
                    audio_event_pickup();
                    uint32_t gid = ntohl(pkt->ground_item_id);
                    for (int i = 0; i < MAX_GROUND_ITEMS; i++) {
                        if (g_current_game->playing->ground_items[i].active &&
                            g_current_game->playing->ground_items[i].ground_item_id == gid) {
                            g_current_game->playing->ground_items[i].active = 0;
                            break;
                        }
                    }
                    /* The bag is not written from here.
                     *
                     * This used to set the named slot to the item and to
                     * `quantity`, which is how many units the pickup added --
                     * not what the stack now holds. On a pickup that topped up
                     * a stack the player was already carrying, that wrote the
                     * wrong number into the right slot; before the server
                     * started naming the stack it merged into, it wrote a
                     * whole phantom item into an empty one. The authoritative
                     * INVENTORY_UPDATE for every slot the pickup touched
                     * arrives immediately behind this packet and is the only
                     * thing that should be believed. */
                }
            }
            break;

        case PACKET_LOOT_DESPAWN:
            if (length >= (int)sizeof(LootDespawnPacket)) {
                LootDespawnPacket* pkt = (LootDespawnPacket*)data;
                uint32_t despawn_id = ntohl(pkt->ground_item_id);
                NET_LOG("[NET] Loot despawned: %u\n", despawn_id);
                if (g_current_game && g_current_game->playing) {
                    for (int i = 0; i < MAX_GROUND_ITEMS; i++) {
                        if (g_current_game->playing->ground_items[i].active &&
                            g_current_game->playing->ground_items[i].ground_item_id == despawn_id) {
                            g_current_game->playing->ground_items[i].active = 0;
                            break;
                        }
                    }
                }
            }
            break;

        case PACKET_SHOP_OPEN:
            if (length >= (int)sizeof(ShopOpenPacket) && g_current_game && g_current_game->playing) {
                ShopOpenPacket* pkt = (ShopOpenPacket*)data;
                ShopState* shop = &g_current_game->playing->shop;
                shop->shop_id = ntohl(pkt->shop_id);
                uint8_t count = pkt->item_count;
                if (count > MAX_SHOP_ITEMS) count = MAX_SHOP_ITEMS;
                shop->item_count = count;
                memcpy(shop->shop_name, pkt->shop_name, 31);
                shop->shop_name[31] = '\0';
                for (int i = 0; i < count; i++) {
                    shop->items[i].item_id   = ntohl(pkt->items[i].item_id);
                    shop->items[i].buy_price = ntohl(pkt->items[i].buy_price);
                }
                shop->sell_tab  = 0;
                shop->is_open   = 1;
                NET_LOG("[NET] Shop open: id=%u '%s' (%u items)\n",
                       shop->shop_id, shop->shop_name, count);
            }
            break;

        case PACKET_SHOP_BUY_RESPONSE:
            if (length >= (int)sizeof(ShopBuyResponsePacket) && g_current_game) {
                ShopBuyResponsePacket* pkt = (ShopBuyResponsePacket*)data;
                pkt->message[63] = '\0';
                NET_LOG("[NET] Shop buy: %s — %s\n",
                       pkt->success ? "OK" : "FAIL", pkt->message);
                if (pkt->success) {
                    // Update the charged balance and inventory from server-authoritative values
                    if (world_currency_valid(pkt->currency_id))
                        g_current_game->player.info.currency[pkt->currency_id] =
                            ntohl(pkt->new_balance);
                    uint8_t slot = pkt->inventory_slot;
                    if (slot < INVENTORY_SIZE && g_current_game->inventory) {
                        uint32_t item_id = ntohl(pkt->item_id);
                        g_current_game->inventory->slots[slot].template_id = item_id;
                        g_current_game->inventory->slots[slot].quantity     = 1;
                    }
                }
            }
            break;

        case PACKET_SHOP_SELL_RESPONSE:
            if (length >= (int)sizeof(ShopSellResponsePacket) && g_current_game) {
                ShopSellResponsePacket* pkt = (ShopSellResponsePacket*)data;
                pkt->message[63] = '\0';
                NET_LOG("[NET] Shop sell: %s — %s\n",
                       pkt->success ? "OK" : "FAIL", pkt->message);
                if (pkt->success) {
                    if (world_currency_valid(pkt->currency_id))
                        g_current_game->player.info.currency[pkt->currency_id] =
                            ntohl(pkt->new_balance);
                    uint8_t slot = pkt->inventory_slot;
                    if (slot < INVENTORY_SIZE && g_current_game->inventory) {
                        g_current_game->inventory->slots[slot].template_id = 0;
                        g_current_game->inventory->slots[slot].quantity     = 0;
                    }
                }
            }
            break;

        default:
            return 0;   // not ours; the next domain gets a look
    }

    return 1;
}

/* --- Inventory, loot, and shop requests ---------------------------------- */

/**
 * Send a request to equip an inventory item into an equipment slot.
 */
void network_send_equip_item(uint32_t item_id, uint8_t inventory_slot, uint8_t equip_slot) {
    if (!g_net.connected) return;

    EquipItemPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_EQUIP_ITEM;
    pkt.header.player_id = htonl(g_net.character_id);
    pkt.header.payload_size = htons(sizeof(EquipItemPacket) - sizeof(PacketHeader));
    pkt.item_id = htonl(item_id);
    pkt.inventory_slot = inventory_slot;
    pkt.equip_slot = equip_slot;

    net_send((char*)&pkt, sizeof(pkt));
    NET_LOG("[NET] Equip item sent: item=%u inv_slot=%u equip_slot=%u\n",
           item_id, inventory_slot, equip_slot);
}

/**
 * Send a request to unequip an equipment slot.
 */
void network_send_unequip_item(uint8_t equip_slot) {
    if (!g_net.connected) return;

    UnequipItemPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_UNEQUIP_ITEM;
    pkt.header.player_id = htonl(g_net.character_id);
    pkt.header.payload_size = htons(sizeof(UnequipItemPacket) - sizeof(PacketHeader));
    pkt.equip_slot = equip_slot;

    net_send((char*)&pkt, sizeof(pkt));
    NET_LOG("[NET] Unequip item sent: equip_slot=%u\n", equip_slot);
}

/**
 * Send a request to use an inventory slot.
 */
void network_send_use_item(uint8_t inventory_slot) {
    if (!g_net.connected) return;

    UseItemPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_USE_ITEM;
    pkt.header.player_id = htonl(g_net.character_id);
    pkt.header.payload_size = htons(sizeof(UseItemPacket) - sizeof(PacketHeader));
    pkt.inventory_slot = inventory_slot;

    net_send((char*)&pkt, sizeof(pkt));
    NET_LOG("[NET] Use item sent: slot=%u\n", inventory_slot);
}

/**
 * Send a request to drop an inventory slot.
 */
void network_send_drop_item(uint8_t inventory_slot) {
    if (!g_net.connected) return;

    DropItemPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_DROP_ITEM;
    pkt.header.player_id = htonl(g_net.character_id);
    pkt.header.payload_size = htons(sizeof(DropItemPacket) - sizeof(PacketHeader));
    pkt.inventory_slot = inventory_slot;

    net_send((char*)&pkt, sizeof(pkt));
    NET_LOG("[NET] Drop item sent: slot=%u\n", inventory_slot);
}

/**
 * Send a request to move or swap two inventory slots.
 */
void network_send_move_item(uint8_t from_slot, uint8_t to_slot) {
    if (!g_net.connected) return;

    MoveItemPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_MOVE_ITEM;
    pkt.header.player_id = htonl(g_net.character_id);
    pkt.header.payload_size = htons(sizeof(MoveItemPacket) - sizeof(PacketHeader));
    pkt.from_slot = from_slot;
    pkt.to_slot = to_slot;

    net_send((char*)&pkt, sizeof(pkt));
}

/**
 * Send a pickup request for a ground-item instance.
 */
void network_send_loot_pickup(uint32_t ground_item_id) {
    if (!g_net.connected) return;

    LootPickupRequestPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_LOOT_PICKUP_REQUEST;
    pkt.header.player_id = htonl(g_net.character_id);
    pkt.header.payload_size = htons(sizeof(LootPickupRequestPacket) - sizeof(PacketHeader));
    pkt.ground_item_id = htonl(ground_item_id);

    net_send((char*)&pkt, sizeof(pkt));
    NET_LOG("[NET] Loot pickup request sent: ground_item=%u\n", ground_item_id);
}

/**
 * Send a request to buy an item from a shop.
 */
void network_send_shop_buy(uint32_t shop_id, uint32_t item_id) {
    if (!g_net.connected) return;

    ShopBuyPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_SHOP_BUY;
    pkt.header.player_id = htonl(g_net.character_id);
    pkt.header.payload_size = htons(sizeof(ShopBuyPacket) - sizeof(PacketHeader));
    pkt.shop_id = htonl(shop_id);
    pkt.item_id = htonl(item_id);

    net_send((char*)&pkt, sizeof(pkt));
    NET_LOG("[NET] Shop buy: shop=%u item=%u\n", shop_id, item_id);
}

/**
 * Send a request to sell an inventory slot to a shop.
 */
void network_send_shop_sell(uint32_t shop_id, uint8_t inventory_slot) {
    if (!g_net.connected) return;

    ShopSellPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.type = PACKET_SHOP_SELL;
    pkt.header.player_id = htonl(g_net.character_id);
    pkt.header.payload_size = htons(sizeof(ShopSellPacket) - sizeof(PacketHeader));
    pkt.shop_id = htonl(shop_id);
    pkt.inventory_slot = inventory_slot;

    net_send((char*)&pkt, sizeof(pkt));
    NET_LOG("[NET] Shop sell: shop=%u slot=%u\n", shop_id, inventory_slot);
}
