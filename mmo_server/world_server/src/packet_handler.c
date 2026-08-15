#include "packet_handler.h"
#include "zone_system.h"
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

void handle_ping(int client_fd, uint8_t* buffer, uint32_t character_id) {
    // Extract client-reported ping_ms from payload if present
    PacketHeader* hdr = (PacketHeader*)buffer;
    if (ntohs(hdr->payload_size) >= sizeof(uint16_t)) {
        uint16_t* ping_ptr = (uint16_t*)(buffer + sizeof(PacketHeader));
        uint16_t client_ping = ntohs(*ping_ptr);
        ActivePlayer* player = player_acquire(character_id);
        if (player) {
            player->ping_ms = client_ping;
            player_release(player);
        }
    }
    // Echo the full ping packet back (header + uint16_t payload) so the client
    // can measure RTT. Sending only the 7-byte header while declaring
    // payload_size=2 would desynchronize the client's TCP reassembly.
    server_send(client_fd, buffer, sizeof(PacketHeader) + sizeof(uint16_t));
}

void handle_request_player_data(int client_fd, uint32_t character_id) {
    // Use the same function that sends CharacterInfo + PlayerStatsPacket
    player_send_data_response(client_fd, character_id);
}

void handle_player_move(int client_fd, uint32_t character_id, PlayerMovePacket* pkt) {
    ActivePlayer* player = player_acquire(character_id);
    if (!player) return;
    if (!player->is_loaded) { player_release(player); return; }

    float client_x = pkt->pos_x;
    float client_y = pkt->pos_y;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    double time_delta = (now.tv_sec  - player->last_move_tv.tv_sec) +
                        (now.tv_nsec - player->last_move_tv.tv_nsec) / 1e9;
     
    if (time_delta < 0.001 || time_delta > 5.0) time_delta = 0.1;
    
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
        correction.header.payload_size = htons(sizeof(PlayerMoveAckPacket) - sizeof(PacketHeader)); 
        correction.pos_x = player->pos_x;
        correction.pos_y = player->pos_y;
        
        server_send(client_fd, &correction, sizeof(correction));
        player_release(player);
        return;
    }

    // Collision check — reject moves into solid tiles (player box = 16px half-size)
    if (world_collision_check_box(client_x, client_y, 16.0f)) {
        PlayerMoveAckPacket correction;
        memset(&correction, 0, sizeof(correction));
        correction.header.type = PACKET_PLAYER_MOVE_ACK;
        correction.header.payload_size = htons(sizeof(PlayerMoveAckPacket) - sizeof(PacketHeader));
        correction.pos_x = player->pos_x;
        correction.pos_y = player->pos_y;
        server_send(client_fd, &correction, sizeof(correction));
        player_release(player);
        return;
    }

    player->pos_x = client_x;
    player->pos_y = client_y;
    player->last_move_tv = now;
    player->is_dirty = 1;

    // Zone boundary check — notify client if they crossed into a new zone
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

void handle_equip_item(int client_fd, uint32_t character_id, uint8_t* buffer, ssize_t bytes) {
    if (bytes < (ssize_t)sizeof(EquipItemPacket)) {
        printf("Invalid equip packet size\n");
        return;
    }
    
    EquipItemPacket* equip = (EquipItemPacket*)buffer;
    uint32_t item_id = ntohl(equip->item_id);
    uint8_t inventory_slot = equip->inventory_slot;
    uint8_t equip_slot = equip->equip_slot;
    
    ActivePlayer* player = player_acquire(character_id);
    if (!player) {
        printf("Player not found for equip\n");
        return;
    }
    if (!player->is_loaded) { player_release(player); return; }

    // Verify item is in inventory
    if (inventory_slot >= 150 || player->inventory[inventory_slot] != item_id) {
        printf("Item %u not in inventory slot %u\n", item_id, inventory_slot);
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
        printf("Item %u does not exist\n", item_id);
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
        printf("Item %s cannot be equipped in slot %u\n", item->name, equip_slot);
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
    if (!item_can_equip(item_id, player->level, player->player_class, player->player_race)) {
        printf("Player cannot equip item: %s\n", item->name);
        player_release(player);
        
        EquipItemResponsePacket response = {0};
        response.header.type = PACKET_EQUIP_ITEM_RESPONSE;
        response.header.player_id = htonl(character_id);
        response.success = 0;
        strncpy(response.message, "Requirements not met", sizeof(response.message) - 1);
        server_send(client_fd, &response, sizeof(response));
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
            
            // If two-handed, move off-hand back to inventory
            if (item->is_two_handed && player->second_hand != 0) {
                int offhand_slot = -1;
                for (int s = 0; s < 150; s++) {
                    if (player->inventory[s] == 0) {
                        offhand_slot = s;
                        break;
                    }
                }
                if (offhand_slot == -1) {
                    // No room for off-hand — reject equip
                    player_release(player);
                    EquipItemResponsePacket response = {0};
                    response.header.type = PACKET_EQUIP_ITEM_RESPONSE;
                    response.header.player_id = htonl(character_id);
                    response.success = 0;
                    strncpy(response.message, "Inventory full (off-hand)", sizeof(response.message) - 1);
                    server_send(client_fd, &response, sizeof(response));
                    return;
                }
                player->inventory[offhand_slot] = player->second_hand;
                player->second_hand = 0;
            }
            break;
            
        case SLOT_OFF_HAND:
            // Check if main hand is two-handed
            if (player->main_hand != 0) {
                const ItemDefinition* main_hand = item_get(player->main_hand);
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
            
            old_item = player->second_hand;
            player->second_hand = item_id;
            break;
            
        default:
            player_release(player);
            return;
    }
    
    // Put old item back in inventory slot
    player->inventory[inventory_slot] = old_item;
    
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
    response.returned_item = htonl(old_item);
    snprintf(response.message, sizeof(response.message), "Equipped: %s", item->name);

    server_send(client_fd, &response, sizeof(response));

    // Send updated stats to client
    player_send_stats(client_fd, player);

    printf("Character %u equipped %s\n", character_id, item->name);
}

void handle_unequip_item(int client_fd, uint32_t character_id, uint8_t* buffer, ssize_t bytes) {
    if (bytes < (ssize_t)sizeof(UnequipItemPacket)) {
        printf("Invalid unequip packet size\n");
        return;
    }
    
    UnequipItemPacket* unequip = (UnequipItemPacket*)buffer;
    uint8_t equip_slot = unequip->equip_slot;
    
    ActivePlayer* player = player_acquire(character_id);
    if (!player) return;
    if (!player->is_loaded) { player_release(player); return; }

    // Find empty inventory slot
    int inventory_slot = -1;
    for (int i = 0; i < 150; i++) {
        if (player->inventory[i] == 0) {
            inventory_slot = i;
            break;
        }
    }
    
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
    
    uint32_t unequipped_item = 0;
    
    // Unequip the item
    switch (equip_slot) {
        case SLOT_HELMET:
            unequipped_item = player->helmet;
            player->helmet = 0;
            break;
        case SLOT_GLOVES:
            unequipped_item = player->gloves;
            player->gloves = 0;
            break;
        case SLOT_CHEST:
            unequipped_item = player->chest_armor;
            player->chest_armor = 0;
            break;
        case SLOT_LEGGINGS:
            unequipped_item = player->leggings;
            player->leggings = 0;
            break;
        case SLOT_BOOTS:
            unequipped_item = player->boots;
            player->boots = 0;
            break;
        case SLOT_MAIN_HAND:
            unequipped_item = player->main_hand;
            player->main_hand = 0;
            break;
        case SLOT_OFF_HAND:
            unequipped_item = player->second_hand;
            player->second_hand = 0;
            break;
        default:
            player_release(player);
            return;
    }
    
    if (unequipped_item == 0) {
        player_release(player);
        return; // Nothing was equipped
    }
    
    // Add to inventory
    player->inventory[inventory_slot] = unequipped_item;
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

    // Send updated stats to client
    player_send_stats(client_fd, player);

    printf("Character %u unequipped item from slot %u\n", character_id, equip_slot);
}

void handle_use_item(int client_fd, uint32_t character_id, uint8_t* buffer, ssize_t bytes) {
    if (bytes < (ssize_t)sizeof(UseItemPacket)) {
        printf("Invalid use item packet size\n");
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
    if (inventory_slot >= 150 || player->inventory[inventory_slot] == 0) {
        player_release(player);
        response.success = 0;
        strncpy(response.message, "No item in that slot", sizeof(response.message) - 1);
        server_send(client_fd, &response, sizeof(response));
        return;
    }

    uint32_t item_id = player->inventory[inventory_slot];
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
            int32_t missing = player->max_mana - player->mana;
            mp_changed = (item->use_value > missing) ? missing : item->use_value;
            player->mana += mp_changed;
            break;
        }
        case USE_EFFECT_RESTORE_BOTH: {
            int32_t hp_missing = player->max_health - player->health;
            hp_changed = (item->use_value > hp_missing) ? hp_missing : item->use_value;
            player->health += hp_changed;

            int32_t mp_missing = player->max_mana - player->mana;
            mp_changed = (item->use_value > mp_missing) ? mp_missing : item->use_value;
            player->mana += mp_changed;
            break;
        }
        default:
            break;
    }

    // Consume the item
    player->inventory[inventory_slot] = 0;
    player->is_dirty = 1;
    player->last_consumable_time = now;

    // Build response
    response.success = 1;
    response.effect_type = item->use_effect;
    response.health_changed = htonl((uint32_t)hp_changed);
    response.mana_changed = htonl((uint32_t)mp_changed);
    response.new_health = htonl((uint32_t)player->health);
    response.new_mana = htonl((uint32_t)player->mana);
    response.item_id = htonl(item_id);

    player_release(player);

    snprintf(response.message, sizeof(response.message), "Used %s", item->name);
    server_send(client_fd, &response, sizeof(response));
    printf("Character %u used %s (HP+%d, MP+%d)\n",
           character_id, item->name, hp_changed, mp_changed);
}

void handle_drop_item(int client_fd, uint32_t character_id, uint8_t* buffer, ssize_t bytes) {
    if (bytes < (ssize_t)sizeof(DropItemPacket)) {
        printf("Invalid drop packet size\n");
        return;
    }
    
    DropItemPacket* drop = (DropItemPacket*)buffer;
    uint8_t inventory_slot = drop->inventory_slot;
    
    ActivePlayer* player = player_acquire(character_id);
    if (!player) return;
    if (!player->is_loaded) { player_release(player); return; }

    if (inventory_slot >= 150 || player->inventory[inventory_slot] == 0) {
        player_release(player);
        return;
    }

    uint32_t item_id = player->inventory[inventory_slot];
    float drop_x = player->pos_x;
    float drop_y = player->pos_y;
    player->inventory[inventory_slot] = 0;
    player->is_dirty = 1;

    player_release(player);

    // Spawn item on the ground near the player
    loot_drop_item(item_id, 1, drop_x, drop_y, character_id);
    printf("Character %u dropped item %u\n", character_id, item_id);
    
    DropItemResponsePacket response = {0};
    response.header.type = PACKET_DROP_ITEM_RESPONSE;
    response.header.player_id = htonl(character_id);
    response.header.payload_size = htons(sizeof(DropItemResponsePacket) - sizeof(PacketHeader)); 
    response.success = 1;
    response.dropped_item = htonl(item_id);
    server_send(client_fd, &response, sizeof(response));
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
    
    ActivePlayer* player = player_acquire(character_id);
    if (!player) return;
    if (!player->is_loaded) { player_release(player); return; }

    // Swap items
    uint32_t temp = player->inventory[from_slot];
    player->inventory[from_slot] = player->inventory[to_slot];
    player->inventory[to_slot] = temp;
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
    printf("Character %u moved item from slot %u to %u\n", character_id, from_slot, to_slot);
}

#define CHAT_LOCAL_RANGE 800.0f

void handle_chat_send(int client_fd, uint32_t character_id, uint8_t* buffer, ssize_t bytes) {
    (void)client_fd;  // Sender receives via broadcast loop

    if (bytes < (ssize_t)sizeof(ChatSendPacket)) {
        printf("Invalid chat packet size\n");
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

    printf("[CHAT] %s (ch=%u): %s\n", msg.sender_name, channel, msg.message);

    // Broadcast based on channel
    extern ActivePlayer active_players[];
    extern pthread_mutex_t active_players_lock;

    pthread_mutex_lock(&active_players_lock);

    if (channel == CHAT_CHANNEL_WHISPER) {
        // Message format sent by client: "TargetName rest of message"
        // Parse the first word as target name and remainder as the message body.
        const char* space = strchr(msg.message, ' ');
        if (!space || space == msg.message || *(space + 1) == '\0') {
            // Malformed — no target name or no body; silently drop
            pthread_mutex_unlock(&active_players_lock);
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

        printf("[WHISPER] %s -> %s: %s\n", sender_name, target_name, msg.message);

        // Find target and deliver
        int target_fd = -1;
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (!active_players[i].is_loaded) continue;
            pthread_mutex_lock(&active_players[i].lock);
            int match = (strncmp(active_players[i].username, target_name, 31) == 0);
            int fd    = active_players[i].client_fd;
            pthread_mutex_unlock(&active_players[i].lock);
            if (match) {
                server_send(fd, &msg, sizeof(msg));
                target_fd = fd;
                break;
            }
        }

        if (target_fd == -1) {
            printf("[WHISPER] Target '%s' not online\n", target_name);
        } else {
            // Echo back to sender: sender_name field shows "→ TargetName"
            // so the client renders it as "[W] → TargetName: message"
            ChatMessagePacket echo = msg;
            memset(echo.sender_name, 0, sizeof(echo.sender_name));
            snprintf(echo.sender_name, sizeof(echo.sender_name), "-> %.28s", target_name);
            server_send(client_fd, &echo, sizeof(echo));
        }
    } else {
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (!active_players[i].is_loaded) continue;

            if (channel == CHAT_CHANNEL_LOCAL) {
                pthread_mutex_lock(&active_players[i].lock);
                float dx = active_players[i].pos_x - sender_x;
                float dy = active_players[i].pos_y - sender_y;
                float dist_sq = dx * dx + dy * dy;
                int in_range = (dist_sq <= CHAT_LOCAL_RANGE * CHAT_LOCAL_RANGE);
                int fd = active_players[i].client_fd;
                pthread_mutex_unlock(&active_players[i].lock);
                if (in_range) server_send(fd, &msg, sizeof(msg));

            } else if (channel == CHAT_CHANNEL_GLOBAL) {
                pthread_mutex_lock(&active_players[i].lock);
                int fd = active_players[i].client_fd;
                pthread_mutex_unlock(&active_players[i].lock);
                server_send(fd, &msg, sizeof(msg));

            } else if (channel == CHAT_CHANNEL_PARTY) {
                pthread_mutex_lock(&active_players[i].lock);
                uint32_t their_party = active_players[i].party_id;
                int fd = active_players[i].client_fd;
                pthread_mutex_unlock(&active_players[i].lock);
                if (sender_party != 0 && their_party == sender_party)
                    server_send(fd, &msg, sizeof(msg));
            }
        }
    }

    pthread_mutex_unlock(&active_players_lock);
}

// ============================================================================
// PARTY PACKET HANDLERS
// ============================================================================

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
    extern pthread_mutex_t active_players_lock;

    uint32_t target_id = 0;
    pthread_mutex_lock(&active_players_lock);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!active_players[i].is_loaded) continue;
        pthread_mutex_lock(&active_players[i].lock);
        if (strncmp(active_players[i].username, target_name, 31) == 0) {
            target_id = active_players[i].character_id;
            pthread_mutex_unlock(&active_players[i].lock);
            break;
        }
        pthread_mutex_unlock(&active_players[i].lock);
    }
    pthread_mutex_unlock(&active_players_lock);

    if (target_id == 0) {
        printf("[PARTY] Invite failed: player '%s' not found\n", target_name);
        return;
    }

    // Can't invite yourself
    if (target_id == character_id) {
        printf("[PARTY] Invite failed: can't invite yourself\n");
        return;
    }

    // Check if target is already in a party
    Party* target_party = party_find_by_player(target_id);
    if (target_party) {
        printf("[PARTY] Invite failed: '%s' already in a party\n", target_name);
        return;
    }

    if (inviter_party_id != 0) {
        // Already in a party — check if leader and if party is full
        Party* p = party_find(inviter_party_id);
        if (p) {
            pthread_mutex_lock(&p->lock);
            if (p->leader_id != character_id) {
                pthread_mutex_unlock(&p->lock);
                printf("[PARTY] Invite failed: not party leader\n");
                return;
            }
            if (p->member_count >= MAX_PARTY_SIZE) {
                pthread_mutex_unlock(&p->lock);
                printf("[PARTY] Invite failed: party full\n");
                return;
            }
            pthread_mutex_unlock(&p->lock);
        }
    }

    // Create the invite
    if (!party_invite_create(character_id, target_id, inviter_party_id)) {
        printf("[PARTY] Invite failed: target already has pending invite or no slots\n");
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

    printf("[PARTY] Player %u invited '%s' (%u) to party\n", character_id, target_name, target_id);
    (void)client_fd;
}

void handle_party_accept(int client_fd, uint32_t character_id) {
    (void)client_fd;

    PendingInvite* inv = party_invite_find_for_player(character_id);
    if (!inv) {
        printf("[PARTY] Accept failed: no pending invite for player %u\n", character_id);
        return;
    }

    uint32_t from_id = inv->from_id;
    uint32_t existing_party = inv->party_id;

    // Clear the invite
    party_invite_remove(character_id);

    // Check if acceptor is already in a party
    Party* already = party_find_by_player(character_id);
    if (already) {
        printf("[PARTY] Accept failed: player %u already in a party\n", character_id);
        return;
    }

    uint32_t party_id;

    if (existing_party != 0) {
        // Join existing party
        Party* p = party_find(existing_party);
        if (!p) {
            printf("[PARTY] Accept failed: party %u no longer exists\n", existing_party);
            return;
        }
        party_id = existing_party;
    } else {
        // Inviter had no party — create one with inviter as leader
        // But first check inviter hasn't joined a party since
        Party* inviter_party = party_find_by_player(from_id);
        if (inviter_party) {
            party_id = inviter_party->party_id;
        } else {
            party_id = party_create(from_id);
            if (party_id == 0) {
                printf("[PARTY] Accept failed: couldn't create party\n");
                return;
            }
            party_broadcast_update(party_id);
        }
    }

    if (!party_add_member(party_id, character_id)) {
        printf("[PARTY] Accept failed: couldn't add player %u to party %u\n",
               character_id, party_id);
        return;
    }

    printf("[PARTY] Player %u accepted invite, joined party %u\n", character_id, party_id);
    party_broadcast_update(party_id);
}

void handle_party_decline(int client_fd, uint32_t character_id) {
    (void)client_fd;
    party_invite_remove(character_id);
    printf("[PARTY] Player %u declined party invite\n", character_id);
}

void handle_party_leave(int client_fd, uint32_t character_id) {
    (void)client_fd;
    party_remove_member(character_id);
    printf("[PARTY] Player %u left their party\n", character_id);
}

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

    printf("[PARTY] Player %u kicked player %u from party\n", character_id, target_id);
    party_remove_member(target_id);
}
void handle_session_list_request(int client_fd, uint8_t* buffer, ssize_t bytes) {
    extern ActivePlayer active_players[];
    extern pthread_mutex_t active_players_lock;

    if (bytes < (ssize_t)sizeof(SessionListRequestPacket)) return;

    SessionListRequestPacket* req = (SessionListRequestPacket*)buffer;
    uint16_t requested_page = ntohs(req->page);

    // Snapshot counts and build entry list under lock
    // We collect up to (requested_page+1)*SESSION_LIST_PAGE_SIZE entries
    // then slice the right page out.
    // For simplicity with potentially thousands of players, we collect all
    // loaded player ids/data, sort by character_id, then page.

    // Step 1: collect all online players
    typedef struct {
        uint32_t character_id;
        char     name[32];
        int      level;
        uint8_t  player_class;
        uint8_t  player_race;
        uint16_t ping_ms;
    } Snap;

    Snap* snaps = NULL;
    int   snap_count = 0;

    pthread_mutex_lock(&active_players_lock);
    // Count first
    for (int i = 0; i < MAX_PLAYERS; i++) {
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
            for (int i = 0; i < MAX_PLAYERS && idx < snap_count; i++) {
                ActivePlayer* ap = &active_players[i];
                if (!ap->is_loaded || !ap->is_ready) continue;
                // Hold per-player lock while copying to avoid data races with
                // threads that already hold ap->lock via player_acquire.
                pthread_mutex_lock(&ap->lock);
                snaps[idx].character_id = ap->character_id;
                strncpy(snaps[idx].name, ap->username, 31);
                snaps[idx].name[31]     = '\0';
                snaps[idx].level        = ap->level;
                snaps[idx].player_class = (uint8_t)ap->player_class;
                snaps[idx].player_race  = (uint8_t)ap->player_race;
                snaps[idx].ping_ms      = ap->ping_ms;
                pthread_mutex_unlock(&ap->lock);
                idx++;
            }
            snap_count = idx;
        }
    }
    pthread_mutex_unlock(&active_players_lock);

    // Step 2: compute pagination
    uint32_t total_players = (uint32_t)snap_count;
    uint16_t total_pages   = (uint16_t)((snap_count + SESSION_LIST_PAGE_SIZE - 1) / SESSION_LIST_PAGE_SIZE);
    if (total_pages == 0) total_pages = 1;
    if (requested_page >= total_pages) requested_page = total_pages - 1;

    int page_start = (int)requested_page * SESSION_LIST_PAGE_SIZE;
    int page_count = snap_count - page_start;
    if (page_count > SESSION_LIST_PAGE_SIZE) page_count = SESSION_LIST_PAGE_SIZE;
    if (page_count < 0) page_count = 0;

    // Step 3: build and send response
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
        e->player_class = s->player_class;
        e->player_race  = s->player_race;
        e->ping_ms      = htons(s->ping_ms);
    }

    free(snaps);

    size_t send_size = offsetof(SessionListResponsePacket, entries) +
                       (size_t)page_count * sizeof(SessionPlayerEntry);
    server_send(client_fd, &resp, send_size);
    printf("[SESSION] Sent page %u/%u (%d entries, %u total) to fd=%d\n",
           requested_page + 1, total_pages, page_count, total_players, client_fd);
}
