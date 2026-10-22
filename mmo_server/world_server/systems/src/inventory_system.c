// Inventory rules: equip, unequip, use, drop and move, decided on the server.

#include "inventory_system.h"
#include "outbox.h"
#include "log.h"
#include "player_data.h"
#include "player_effects.h"
#include "player_level.h"
#include "item_registry.h"
#include "loot.h"
#include "str_fixed.h"
#include "utils.h"
#include "zone_system.h"
#include "emote_handler.h"
#include "npc_query.h"
#include "friends.h"
#include "party.h"
#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
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

/** Validate and apply an inventory-to-equipment transfer. */
void inventory_system_equip(int client_fd, uint32_t character_id, uint32_t item_id, uint8_t inventory_slot, uint8_t equip_slot) {
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
        outbox_send(client_fd, &response, sizeof(response));
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
        outbox_send(client_fd, &response, sizeof(response));
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
        outbox_send(client_fd, &response, sizeof(response));
        return;
    }

    // Check if player meets requirements
    if (!item_can_equip(item_id, player->level, player->race_id)) {
        LOG_WARN_RL(5, 60, "Player cannot equip item: %s", item->name);
        player_release(player);

        EquipItemResponsePacket response = {0};
        response.header.type = PACKET_EQUIP_ITEM_RESPONSE;
        response.header.player_id = htonl(character_id);
        response.success = 0;
        strncpy(response.message, "Requirements not met", sizeof(response.message) - 1);
        outbox_send(client_fd, &response, sizeof(response));
        return;
    }

    int equip_index = equip_index_for(equip_slot);
    if (equip_index < 0) {
        player_release(player);
        return;
    }

    /* Enforce two-handed and off-hand exclusivity.
     *
     * Displacing the off-hand touches two slots beyond the two this equip
     * would otherwise change, and they have to be told to the client with the
     * rest. They were not, so a two-handed weapon equipped over a shield left
     * the client drawing the shield still worn and the bag slot it had moved
     * into still empty -- and nothing later corrected it, because the client
     * answers a successful equip with a stats request, and stats carry no
     * inventory. Recorded here and appended to the update below. */
    int displaced_slot = -1;

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
            outbox_send(client_fd, &response, sizeof(response));
            return;
        }
        player->inventory[offhand_slot] = player->equipment[EQUIP_SECOND_HAND];
        memset(&player->equipment[EQUIP_SECOND_HAND], 0, sizeof(ItemInstance));
        displaced_slot = offhand_slot;
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
            outbox_send(client_fd, &response, sizeof(response));
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

    outbox_send(client_fd, &response, sizeof(response));

    // synchronize every slot this equip changed, the displaced off-hand included
    {
        uint16_t changed[4];
        int n = 0;
        changed[n++] = inventory_slot;
        changed[n++] = (uint16_t)(EQUIP_SLOT_BASE + equip_index);
        if (displaced_slot >= 0) {
            changed[n++] = (uint16_t)displaced_slot;
            changed[n++] = (uint16_t)(EQUIP_SLOT_BASE + EQUIP_SECOND_HAND);
        }
        player_send_slot_updates(client_fd, character_id, changed, n);
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
void inventory_system_unequip(int client_fd, uint32_t character_id, uint8_t equip_slot) {
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
        outbox_send(client_fd, &response, sizeof(response));
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

    outbox_send(client_fd, &response, sizeof(response));

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
void inventory_system_use(int client_fd, uint32_t character_id, uint8_t inventory_slot) {
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
        outbox_send(client_fd, &response, sizeof(response));
        return;
    }

    uint32_t item_id = player->inventory[inventory_slot].item_id;
    const ItemDefinition* item = item_get(item_id);
    if (!item) {
        player_release(player);
        response.success = 0;
        strncpy(response.message, "Invalid item", sizeof(response.message) - 1);
        outbox_send(client_fd, &response, sizeof(response));
        return;
    }

    // Must be a consumable
    if (item->type != ITEM_TYPE_CONSUMABLE || item->use_effect == USE_EFFECT_NONE) {
        player_release(player);
        response.success = 0;
        strncpy(response.message, "Item is not consumable", sizeof(response.message) - 1);
        outbox_send(client_fd, &response, sizeof(response));
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
            outbox_send(client_fd, &response, sizeof(response));
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
    outbox_send(client_fd, &response, sizeof(response));

    // synchronize the remaining stack
    {
        uint16_t changed[1] = { inventory_slot };
        player_send_slot_updates(client_fd, character_id, changed, 1);
    }

    LOG_DEBUG("Character %u used %s (HP+%d, MP+%d)", character_id, item->name, hp_changed, mp_changed);
}

/** Remove one inventory unit and create its ground item. */
void inventory_system_drop(int client_fd, uint32_t character_id, uint8_t inventory_slot) {
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
    float drop_z = player->pos_z;
    // match the removed quantity to the ground item
    inventory_remove_at(player->inventory, inventory_slot, 1);
    player->is_dirty = 1;

    player_release(player);

    // Spawn item on the ground near the player
    loot_drop_item(item_id, 1, drop_x, drop_y, drop_z, character_id);
    LOG_DEBUG("Character %u dropped item %u", character_id, item_id);

    DropItemResponsePacket response = {0};
    response.header.type = PACKET_DROP_ITEM_RESPONSE;
    response.header.player_id = htonl(character_id);
    response.header.payload_size = htons(sizeof(DropItemResponsePacket) - sizeof(PacketHeader));
    response.success = 1;
    response.dropped_item = htonl(item_id);
    outbox_send(client_fd, &response, sizeof(response));

    // synchronize the remaining stack
    {
        uint16_t changed[1] = { inventory_slot };
        player_send_slot_updates(client_fd, character_id, changed, 1);
    }
}

/** Move, merge, or swap inventory slots requested by a client. */
void inventory_system_move(int client_fd, uint32_t character_id, uint8_t from_slot, uint8_t to_slot) {
    if (from_slot >= INVENTORY_SLOTS || to_slot >= INVENTORY_SLOTS) return;

    ActivePlayer* player = player_acquire(character_id);
    if (!player) return;
    if (!player->is_loaded) { player_release(player); return; }

    // retain merge overflow in the source slot
    const ItemDefinition* moved = item_get(player->inventory[from_slot].item_id);
    int moved_ok = inventory_move(player->inventory, from_slot, to_slot,
                                  moved ? moved->max_stack : 1);
    if (moved_ok) player->is_dirty = 1;

    player_release(player);

    /* Answer with what actually happened. This reported success unconditionally,
     * including when inventory_move() had refused -- an empty source slot, or a
     * slot dragged onto itself -- which made the client's own put-it-back path
     * unreachable and left the player's bag showing a move the server had not
     * made until some later update happened to overwrite it. */
    MoveItemResponsePacket response = {0};
    response.header.type = PACKET_MOVE_ITEM_RESPONSE;
    response.header.player_id = htonl(character_id);
    response.header.payload_size = htons(sizeof(MoveItemResponsePacket) - sizeof(PacketHeader));
    response.success = moved_ok ? 1 : 0;
    response.from_slot = from_slot;
    response.to_slot = to_slot;

    outbox_send(client_fd, &response, sizeof(response));

    // synchronize both merge or swap endpoints
    {
        uint16_t changed[2] = { from_slot, to_slot };
        player_send_slot_updates(client_fd, character_id, changed, 2);
    }

    LOG_DEBUG("Character %u moved item from slot %u to %u (%s)", character_id,
              from_slot, to_slot, moved_ok ? "applied" : "refused");
}
