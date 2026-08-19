/**
 * @file
 * Handle authenticated world packets for movement, inventory, chat, parties, and sessions.
 */

#include "packet_handler.h"
#include "log.h"
#include "player_data.h"
#include "zone_system.h"
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

/** Record client latency and echo its ping packet. */
void handle_ping(int client_fd, uint8_t* buffer, uint32_t character_id, int player_slot) {
    PacketHeader* hdr = (PacketHeader*)buffer;
    if (ntohs(hdr->payload_size) >= sizeof(uint16_t)) {
        uint16_t* ping_ptr = (uint16_t*)(buffer + sizeof(PacketHeader));
        uint16_t client_ping = ntohs(*ping_ptr);
        ActivePlayer* player = player_acquire_hint(character_id, player_slot);
        if (player) {
            player->ping_ms = client_ping;
            player_release(player);
        }
    }
    // echo the declared payload for stream framing
    server_send(client_fd, buffer, sizeof(PacketHeader) + sizeof(uint16_t));
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
            strncpy(zpkt.zone_name, zone->name, sizeof(zpkt.zone_name) - 1);
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

    // Send updated stats to client
    player_send_stats(client_fd, player);

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

    // Send updated stats to client
    player_send_stats(client_fd, player);

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

    if (from_slot >= 150 || to_slot >= 150) return;

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

#define CHAT_LOCAL_RANGE 800.0f

/** Sanitize and distribute a chat message by channel rules. */
void handle_chat_send(int client_fd, uint32_t character_id, uint8_t* buffer, ssize_t bytes) {
    (void)client_fd;  // Sender receives via broadcast loop

    if (bytes < (ssize_t)sizeof(ChatSendPacket)) {
        LOG_WARN_RL(5, 60, "Invalid chat packet size");
        return;
    }

    ChatSendPacket* chat = (ChatSendPacket*)buffer;

    // Sanitize: ensure null termination
    chat->message[MAX_CHAT_MESSAGE - 1] = '\0';

    // Reject empty messages
    if (chat->message[0] == '\0') return;

    uint8_t channel = chat->channel;

    // Get sender info — acquire lock, extract needed fields, release immediately
    ActivePlayer* sender = player_acquire(character_id);
    if (!sender) return;
    char sender_name[32];
    strncpy(sender_name, sender->username, sizeof(sender_name) - 1);
    sender_name[31] = '\0';
    float sender_x = sender->pos_x;
    float sender_y = sender->pos_y;
    uint32_t sender_party = sender->party_id;
    player_release(sender);

    // Build broadcast packet
    ChatMessagePacket msg = {0};
    msg.header.type = PACKET_CHAT_MESSAGE;
    msg.header.player_id = htonl(character_id);
    msg.header.payload_size = htons(sizeof(ChatMessagePacket) - sizeof(PacketHeader));
    msg.sender_id = htonl(character_id);
    msg.channel = channel;
    strncpy(msg.sender_name, sender_name, sizeof(msg.sender_name) - 1);
    strncpy(msg.message, chat->message, sizeof(msg.message) - 1);

    LOG_DEBUG("[CHAT] %s (ch=%u): %s", msg.sender_name, channel, msg.message);

    // collect recipients before performing socket writes
    extern ActivePlayer active_players[];

    int recipients[MAX_PLAYERS];
    int recipient_count = 0;

    player_registry_rdlock();
    int n_slots = 0;
    const int* slots = player_active_list_locked(&n_slots);

    if (channel == CHAT_CHANNEL_WHISPER) {
        // split "TargetName message" whisper framing
        const char* space = strchr(msg.message, ' ');
        if (!space || space == msg.message || *(space + 1) == '\0') {
            // Malformed — no target name or no body; silently drop
            player_registry_unlock();
            return;
        }

        char target_name[32] = {0};
        size_t name_len = (size_t)(space - msg.message);
        if (name_len >= sizeof(target_name)) name_len = sizeof(target_name) - 1;
        memcpy(target_name, msg.message, name_len);

        // Rewrite msg.message to just the body text
        char body[MAX_CHAT_MESSAGE];
        strncpy(body, space + 1, sizeof(body) - 1);
        body[sizeof(body) - 1] = '\0';
        memset(msg.message, 0, sizeof(msg.message));
        strncpy(msg.message, body, sizeof(msg.message) - 1);

        LOG_DEBUG("[WHISPER] %s -> %s: %s", sender_name, target_name, msg.message);

        // Find target
        int target_fd = -1;
        for (int s = 0; s < n_slots; s++) {
            int i = slots[s];
            if (!active_players[i].is_loaded) continue;
            pthread_mutex_lock(&active_players[i].lock);
            int match = (strncmp(active_players[i].username, target_name, 31) == 0);
            int fd    = active_players[i].client_fd;
            pthread_mutex_unlock(&active_players[i].lock);
            if (match) {
                target_fd = fd;
                break;
            }
        }

        player_registry_unlock();

        if (target_fd == -1) {
            LOG_WARN_RL(5, 60, "[WHISPER] Target '%s' not online", target_name);
        } else {
            server_send(target_fd, &msg, sizeof(msg));
            // label the sender's whisper echo with its target
            ChatMessagePacket echo = msg;
            memset(echo.sender_name, 0, sizeof(echo.sender_name));
            snprintf(echo.sender_name, sizeof(echo.sender_name), "-> %.28s", target_name);
            server_send(client_fd, &echo, sizeof(echo));
        }
        return;
    }

    for (int s = 0; s < n_slots; s++) {
        int i = slots[s];
        if (!active_players[i].is_loaded) continue;

        pthread_mutex_lock(&active_players[i].lock);
        int      fd          = active_players[i].client_fd;
        float    dx          = active_players[i].pos_x - sender_x;
        float    dy          = active_players[i].pos_y - sender_y;
        uint32_t their_party = active_players[i].party_id;
        pthread_mutex_unlock(&active_players[i].lock);

        int wants = 0;
        if (channel == CHAT_CHANNEL_LOCAL)
            wants = (dx * dx + dy * dy) <= CHAT_LOCAL_RANGE * CHAT_LOCAL_RANGE;
        else if (channel == CHAT_CHANNEL_GLOBAL)
            wants = 1;
        else if (channel == CHAT_CHANNEL_PARTY)
            wants = (sender_party != 0 && their_party == sender_party);

        if (wants) recipients[recipient_count++] = fd;
    }

    player_registry_unlock();

    for (int r = 0; r < recipient_count; r++)
        server_send(recipients[r], &msg, sizeof(msg));
}

/** Validate and deliver a party invitation by player name. */
void handle_party_invite(int client_fd, uint32_t character_id, uint8_t* buffer, ssize_t bytes) {
    if (bytes < (ssize_t)sizeof(PartyInvitePacket)) return;

    PartyInvitePacket* pkt = (PartyInvitePacket*)buffer;
    char target_name[32];
    memset(target_name, 0, sizeof(target_name));
    strncpy(target_name, pkt->target_name, 31);

    // Acquire inviter, extract all needed fields, release immediately
    ActivePlayer* inviter = player_acquire(character_id);
    if (!inviter) return;
    uint32_t inviter_party_id = inviter->party_id;
    char inviter_name[32];
    strncpy(inviter_name, inviter->username, 31);
    inviter_name[31] = '\0';
    player_release(inviter);

    // Find target by name
    extern ActivePlayer active_players[];

    uint32_t target_id = 0;
    player_registry_rdlock();
    int online_count = 0;
    const int* online = player_active_list_locked(&online_count);
    for (int n = 0; n < online_count; n++) {
        int i = online[n];
        if (!active_players[i].is_loaded) continue;
        pthread_mutex_lock(&active_players[i].lock);
        if (strncmp(active_players[i].username, target_name, 31) == 0) {
            target_id = active_players[i].character_id;
            pthread_mutex_unlock(&active_players[i].lock);
            break;
        }
        pthread_mutex_unlock(&active_players[i].lock);
    }
    player_registry_unlock();

    if (target_id == 0) {
        LOG_WARN_RL(5, 60, "[PARTY] Invite failed: player '%s' not found", target_name);
        return;
    }

    // Can't invite yourself
    if (target_id == character_id) {
        LOG_WARN_RL(5, 60, "[PARTY] Invite failed: can't invite yourself");
        return;
    }

    // Check if target is already in a party
    Party* target_party = party_find_by_player(target_id);
    if (target_party) {
        LOG_WARN_RL(5, 60, "[PARTY] Invite failed: '%s' already in a party", target_name);
        return;
    }

    if (inviter_party_id != 0) {
        // Already in a party — check if leader and if party is full
        Party* p = party_find(inviter_party_id);
        if (p) {
            pthread_mutex_lock(&p->lock);
            if (p->leader_id != character_id) {
                pthread_mutex_unlock(&p->lock);
                LOG_WARN_RL(5, 60, "[PARTY] Invite failed: not party leader");
                return;
            }
            if (p->member_count >= MAX_PARTY_SIZE) {
                pthread_mutex_unlock(&p->lock);
                LOG_WARN_RL(5, 60, "[PARTY] Invite failed: party full");
                return;
            }
            pthread_mutex_unlock(&p->lock);
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
    strncpy(notify.from_name, inviter_name, 31);

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

    PendingInvite* inv = party_invite_find_for_player(character_id);
    if (!inv) {
        LOG_WARN_RL(5, 60, "[PARTY] Accept failed: no pending invite for player %u", character_id);
        return;
    }

    uint32_t from_id = inv->from_id;
    uint32_t existing_party = inv->party_id;

    // Clear the invite
    party_invite_remove(character_id);

    // Check if acceptor is already in a party
    Party* already = party_find_by_player(character_id);
    if (already) {
        LOG_WARN_RL(5, 60, "[PARTY] Accept failed: player %u already in a party", character_id);
        return;
    }

    uint32_t party_id;

    if (existing_party != 0) {
        // Join existing party
        Party* p = party_find(existing_party);
        if (!p) {
            LOG_WARN_RL(5, 60, "[PARTY] Accept failed: party %u no longer exists", existing_party);
            return;
        }
        party_id = existing_party;
    } else {
        // reuse a party joined since the invitation
        Party* inviter_party = party_find_by_player(from_id);
        if (inviter_party) {
            party_id = inviter_party->party_id;
        } else {
            party_id = party_create(from_id);
            if (party_id == 0) {
                LOG_WARN_RL(5, 60, "[PARTY] Accept failed: couldn't create party");
                return;
            }
            party_broadcast_update(party_id);
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

    // Find kicker's party
    Party* p = party_find_by_player(character_id);
    if (!p) return;

    pthread_mutex_lock(&p->lock);
    // Only leader can kick
    if (p->leader_id != character_id) {
        pthread_mutex_unlock(&p->lock);
        return;
    }

    // Verify target is in the same party
    int found = 0;
    for (int i = 0; i < MAX_PARTY_SIZE; i++) {
        if (p->members[i] == target_id) {
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&p->lock);

    if (!found) return;

    LOG_DEBUG("[PARTY] Player %u kicked player %u from party", character_id, target_id);
    party_remove_member(target_id);
}
/** Build and send one page of online player summaries. */
void handle_session_list_request(int client_fd, uint8_t* buffer, ssize_t bytes) {
    extern ActivePlayer active_players[];

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
