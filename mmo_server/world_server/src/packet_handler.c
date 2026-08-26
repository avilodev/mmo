/**
 * @file
 * Handle authenticated world packets for movement, inventory, chat, parties, and sessions.
 */

#include "packet_handler.h"
#include "str_fixed.h"
#include "log.h"
#include "player_data.h"
#include "zone_system.h"
#include "config.h"
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

static int equip_index_for(uint8_t equip_slot) {
    switch (equip_slot) {
        case SLOT_HELMET:    return EQUIP_HELMET;
        case SLOT_GLOVES:    return EQUIP_GLOVES;
        case SLOT_CHEST:     return EQUIP_CHEST;
        case SLOT_LEGGINGS:  return EQUIP_LEGGINGS;
        case SLOT_BOOTS:     return EQUIP_BOOTS;
        case SLOT_MAIN_HAND: return EQUIP_MAIN_HAND;
        case SLOT_OFF_HAND:  return EQUIP_SECOND_HAND;
        default:             return -1;
    }
}

/**
 * Record client latency and echo the ping packet back.
 *
 * The echo is the framed packet, byte for byte -- not a fixed
 * header-plus-two-bytes. The two differ whenever a client declares a
 * payload_size larger than the latency field, and the old fixed reply left the
 * surplus bytes unsent while the header the client reads still announced them:
 * the client's next read then took the following packet's header as this
 * packet's tail, and every packet after it on that connection was misframed.
 * The realm's ping route (realm_server/src/routes.c) already echoed what it
 * was given; this is the same rule on the world side.
 *
 * @param bytes  Length of the framed packet, as computed by the read loop from
 *               the declared payload_size. Never larger than what was received.
 */
void handle_ping(int client_fd, uint8_t* buffer, ssize_t bytes, uint32_t character_id, int player_slot) {
    PacketHeader* hdr = (PacketHeader*)buffer;
    if (bytes >= (ssize_t)(sizeof(PacketHeader) + sizeof(uint16_t)) &&
        ntohs(hdr->payload_size) >= sizeof(uint16_t)) {
        /* memcpy, not *(uint16_t*)(buffer + sizeof(PacketHeader)).
         *
         * PacketHeader is 7 bytes, so the latency field sits at an odd offset.
         * Casting the buffer to a uint16_t* asserts a 2-byte alignment that is
         * never there: undefined behaviour on any target, and on the AArch64
         * targets protocol.h names, a trap or a kernel fixup on the hot path.
         * memcpy of two bytes compiles to the same unaligned load x86 was
         * already doing, without the claim. The realm router carried the same
         * defect in its character-list case; see realm_server/src/routes.c. */
        uint16_t raw_ping = 0;
        memcpy(&raw_ping, buffer + sizeof(PacketHeader), sizeof(raw_ping));
        uint16_t client_ping = ntohs(raw_ping);
        ActivePlayer* player = player_acquire_hint(character_id, player_slot);
        if (player) {
            player->ping_ms = client_ping;
            player_release(player);
        }
    }
    if (bytes < (ssize_t)sizeof(PacketHeader)) return;
    server_send(client_fd, buffer, (size_t)bytes);
}

/** Send the requested character data to a client. */
void handle_request_player_data(int client_fd, uint32_t character_id) {
    player_send_data_response(client_fd, character_id);
}

static void send_move_correction(int client_fd, float pos_x, float pos_y) {
    PlayerMoveAckPacket correction;
    memset(&correction, 0, sizeof(correction));
    correction.header.type = PACKET_PLAYER_MOVE_ACK;
    correction.header.payload_size = htons(sizeof(PlayerMoveAckPacket) - sizeof(PacketHeader));
    correction.pos_x = pos_x;
    correction.pos_y = pos_y;
    server_send(client_fd, &correction, sizeof(correction));
}

/** Validate and apply a client movement proposal. */
void handle_player_move(int client_fd, uint32_t character_id, int player_slot, PlayerMovePacket* pkt) {
    ActivePlayer* player = player_acquire_hint(character_id, player_slot);
    if (!player) return;
    if (!player->is_loaded) { player_release(player); return; }

    float client_x = pkt->pos_x;
    float client_y = pkt->pos_y;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    // use server-owned movement speed
    MoveVerdict verdict = move_validate(&player->move_budget,
                                        player->pos_x, player->pos_y,
                                        client_x, client_y,
                                        player->move_speed, &now);

    if (verdict != MOVE_ACCEPT) {
        LOG_WARN_RL(5, 60,
                    "[MOVE] rejected (%s) character %u: (%.1f, %.1f) -> (%.1f, %.1f)",
                    move_verdict_name(verdict), character_id,
                    player->pos_x, player->pos_y, client_x, client_y);

        float server_x = player->pos_x;
        float server_y = player->pos_y;
        player_release(player);
        send_move_correction(client_fd, server_x, server_y);
        return;
    }

    player->pos_x = client_x;
    player->pos_y = client_y;
    player->is_dirty = 1;

    // notify only on zone transitions
    const WorldZone* zone = zone_lookup(client_x, client_y);
    uint8_t new_zone_id = zone ? zone->id : 0;
    if (new_zone_id != player->current_zone_id) {
        player->current_zone_id = new_zone_id;
        player_release(player);

        if (zone) {
            ZoneChangePacket zpkt = {0};
            zpkt.header.type         = PACKET_ZONE_CHANGE;
            zpkt.header.player_id    = htonl(character_id);
            zpkt.header.payload_size = htons(sizeof(ZoneChangePacket) - sizeof(PacketHeader));
            zpkt.zone_id   = zone->id;
            zpkt.zone_type = zone->type;
            STR_COPY_FIELD(zpkt.zone_name, zone->name);
            server_send(client_fd, &zpkt, sizeof(zpkt));
        }
    } else {
        player_release(player);
    }
}

/** Validate and apply an inventory-to-equipment transfer. */
void handle_equip_item(int client_fd, uint32_t character_id, uint8_t* buffer, ssize_t bytes) {
    if (bytes < (ssize_t)sizeof(EquipItemPacket)) {
        LOG_WARN_RL(5, 60, "Invalid equip packet size");
        return;
    }

    EquipItemPacket* equip = (EquipItemPacket*)buffer;
    uint32_t item_id = ntohl(equip->item_id);
    uint8_t inventory_slot = equip->inventory_slot;
    uint8_t equip_slot = equip->equip_slot;

    ActivePlayer* player = player_acquire(character_id);
    if (!player) {
        LOG_WARN_RL(5, 60, "Player not found for equip");
        return;
    }
    if (!player->is_loaded) { player_release(player); return; }

    // Verify item is in inventory
    if (inventory_slot >= INVENTORY_SLOTS ||
        player->inventory[inventory_slot].instance_id == 0 ||
        player->inventory[inventory_slot].item_id != item_id) {
        LOG_WARN_RL(5, 60, "Item %u not in inventory slot %u", item_id, inventory_slot);
        player_release(player);

        // Send error response
        EquipItemResponsePacket response = {0};
        response.header.type = PACKET_EQUIP_ITEM_RESPONSE;
        response.header.player_id = htonl(character_id);
        response.header.payload_size = htons(sizeof(EquipItemResponsePacket) - sizeof(PacketHeader));
        response.success = 0;
        strncpy(response.message, "Item not in inventory", sizeof(response.message) - 1);
        server_send(client_fd, &response, sizeof(response));
        return;
    }

    // Get item definition
    const ItemDefinition* item = item_get(item_id);
    if (!item) {
        LOG_WARN_RL(5, 60, "Item %u does not exist", item_id);
        player_release(player);

        EquipItemResponsePacket response = {0};
        response.header.type = PACKET_EQUIP_ITEM_RESPONSE;
        response.header.player_id = htonl(character_id);
        response.header.payload_size = htons(sizeof(EquipItemResponsePacket) - sizeof(PacketHeader));
        response.success = 0;
        strncpy(response.message, "Invalid item", sizeof(response.message) - 1);
        server_send(client_fd, &response, sizeof(response));
        return;
    }

    // Validate slot
    if (item->slot != equip_slot && !(item->is_two_handed && equip_slot == SLOT_MAIN_HAND)) {
        LOG_WARN_RL(5, 60, "Item %s cannot be equipped in slot %u", item->name, equip_slot);
        player_release(player);

        EquipItemResponsePacket response = {0};
        response.header.type = PACKET_EQUIP_ITEM_RESPONSE;
        response.header.player_id = htonl(character_id);
        response.success = 0;
        strncpy(response.message, "Invalid slot for this item", sizeof(response.message) - 1);
        server_send(client_fd, &response, sizeof(response));
        return;
    }

    // Check if player meets requirements
    if (!item_can_equip(item_id, player->level, player->race_id, player->race_id)) {
        LOG_WARN_RL(5, 60, "Player cannot equip item: %s", item->name);
        player_release(player);

        EquipItemResponsePacket response = {0};
        response.header.type = PACKET_EQUIP_ITEM_RESPONSE;
        response.header.player_id = htonl(character_id);
        response.success = 0;
        strncpy(response.message, "Requirements not met", sizeof(response.message) - 1);
        server_send(client_fd, &response, sizeof(response));
        return;
    }

    int equip_index = equip_index_for(equip_slot);
    if (equip_index < 0) {
        player_release(player);
        return;
    }

    // enforce two-handed and off-hand exclusivity
    if (equip_slot == SLOT_MAIN_HAND && item->is_two_handed &&
        player->equipment[EQUIP_SECOND_HAND].instance_id != 0) {

        int offhand_slot = inventory_first_free(player->inventory);
        if (offhand_slot < 0) {
            player_release(player);
            EquipItemResponsePacket response = {0};
            response.header.type = PACKET_EQUIP_ITEM_RESPONSE;
            response.header.player_id = htonl(character_id);
            response.success = 0;
            strncpy(response.message, "Inventory full (off-hand)", sizeof(response.message) - 1);
            server_send(client_fd, &response, sizeof(response));
            return;
        }
        player->inventory[offhand_slot] = player->equipment[EQUIP_SECOND_HAND];
        memset(&player->equipment[EQUIP_SECOND_HAND], 0, sizeof(ItemInstance));
    }

    if (equip_slot == SLOT_OFF_HAND &&
        player->equipment[EQUIP_MAIN_HAND].instance_id != 0) {
        const ItemDefinition* main_hand =
            item_get(player->equipment[EQUIP_MAIN_HAND].item_id);
        if (main_hand && main_hand->is_two_handed) {
            player_release(player);
            EquipItemResponsePacket response = {0};
            response.header.type = PACKET_EQUIP_ITEM_RESPONSE;
            response.header.player_id = htonl(character_id);
            response.success = 0;
            strncpy(response.message, "Cannot equip with two-handed weapon", sizeof(response.message) - 1);
            server_send(client_fd, &response, sizeof(response));
            return;
        }
    }

    // preserve instance identity across the swap
    ItemInstance incoming = player->inventory[inventory_slot];
    player->inventory[inventory_slot] = player->equipment[equip_index];
    player->equipment[equip_index]    = incoming;

    if (item->bind_on_equip) player->equipment[equip_index].is_bound = 1;

    uint32_t returned_item = player->inventory[inventory_slot].item_id;

    player->is_dirty = 1;

    // Recalculate stats with new equipment
    player_apply_equipment_bonuses(player);

    player_release(player);

    // Send success response
    EquipItemResponsePacket response = {0};
    response.header.type = PACKET_EQUIP_ITEM_RESPONSE;
    response.header.player_id = htonl(character_id);
    response.success = 1;
    response.equipped_item = htonl(item_id);
    response.returned_item = htonl(returned_item);
    snprintf(response.message, sizeof(response.message), "Equipped: %s", item->name);

    server_send(client_fd, &response, sizeof(response));

    // synchronize both changed slots
    {
        uint16_t changed[2] = { inventory_slot,
                                (uint16_t)(EQUIP_SLOT_BASE + equip_index) };
        player_send_slot_updates(client_fd, character_id, changed, 2);
    }

    /* Re-resolve the character rather than reusing the released pointer.
     *
     * player_release() gave the slot up. It stays a valid address -- the
     * registry is a static array -- but it stops being *this* character's:
     * between the release above and here, this character can disconnect and
     * the slot be handed to someone else, and player_send_stats() would then
     * lock that stranger's slot and send their statistics down this
     * descriptor. Narrow, but it is a cross-character read, and the ownership
     * rule in player_data.h says a pointer is only good while it is held.
     *
     * The re-acquire is after the response and the slot updates on purpose, so
     * the client still sees them in the order it did before. */
    {
        ActivePlayer* still_here = player_acquire(character_id);
        if (still_here) {
            player_send_stats_locked(client_fd, still_here);
            player_release(still_here);
        }
    }

    LOG_DEBUG("Character %u equipped %s", character_id, item->name);
}

/** Validate and move equipped gear into the inventory. */
void handle_unequip_item(int client_fd, uint32_t character_id, uint8_t* buffer, ssize_t bytes) {
    if (bytes < (ssize_t)sizeof(UnequipItemPacket)) {
        LOG_WARN_RL(5, 60, "Invalid unequip packet size");
        return;
    }

    UnequipItemPacket* unequip = (UnequipItemPacket*)buffer;
    uint8_t equip_slot = unequip->equip_slot;

    ActivePlayer* player = player_acquire(character_id);
    if (!player) return;
    if (!player->is_loaded) { player_release(player); return; }

    int inventory_slot = inventory_first_free(player->inventory);

    if (inventory_slot == -1) {
        player_release(player);

        // Send error - inventory full
        UnequipItemResponsePacket response = {0};
        response.header.type = PACKET_UNEQUIP_ITEM_RESPONSE;
        response.header.player_id = htonl(character_id);
        response.success = 0;
        strncpy(response.message, "Inventory full", sizeof(response.message) - 1);
        server_send(client_fd, &response, sizeof(response));
        return;
    }

    int equip_index = equip_index_for(equip_slot);
    if (equip_index < 0) {
        player_release(player);
        return;
    }

    if (player->equipment[equip_index].instance_id == 0) {
        player_release(player);
        return;   // nothing was equipped
    }

    // preserve instance identity and binding
    uint32_t unequipped_item = player->equipment[equip_index].item_id;
    player->inventory[inventory_slot] = player->equipment[equip_index];
    memset(&player->equipment[equip_index], 0, sizeof(ItemInstance));
    player->is_dirty = 1;

    // Recalculate stats without this equipment
    player_apply_equipment_bonuses(player);

    player_release(player);

    // Send response
    UnequipItemResponsePacket response = {0};
    response.header.type = PACKET_UNEQUIP_ITEM_RESPONSE;
    response.header.player_id = htonl(character_id);
    response.success = 1;
    response.unequipped_item = htonl(unequipped_item);
    response.inventory_slot = inventory_slot;
    strncpy(response.message, "Item unequipped", sizeof(response.message) - 1);

    server_send(client_fd, &response, sizeof(response));

    {
        uint16_t changed[2] = { (uint16_t)inventory_slot,
                                (uint16_t)(EQUIP_SLOT_BASE + equip_index) };
        player_send_slot_updates(client_fd, character_id, changed, 2);
    }

    /* Re-resolve the character rather than reusing the released pointer.
     *
     * player_release() gave the slot up. It stays a valid address -- the
     * registry is a static array -- but it stops being *this* character's:
     * between the release above and here, this character can disconnect and
     * the slot be handed to someone else, and player_send_stats() would then
     * lock that stranger's slot and send their statistics down this
     * descriptor. Narrow, but it is a cross-character read, and the ownership
     * rule in player_data.h says a pointer is only good while it is held.
     *
     * The re-acquire is after the response and the slot updates on purpose, so
     * the client still sees them in the order it did before. */
    {
        ActivePlayer* still_here = player_acquire(character_id);
        if (still_here) {
            player_send_stats_locked(client_fd, still_here);
            player_release(still_here);
        }
    }

    LOG_DEBUG("Character %u unequipped item from slot %u", character_id, equip_slot);
}

/** Validate, consume, and apply one inventory item. */
void handle_use_item(int client_fd, uint32_t character_id, uint8_t* buffer, ssize_t bytes) {
    if (bytes < (ssize_t)sizeof(UseItemPacket)) {
        LOG_WARN_RL(5, 60, "Invalid use item packet size");
        return;
    }

    UseItemPacket* use = (UseItemPacket*)buffer;
    uint8_t inventory_slot = use->inventory_slot;

    ActivePlayer* player = player_acquire(character_id);
    if (!player) return;
    if (!player->is_loaded) { player_release(player); return; }

    UseItemResponsePacket response = {0};
    response.header.type = PACKET_USE_ITEM_RESPONSE;
    response.header.player_id = htonl(character_id);
    response.header.payload_size = htons(sizeof(UseItemResponsePacket) - sizeof(PacketHeader));

    // Validate slot
    if (inventory_slot >= INVENTORY_SLOTS ||
        player->inventory[inventory_slot].instance_id == 0) {
        player_release(player);
        response.success = 0;
        strncpy(response.message, "No item in that slot", sizeof(response.message) - 1);
        server_send(client_fd, &response, sizeof(response));
        return;
    }

    uint32_t item_id = player->inventory[inventory_slot].item_id;
    const ItemDefinition* item = item_get(item_id);
    if (!item) {
        player_release(player);
        response.success = 0;
        strncpy(response.message, "Invalid item", sizeof(response.message) - 1);
        server_send(client_fd, &response, sizeof(response));
        return;
    }

    // Must be a consumable
    if (item->type != ITEM_TYPE_CONSUMABLE || item->use_effect == USE_EFFECT_NONE) {
        player_release(player);
        response.success = 0;
        strncpy(response.message, "Item is not consumable", sizeof(response.message) - 1);
        server_send(client_fd, &response, sizeof(response));
        return;
    }

    // Check cooldown
    struct timespec now_ts;
    clock_gettime(CLOCK_MONOTONIC, &now_ts);
    double now = now_ts.tv_sec + now_ts.tv_nsec / 1e9;

    if (item->use_cooldown > 0.0f && player->last_consumable_time > 0.0) {
        double elapsed = now - player->last_consumable_time;
        if (elapsed < (double)item->use_cooldown) {
            player_release(player);
            response.success = 0;
            snprintf(response.message, sizeof(response.message),
                     "On cooldown (%.1fs)", item->use_cooldown - elapsed);
            server_send(client_fd, &response, sizeof(response));
            return;
        }
    }

    // Apply the effect
    int32_t hp_changed = 0;
    int32_t mp_changed = 0;

    switch (item->use_effect) {
        case USE_EFFECT_RESTORE_HEALTH: {
            int32_t missing = player->max_health - player->health;
            hp_changed = (item->use_value > missing) ? missing : item->use_value;
            player->health += hp_changed;
            break;
        }
        case USE_EFFECT_RESTORE_MANA: {
            /* Restores whichever pool the character's role selects. Human Form has no
             * pool, so max_resource is zero there and the item restores nothing. */
            int32_t missing = player->max_resource - player->resource;
            mp_changed = (item->use_value > missing) ? missing : item->use_value;
            if (mp_changed < 0) mp_changed = 0;
            player->resource += mp_changed;
            break;
        }
        case USE_EFFECT_RESTORE_BOTH: {
            int32_t hp_missing = player->max_health - player->health;
            hp_changed = (item->use_value > hp_missing) ? hp_missing : item->use_value;
            player->health += hp_changed;

            int32_t mp_missing = player->max_resource - player->resource;
            mp_changed = (item->use_value > mp_missing) ? mp_missing : item->use_value;
            if (mp_changed < 0) mp_changed = 0;
            player->resource += mp_changed;
            break;
        }
        default:
            break;
    }

    // consume one unit from the stack
    inventory_remove_at(player->inventory, inventory_slot, 1);
    player->is_dirty = 1;
    player->last_consumable_time = now;

    // Build response
    response.success = 1;
    response.effect_type = item->use_effect;
    response.health_changed = htonl((uint32_t)hp_changed);
    response.mana_changed = htonl((uint32_t)mp_changed);
    response.new_health = htonl((uint32_t)player->health);
    response.new_mana = htonl((uint32_t)player->resource);
    response.item_id = htonl(item_id);
    /* The slot the request named. The client used to have to guess it from
     * item_id, and guessed wrong whenever the same item sat in two stacks. */
    response.inventory_slot = (uint8_t)inventory_slot;

    player_release(player);

    snprintf(response.message, sizeof(response.message), "Used %s", item->name);
    server_send(client_fd, &response, sizeof(response));

    // synchronize the remaining stack
    {
        uint16_t changed[1] = { inventory_slot };
        player_send_slot_updates(client_fd, character_id, changed, 1);
    }

    LOG_DEBUG("Character %u used %s (HP+%d, MP+%d)", character_id, item->name, hp_changed, mp_changed);
}

/** Remove one inventory unit and create its ground item. */
void handle_drop_item(int client_fd, uint32_t character_id, uint8_t* buffer, ssize_t bytes) {
    if (bytes < (ssize_t)sizeof(DropItemPacket)) {
        LOG_WARN_RL(5, 60, "Invalid drop packet size");
        return;
    }

    DropItemPacket* drop = (DropItemPacket*)buffer;
    uint8_t inventory_slot = drop->inventory_slot;

    ActivePlayer* player = player_acquire(character_id);
    if (!player) return;
    if (!player->is_loaded) { player_release(player); return; }

    if (inventory_slot >= INVENTORY_SLOTS ||
        player->inventory[inventory_slot].instance_id == 0) {
        player_release(player);
        return;
    }

    uint32_t item_id = player->inventory[inventory_slot].item_id;
    float drop_x = player->pos_x;
    float drop_y = player->pos_y;
    // match the removed quantity to the ground item
    inventory_remove_at(player->inventory, inventory_slot, 1);
    player->is_dirty = 1;

    player_release(player);

    // Spawn item on the ground near the player
    loot_drop_item(item_id, 1, drop_x, drop_y, character_id);
    LOG_DEBUG("Character %u dropped item %u", character_id, item_id);

    DropItemResponsePacket response = {0};
    response.header.type = PACKET_DROP_ITEM_RESPONSE;
    response.header.player_id = htonl(character_id);
    response.header.payload_size = htons(sizeof(DropItemResponsePacket) - sizeof(PacketHeader));
    response.success = 1;
    response.dropped_item = htonl(item_id);
    server_send(client_fd, &response, sizeof(response));

    // synchronize the remaining stack
    {
        uint16_t changed[1] = { inventory_slot };
        player_send_slot_updates(client_fd, character_id, changed, 1);
    }
}

/** Move, merge, or swap inventory slots requested by a client. */
void handle_move_item(int client_fd, uint32_t character_id, uint8_t* buffer, ssize_t bytes) {
    if (bytes < (ssize_t)sizeof(MoveItemPacket)) {
        LOG_WARN_RL(5, 60, "Invalid move item packet size");
        return;
    }

    MoveItemPacket* move = (MoveItemPacket*)buffer;
    uint8_t from_slot = move->from_slot;
    uint8_t to_slot = move->to_slot;

    if (from_slot >= INVENTORY_SLOTS || to_slot >= INVENTORY_SLOTS) return;

    ActivePlayer* player = player_acquire(character_id);
    if (!player) return;
    if (!player->is_loaded) { player_release(player); return; }

    // retain merge overflow in the source slot
    const ItemDefinition* moved = item_get(player->inventory[from_slot].item_id);
    inventory_move(player->inventory, from_slot, to_slot,
                   moved ? moved->max_stack : 1);
    player->is_dirty = 1;

    player_release(player);

    MoveItemResponsePacket response = {0};
    response.header.type = PACKET_MOVE_ITEM_RESPONSE;
    response.header.player_id = htonl(character_id);
    response.header.payload_size = htons(sizeof(MoveItemResponsePacket) - sizeof(PacketHeader));
    response.success = 1;
    response.from_slot = from_slot;
    response.to_slot = to_slot;

    server_send(client_fd, &response, sizeof(response));

    // synchronize both merge or swap endpoints
    {
        uint16_t changed[2] = { from_slot, to_slot };
        player_send_slot_updates(client_fd, character_id, changed, 2);
    }

    LOG_DEBUG("Character %u moved item from slot %u to %u", character_id, from_slot, to_slot);
}

/* Chat lives in chat.c now.
 *
 * Recipient selection, whisper parsing and fan-out were written out here, in
 * the generic packet handler -- and the global-channel fan-out ran inline on
 * the network loop thread, taking the registry read lock and every online
 * player's slot mutex in turn for one player's keystroke. See chat.h.
 */

/** Validate and deliver a party invitation by player name. */
void handle_party_invite(int client_fd, uint32_t character_id, uint8_t* buffer, ssize_t bytes) {
    if (bytes < (ssize_t)sizeof(PartyInvitePacket)) return;

    PartyInvitePacket* pkt = (PartyInvitePacket*)buffer;
    char target_name[32];
    memset(target_name, 0, sizeof(target_name));
    STR_COPY_FIELD(target_name, pkt->target_name);

    // Acquire inviter, extract all needed fields, release immediately
    ActivePlayer* inviter = player_acquire(character_id);
    if (!inviter) return;
    uint32_t inviter_party_id = inviter->party_id;
    char inviter_name[32];
    strncpy(inviter_name, inviter->username, 31);
    inviter_name[31] = '\0';
    player_release(inviter);

    /* Find the target by name.
     *
     * One index lookup, on a network loop thread, in place of a walk over
     * every online player taking each one's mutex until the strings matched. */
    uint32_t target_id = player_find_by_name(target_name);

    if (target_id == 0) {
        LOG_WARN_RL(5, 60, "[PARTY] Invite failed: player '%s' not found", target_name);
        return;
    }

    // Can't invite yourself
    if (target_id == character_id) {
        LOG_WARN_RL(5, 60, "[PARTY] Invite failed: can't invite yourself");
        return;
    }

    /* Advisory: the invitation is only a hint that an accept may succeed, and
     * party_add_member() re-decides membership atomically when it arrives. These
     * checks exist so the inviter learns immediately that it will not work. */
    if (party_id_of_player(target_id) != 0) {
        LOG_WARN_RL(5, 60, "[PARTY] Invite failed: '%s' already in a party", target_name);
        return;
    }

    if (inviter_party_id != 0) {
        // Already in a party — check if leader and if party is full
        PartySnapshot inviter_party;
        if (party_snapshot(inviter_party_id, &inviter_party)) {
            if (inviter_party.leader_id != character_id) {
                LOG_WARN_RL(5, 60, "[PARTY] Invite failed: not party leader");
                return;
            }
            if (inviter_party.member_count >= MAX_PARTY_SIZE) {
                LOG_WARN_RL(5, 60, "[PARTY] Invite failed: party full");
                return;
            }
        }
    }

    // Create the invite
    if (!party_invite_create(character_id, target_id, inviter_party_id)) {
        LOG_WARN_RL(5, 60, "[PARTY] Invite failed: target already has pending invite or no slots");
        return;
    }

    // Send notification to target
    PartyInviteNotifyPacket notify = {0};
    notify.header.type = PACKET_PARTY_INVITE_NOTIFY;
    notify.header.player_id = htonl(target_id);
    notify.header.payload_size = htons(sizeof(PartyInviteNotifyPacket) - sizeof(PacketHeader));
    notify.from_id = htonl(character_id);
    STR_COPY_FIELD(notify.from_name, inviter_name);

    ActivePlayer* target = player_acquire(target_id);
    if (target) {
        int fd = target->client_fd;
        player_release(target);
        server_send(fd, &notify, sizeof(notify));
    }

    LOG_DEBUG("[PARTY] Player %u invited '%s' (%u) to party", character_id, target_name, target_id);
    (void)client_fd;
}

/** Consume a pending invitation and join or create its party. */
void handle_party_accept(int client_fd, uint32_t character_id) {
    (void)client_fd;

    PendingInvite inv;
    if (!party_invite_find_for_player(character_id, &inv)) {
        LOG_WARN_RL(5, 60, "[PARTY] Accept failed: no pending invite for player %u", character_id);
        return;
    }

    uint32_t from_id = inv.from_id;
    uint32_t existing_party = inv.party_id;

    // Clear the invite
    party_invite_remove(character_id);

    /* No "is the acceptor already in a party" check here. It cannot be made to
     * mean anything from outside the module -- the answer can change between the
     * test and the insert -- so party_add_member() decides it under the pool
     * lock and refuses there. */
    uint32_t party_id;

    if (existing_party != 0) {
        // Join existing party
        PartySnapshot target;
        if (!party_snapshot(existing_party, &target)) {
            LOG_WARN_RL(5, 60, "[PARTY] Accept failed: party %u no longer exists", existing_party);
            return;
        }
        party_id = existing_party;
    } else {
        // reuse a party the inviter joined since the invitation
        party_id = party_id_of_player(from_id);
        if (party_id == 0) {
            party_id = party_create(from_id);
            if (party_id == 0) {
                /* Lost a race, or the pool is full. Re-read rather than fail:
                 * a concurrent accept may have just created the inviter's party. */
                party_id = party_id_of_player(from_id);
            } else {
                party_broadcast_update(party_id);
            }

            if (party_id == 0) {
                LOG_WARN_RL(5, 60, "[PARTY] Accept failed: couldn't create party");
                return;
            }
        }
    }

    if (!party_add_member(party_id, character_id)) {
        LOG_WARN_RL(5, 60, "[PARTY] Accept failed: couldn't add player %u to party %u", character_id, party_id);
        return;
    }

    LOG_DEBUG("[PARTY] Player %u accepted invite, joined party %u", character_id, party_id);
    party_broadcast_update(party_id);
}

/** Remove the character's pending party invitation. */
void handle_party_decline(int client_fd, uint32_t character_id) {
    (void)client_fd;
    party_invite_remove(character_id);
    LOG_DEBUG("[PARTY] Player %u declined party invite", character_id);
}

/** Remove the character from its current party. */
void handle_party_leave(int client_fd, uint32_t character_id) {
    (void)client_fd;
    party_remove_member(character_id);
    LOG_DEBUG("[PARTY] Player %u left their party", character_id);
}

/** Remove a target member when requested by the party leader. */
void handle_party_kick(int client_fd, uint32_t character_id, uint8_t* buffer, ssize_t bytes) {
    (void)client_fd;
    if (bytes < (ssize_t)sizeof(PartyKickPacket)) return;

    PartyKickPacket* pkt = (PartyKickPacket*)buffer;
    uint32_t target_id = ntohl(pkt->target_id);

    // Can't kick yourself (use leave)
    if (target_id == character_id) return;

    /* One snapshot answers both questions -- leadership and target membership --
     * from a single consistent instant, instead of two unsynchronized reads of
     * a pool slot that another thread may recycle in between. */
    PartySnapshot party;
    if (!party_snapshot_of_player(character_id, &party)) return;

    // Only leader can kick
    if (party.leader_id != character_id) return;

    // Verify target is in the same party
    int found = 0;
    for (int i = 0; i < MAX_PARTY_SIZE; i++) {
        if (party.members[i] == target_id) {
            found = 1;
            break;
        }
    }
    if (!found) return;

    LOG_DEBUG("[PARTY] Player %u kicked player %u from party", character_id, target_id);
    party_remove_member(target_id);
}
/**
 * Answer a name query: resolve as many character identifiers as are online.
 *
 * This is what replaced the client labelling nearby players "Player_<id>". The
 * alternative was a name field in every nearby-player broadcast -- 32 bytes
 * per player, thirty-two players, twenty times a second, to resend a constant.
 * A name changes never; asking once and caching is the right shape.
 *
 * Deliberately narrower than the session roster this sits next to. It answers
 * only identifiers the caller already named, so it tells a client nothing it
 * could not already see, and it cannot be walked to enumerate the population:
 * character ids are a dense serial range, but a client that guesses one learns
 * only a name it could have learned by standing next to that player.
 */
void handle_name_query_request(int client_fd, uint8_t* buffer, ssize_t bytes) {
    if (bytes < (ssize_t)sizeof(NameQueryRequestPacket)) return;

    NameQueryRequestPacket* req = (NameQueryRequestPacket*)buffer;

    uint8_t asked = req->count;
    if (asked > MAX_NAME_QUERY) asked = MAX_NAME_QUERY;
    if (asked == 0) return;

    NameQueryResponsePacket resp;
    memset(&resp, 0, sizeof(resp));
    resp.header.type = PACKET_NAME_QUERY_RESPONSE;

    int found = 0;
    for (uint8_t i = 0; i < asked; i++) {
        uint32_t id = ntohl(req->character_ids[i]);
        if (id == 0) continue;

        ActivePlayer* p = player_acquire(id);
        if (!p) continue;   /* offline: left out, not returned blank */

        resp.entries[found].character_id = htonl(id);
        STR_COPY_FIELD(resp.entries[found].name, p->username);
        player_release(p);
        found++;
    }

    if (found == 0) return;

    resp.count = (uint8_t)found;

    /* Only the entries that were filled. The packet's array is sized for the
     * worst case; sending all of it would send 31 zeroed entries to answer one
     * name. */
    size_t send_size = offsetof(NameQueryResponsePacket, entries) +
                       (size_t)found * sizeof(NameQueryEntry);
    resp.header.payload_size = htons((uint16_t)(send_size - sizeof(PacketHeader)));

    server_send(client_fd, &resp, send_size);
    LOG_DEBUG("[NAME] resolved %d of %u requested names", found, asked);
}

/**
 * Build and send one page of online player summaries.
 *
 * Gated, because this is an enumeration surface.
 *
 * It returns a paginated roster of *every* online player -- id, name, level,
 * race and ping -- to any authenticated client, with no scoping to who is
 * nearby, who is in a party, or who is a friend. Whatever it was built for
 * (a "who" list, a debug view), what it hands out is the population of the
 * world on demand: a complete list of who is playing right now, refreshable,
 * and correlating names with characters and activity times.
 *
 * The list itself is a reasonable feature to have, so it is disabled by
 * default rather than deleted, and a deployment that wants it says so with
 * `session_list = on` in the world's .conf. When it is off the request is
 * ignored -- not answered with an error, because a distinguishable refusal
 * still confirms the world is there and the client is authenticated.
 */
void handle_session_list_request(int client_fd, uint8_t* buffer, ssize_t bytes) {
    extern ActivePlayer active_players[];

    if (!g_server.session_list_enabled) {
        LOG_DEBUG("[SESSION] roster request refused: session_list is off");
        return;
    }

    if (bytes < (ssize_t)sizeof(SessionListRequestPacket)) return;

    SessionListRequestPacket* req = (SessionListRequestPacket*)buffer;
    uint16_t requested_page = ntohs(req->page);

    // snapshot online records before pagination
    typedef struct {
        uint32_t character_id;
        char     name[32];
        int      level;
        uint8_t  race_id;
        uint16_t ping_ms;
    } Snap;

    Snap* snaps = NULL;
    int   snap_count = 0;

    player_registry_rdlock();
    int online_count = 0;
    const int* online = player_active_list_locked(&online_count);
    // Count first
    for (int n = 0; n < online_count; n++) {
        int i = online[n];
        if (active_players[i].is_loaded && active_players[i].is_ready)
            snap_count++;
    }
    if (snap_count > 0) {
        snaps = malloc((size_t)snap_count * sizeof(Snap));
        if (!snaps) {
            // Allocation failure: treat as zero players rather than crash
            snap_count = 0;
        } else {
            int idx = 0;
            for (int n = 0; n < online_count && idx < snap_count; n++) {
                int i = online[n];
                ActivePlayer* ap = &active_players[i];
                if (!ap->is_loaded || !ap->is_ready) continue;
                // copy each record under its player lock
                pthread_mutex_lock(&ap->lock);
                snaps[idx].character_id = ap->character_id;
                strncpy(snaps[idx].name, ap->username, 31);
                snaps[idx].name[31]     = '\0';
                snaps[idx].level        = ap->level;
                snaps[idx].race_id = (uint8_t)ap->race_id;
                snaps[idx].ping_ms      = ap->ping_ms;
                pthread_mutex_unlock(&ap->lock);
                idx++;
            }
            snap_count = idx;
        }
    }
    player_registry_unlock();

    uint32_t total_players = (uint32_t)snap_count;
    uint16_t total_pages   = (uint16_t)((snap_count + SESSION_LIST_PAGE_SIZE - 1) / SESSION_LIST_PAGE_SIZE);
    if (total_pages == 0) total_pages = 1;
    if (requested_page >= total_pages) requested_page = total_pages - 1;

    int page_start = (int)requested_page * SESSION_LIST_PAGE_SIZE;
    int page_count = snap_count - page_start;
    if (page_count > SESSION_LIST_PAGE_SIZE) page_count = SESSION_LIST_PAGE_SIZE;
    if (page_count < 0) page_count = 0;

    SessionListResponsePacket resp;
    memset(&resp, 0, sizeof(resp));
    resp.header.type         = PACKET_SESSION_LIST_RESPONSE;
    resp.header.payload_size = htons(sizeof(SessionListResponsePacket) - sizeof(PacketHeader));
    resp.total_players       = htonl(total_players);
    resp.total_pages         = htons(total_pages);
    resp.current_page        = htons(requested_page);
    resp.count               = (uint8_t)page_count;

    for (int i = 0; i < page_count; i++) {
        Snap* s = &snaps[page_start + i];
        SessionPlayerEntry* e = &resp.entries[i];
        e->player_id    = htonl(s->character_id);
        strncpy(e->name, s->name, 31);
        e->name[31]     = '\0';
        e->level        = (uint8_t)(s->level > 255 ? 255 : s->level);
        /* Race and class fuse into one identifier; both wire fields carry it. */
        e->player_class = s->race_id;
        e->player_race  = s->race_id;
        e->ping_ms      = htons(s->ping_ms);
    }

    free(snaps);

    size_t send_size = offsetof(SessionListResponsePacket, entries) +
                       (size_t)page_count * sizeof(SessionPlayerEntry);
    server_send(client_fd, &resp, send_size);
    LOG_DEBUG("[SESSION] Sent page %u/%u (%d entries, %u total) to fd=%d", requested_page + 1, total_pages, page_count, total_players, client_fd);
}
