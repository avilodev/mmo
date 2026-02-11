#include "packet_handler.h"

void handle_ping(int client_fd, uint8_t* buffer) {
    // Echo back the ping packet
    send(client_fd, buffer, sizeof(PacketHeader), 0);
}

void handle_request_player_data(int client_fd, uint32_t character_id) {
    // Use the same function that sends CharacterInfo + PlayerStatsPacket
    player_send_data_response(client_fd, character_id);
}

void handle_player_move(int client_fd, uint32_t character_id, PlayerMovePacket* pkt) {
    ActivePlayer* player = player_find_active(character_id);
    if (!player) return;

    pthread_mutex_lock(&player->lock);

    float client_x = pkt->pos_x;
    float client_y = pkt->pos_y;

    struct timeval now;
    gettimeofday(&now, NULL);
    
    double time_delta = (now.tv_sec - player->last_move_tv.tv_sec) + 
                        (now.tv_usec - player->last_move_tv.tv_usec) / 1000000.0;
    
    if (time_delta < 0.001) time_delta = 0.1;
    
    float dx = client_x - player->pos_x;
    float dy = client_y - player->pos_y;
    float distance = sqrtf(dx * dx + dy * dy);

    // Use SERVER-SIDE move speed (anti-cheat: ignore client speed)
    float server_speed = player->move_speed;
    if (server_speed <= 0.0f) server_speed = 200.0f;  
    float max_speed = server_speed * 3.0f;
    float max_distance = max_speed * time_delta;

    if (distance > max_distance) {
        printf("[MOVE DEBUG] REJECTED - distance %f > max %f (delta=%f, speed=%f)\n", 
               distance, max_distance, time_delta, pkt->player_speed);
        
        PlayerMoveAckPacket correction;
        memset(&correction, 0, sizeof(correction));
        correction.header.type = PACKET_PLAYER_MOVE_ACK;
        correction.pos_x = player->pos_x;
        correction.pos_y = player->pos_y;
        
        send(client_fd, &correction, sizeof(correction), 0);
        pthread_mutex_unlock(&player->lock);
        return;
    }

    player->pos_x = client_x;
    player->pos_y = client_y;
    player->last_move_tv = now;
    player->is_dirty = 1;

    pthread_mutex_unlock(&player->lock);
}

void handle_equip_item(int client_fd, uint32_t character_id, uint8_t* buffer, ssize_t bytes) {
    if (bytes < (ssize_t)sizeof(EquipItemPacket)) {
        printf("Invalid equip packet size\n");
        return;
    }
    
    EquipItemPacket* equip = (EquipItemPacket*)buffer;
    uint32_t item_id = ntohl(equip->item_id);
    uint8_t inventory_slot = equip->inventory_slot;
    uint8_t equip_slot = equip->equip_slot;
    
    ActivePlayer* player = player_find_active(character_id);
    if (!player) {
        printf("Player not found for equip\n");
        return;
    }
    
    pthread_mutex_lock(&player->lock);
    
    // Verify item is in inventory
    if (inventory_slot >= 150 || player->inventory[inventory_slot] != item_id) {
        printf("Item %u not in inventory slot %u\n", item_id, inventory_slot);
        pthread_mutex_unlock(&player->lock);
        
        // Send error response
        EquipItemResponsePacket response = {0};
        response.header.type = PACKET_EQUIP_ITEM_RESPONSE;
        response.header.player_id = htonl(character_id);
        response.success = 0;
        strcpy(response.message, "Item not in inventory");
        send(client_fd, &response, sizeof(response), 0);
        return;
    }
    
    // Get item definition
    const ItemDefinition* item = item_get(item_id);
    if (!item) {
        printf("Item %u does not exist\n", item_id);
        pthread_mutex_unlock(&player->lock);
        
        EquipItemResponsePacket response = {0};
        response.header.type = PACKET_EQUIP_ITEM_RESPONSE;
        response.header.player_id = htonl(character_id);
        response.success = 0;
        strcpy(response.message, "Invalid item");
        send(client_fd, &response, sizeof(response), 0);
        return;
    }
    
    // Validate slot
    if (item->slot != equip_slot && !(item->is_two_handed && equip_slot == SLOT_MAIN_HAND)) {
        printf("Item %s cannot be equipped in slot %u\n", item->name, equip_slot);
        pthread_mutex_unlock(&player->lock);
        
        EquipItemResponsePacket response = {0};
        response.header.type = PACKET_EQUIP_ITEM_RESPONSE;
        response.header.player_id = htonl(character_id);
        response.success = 0;
        strcpy(response.message, "Invalid slot for this item");
        send(client_fd, &response, sizeof(response), 0);
        return;
    }
    
    // Check if player meets requirements
    if (!item_can_equip(item_id, player->level, player->player_class, player->player_race)) {
        printf("Player cannot equip item: %s\n", item->name);
        pthread_mutex_unlock(&player->lock);
        
        EquipItemResponsePacket response = {0};
        response.header.type = PACKET_EQUIP_ITEM_RESPONSE;
        response.header.player_id = htonl(character_id);
        response.success = 0;
        strcpy(response.message, "Requirements not met");
        send(client_fd, &response, sizeof(response), 0);
        return;
    }
    
    // Store the item currently equipped (for swapping back to inventory)
    uint32_t old_item = 0;
    
    // Equip the item
    switch (equip_slot) {
        case SLOT_HELMET:
            old_item = player->helmet;
            player->helmet = item_id;
            break;
            
        case SLOT_GLOVES:
            old_item = player->gloves;
            player->gloves = item_id;
            break;
            
        case SLOT_CHEST:
            old_item = player->chest_armor;
            player->chest_armor = item_id;
            break;
            
        case SLOT_LEGGINGS:
            old_item = player->leggings;
            player->leggings = item_id;
            break;
            
        case SLOT_BOOTS:
            old_item = player->boots;
            player->boots = item_id;
            break;
            
        case SLOT_MAIN_HAND:
            old_item = player->main_hand;
            player->main_hand = item_id;
            
            // If two-handed, clear off-hand
            if (item->is_two_handed && player->second_hand != 0) {
                // TODO: Find inventory slot for off-hand item
                player->second_hand = 0;
                player->second_hand_durability = 0;
            }
            break;
            
        case SLOT_OFF_HAND:
            // Check if main hand is two-handed
            if (player->main_hand != 0) {
                const ItemDefinition* main_hand = item_get(player->main_hand);
                if (main_hand && main_hand->is_two_handed) {
                    pthread_mutex_unlock(&player->lock);
                    
                    EquipItemResponsePacket response = {0};
                    response.header.type = PACKET_EQUIP_ITEM_RESPONSE;
                    response.header.player_id = htonl(character_id);
                    response.success = 0;
                    strcpy(response.message, "Cannot equip with two-handed weapon");
                    send(client_fd, &response, sizeof(response), 0);
                    return;
                }
            }
            
            old_item = player->second_hand;
            player->second_hand = item_id;
            break;
            
        default:
            pthread_mutex_unlock(&player->lock);
            return;
    }
    
    // Put old item back in inventory slot
    player->inventory[inventory_slot] = old_item;
    
    player->is_dirty = 1;
    pthread_mutex_unlock(&player->lock);
    
    // Send success response
    EquipItemResponsePacket response = {0};
    response.header.type = PACKET_EQUIP_ITEM_RESPONSE;
    response.header.player_id = htonl(character_id);
    response.success = 1;
    response.equipped_item = htonl(item_id);
    response.returned_item = htonl(old_item);
    snprintf(response.message, sizeof(response.message), "Equipped: %s", item->name);
    
    send(client_fd, &response, sizeof(response), 0);
    printf("Character %u equipped %s\n", character_id, item->name);
}

void handle_unequip_item(int client_fd, uint32_t character_id, uint8_t* buffer, ssize_t bytes) {
    if (bytes < (ssize_t)sizeof(UnequipItemPacket)) {
        printf("Invalid unequip packet size\n");
        return;
    }
    
    UnequipItemPacket* unequip = (UnequipItemPacket*)buffer;
    uint8_t equip_slot = unequip->equip_slot;
    
    ActivePlayer* player = player_find_active(character_id);
    if (!player) return;
    
    pthread_mutex_lock(&player->lock);
    
    // Find empty inventory slot
    int inventory_slot = -1;
    for (int i = 0; i < 150; i++) {
        if (player->inventory[i] == 0) {
            inventory_slot = i;
            break;
        }
    }
    
    if (inventory_slot == -1) {
        pthread_mutex_unlock(&player->lock);
        
        // Send error - inventory full
        UnequipItemResponsePacket response = {0};
        response.header.type = PACKET_UNEQUIP_ITEM_RESPONSE;
        response.header.player_id = htonl(character_id);
        response.success = 0;
        strcpy(response.message, "Inventory full");
        send(client_fd, &response, sizeof(response), 0);
        return;
    }
    
    uint32_t unequipped_item = 0;
    
    // Unequip the item
    switch (equip_slot) {
        case SLOT_HELMET:
            unequipped_item = player->helmet;
            player->helmet = 0;
            player->helmet_durability = 0;
            break;
        case SLOT_GLOVES:
            unequipped_item = player->gloves;
            player->gloves = 0;
            player->gloves_durability = 0;
            break;
        case SLOT_CHEST:
            unequipped_item = player->chest_armor;
            player->chest_armor = 0;
            player->chest_durability = 0;
            break;
        case SLOT_LEGGINGS:
            unequipped_item = player->leggings;
            player->leggings = 0;
            player->leggings_durability = 0;
            break;
        case SLOT_BOOTS:
            unequipped_item = player->boots;
            player->boots = 0;
            player->boots_durability = 0;
            break;
        case SLOT_MAIN_HAND:
            unequipped_item = player->main_hand;
            player->main_hand = 0;
            player->main_hand_durability = 0;
            break;
        case SLOT_OFF_HAND:
            unequipped_item = player->second_hand;
            player->second_hand = 0;
            player->second_hand_durability = 0;
            break;
        default:
            pthread_mutex_unlock(&player->lock);
            return;
    }
    
    if (unequipped_item == 0) {
        pthread_mutex_unlock(&player->lock);
        return; // Nothing was equipped
    }
    
    // Add to inventory
    player->inventory[inventory_slot] = unequipped_item;
    player->is_dirty = 1;
    
    pthread_mutex_unlock(&player->lock);
    
    // Send response
    UnequipItemResponsePacket response = {0};
    response.header.type = PACKET_UNEQUIP_ITEM_RESPONSE;
    response.header.player_id = htonl(character_id);
    response.success = 1;
    response.unequipped_item = htonl(unequipped_item);
    response.inventory_slot = inventory_slot;
    strcpy(response.message, "Item unequipped");
    
    send(client_fd, &response, sizeof(response), 0);
    printf("Character %u unequipped item from slot %u\n", character_id, equip_slot);
}

void handle_use_item(int client_fd, uint32_t character_id, uint8_t* buffer, ssize_t bytes) {
    if (bytes < (ssize_t)sizeof(UseItemPacket)) {
        printf("Invalid use item packet size\n");
        return;
    }
    
    UseItemPacket* use = (UseItemPacket*)buffer;
    uint8_t inventory_slot = use->inventory_slot;
    
    // TODO: Implement consumable item usage
    printf("Character %u used item in slot %u\n", character_id, inventory_slot);
    
    // For now, just acknowledge
    UseItemResponsePacket response = {0};
    response.header.type = PACKET_USE_ITEM_RESPONSE;
    response.header.player_id = htonl(character_id);
    response.success = 0;
    strcpy(response.message, "Not implemented yet");
    send(client_fd, &response, sizeof(response), 0);
}

void handle_drop_item(int client_fd, uint32_t character_id, uint8_t* buffer, ssize_t bytes) {
    if (bytes < (ssize_t)sizeof(DropItemPacket)) {
        printf("Invalid drop packet size\n");
        return;
    }
    
    DropItemPacket* drop = (DropItemPacket*)buffer;
    uint8_t inventory_slot = drop->inventory_slot;
    
    ActivePlayer* player = player_find_active(character_id);
    if (!player) return;
    
    pthread_mutex_lock(&player->lock);
    
    if (inventory_slot >= 150 || player->inventory[inventory_slot] == 0) {
        pthread_mutex_unlock(&player->lock);
        return;
    }
    
    uint32_t item_id = player->inventory[inventory_slot];
    player->inventory[inventory_slot] = 0;
    player->is_dirty = 1;
    
    pthread_mutex_unlock(&player->lock);
    
    // TODO: Spawn item in world
    printf("Character %u dropped item %u\n", character_id, item_id);
    
    DropItemResponsePacket response = {0};
    response.header.type = PACKET_DROP_ITEM_RESPONSE;
    response.header.player_id = htonl(character_id);
    response.success = 1;
    response.dropped_item = htonl(item_id);
    send(client_fd, &response, sizeof(response), 0);
}

void handle_move_item(int client_fd, uint32_t character_id, uint8_t* buffer, ssize_t bytes) {
    if (bytes < (ssize_t)sizeof(MoveItemPacket)) {
        printf("Invalid move item packet size\n");
        return;
    }
    
    MoveItemPacket* move = (MoveItemPacket*)buffer;
    uint8_t from_slot = move->from_slot;
    uint8_t to_slot = move->to_slot;
    
    if (from_slot >= 150 || to_slot >= 150) return;
    
    ActivePlayer* player = player_find_active(character_id);
    if (!player) return;
    
    pthread_mutex_lock(&player->lock);
    
    // Swap items
    uint32_t temp = player->inventory[from_slot];
    player->inventory[from_slot] = player->inventory[to_slot];
    player->inventory[to_slot] = temp;
    player->is_dirty = 1;
    
    pthread_mutex_unlock(&player->lock);
    
    MoveItemResponsePacket response = {0};
    response.header.type = PACKET_MOVE_ITEM_RESPONSE;
    response.header.player_id = htonl(character_id);
    response.success = 1;
    response.from_slot = from_slot;
    response.to_slot = to_slot;
    
    send(client_fd, &response, sizeof(response), 0);
    printf("Character %u moved item from slot %u to %u\n", character_id, from_slot, to_slot);
}