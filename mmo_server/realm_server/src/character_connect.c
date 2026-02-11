
#include "types.h"
#include "players_database.h"
#include "character_connect.h"
#include "world_database_manager.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <sys/socket.h>

extern PGconn* g_pg;  

#define MAX_CHARACTERS_PER_WORLD 10

void handle_character_list_request(int client_fd, uint32_t account_id, uint32_t world_id) {
    CharacterInfo characters[MAX_CHARACTERS_PER_WORLD];
    
    int count = world_character_get_list(account_id, world_id, 
                                              characters, MAX_CHARACTERS_PER_WORLD);
    
    // Build response packet
    CharacterListResponsePacket response = {0};
    response.header.type = PACKET_CHARACTER_LIST_RESPONSE;
    response.header.player_id = htonl(account_id);
    response.world_id = htonl(world_id);
    response.count = count;
    
    for (int i = 0; i < count; i++) {
        response.characters[i].character_id = htonl(characters[i].character_id);
        strncpy(response.characters[i].name, characters[i].name, 31);
        response.characters[i].name[31] = '\0';
        response.characters[i].level = htonl(characters[i].level);
        response.characters[i].class_id = htonl(characters[i].player_class);
        response.characters[i].race_id = htonl(characters[i].player_race);
    }
    
    send(client_fd, &response, sizeof(response), 0);
    printf("Sent %d characters to account %u for world %u\n", 
           count, account_id, world_id);
}

void handle_character_create_request(int client_fd, uint32_t account_id, 
                                      uint32_t world_id, const char* name, 
                                      int class_id, int race_id) {
    CharacterCreateResponsePacket response = {0};
    response.header.type = PACKET_CHARACTER_CREATE_RESPONSE;
    response.header.player_id = htonl(account_id);
    response.world_id = htonl(world_id);
    
    // Check character limit per world
    int current_count = character_count_in_world(account_id, world_id);
    if (current_count >= MAX_CHARACTERS_PER_WORLD) {
        response.success = 0;
        strncpy(response.message, "Character limit reached for this world", 127);
        send(client_fd, &response, sizeof(response), 0);
        return;
    }
    
    // Validate name (basic validation)
    size_t name_len = strlen(name);
    if (name_len < 3 || name_len > 31) {
        response.success = 0;
        strncpy(response.message, "Name must be 3-31 characters", 127);
        send(client_fd, &response, sizeof(response), 0);
        return;
    }
    
    // Check for valid characters in name (letters only)
    for (size_t i = 0; i < name_len; i++) {
        if (!((name[i] >= 'a' && name[i] <= 'z') || 
              (name[i] >= 'A' && name[i] <= 'Z'))) {
            response.success = 0;
            strncpy(response.message, "Name must contain only letters", 127);
            send(client_fd, &response, sizeof(response), 0);
            return;
        }
    }
    
    // Create character
    uint32_t character_id;
    int success = world_character_create(account_id, world_id, name, 
                                            class_id, race_id, &character_id);
    
    if (success) {
        response.success = 1;
        response.character_id = htonl(character_id);
        strncpy(response.character_name, name, 31);
        strncpy(response.message, "Character created successfully", 127);
        printf("Created character '%s' (ID:%u) for account %u in world %u\n",
               name, character_id, account_id, world_id);
    } else {
        response.success = 0;
        strncpy(response.message, "Character creation failed (name may be taken)", 127);
    }
    
    send(client_fd, &response, sizeof(response), 0);
    
    // If successful, send updated character list
    if (success) {
        handle_character_list_request(client_fd, account_id, world_id);
    }
}

void handle_character_delete_request(int client_fd, uint32_t account_id, 
                                      uint32_t character_id, uint32_t world_id) {
    CharacterDeleteResponsePacket response = {0};
    response.header.type = PACKET_CHARACTER_DELETE_RESPONSE;
    response.header.player_id = htonl(account_id);
    response.character_id = htonl(character_id);
    response.world_id = htonl(world_id);
    
    int success = world_character_delete(account_id, character_id, world_id);
    
    if (success) {
        response.success = 1;
        strncpy(response.message, "Character deleted", 127);
        printf("Deleted character %u for account %u from world %u\n",
               character_id, account_id, world_id);
    } else {
        response.success = 0;
        strncpy(response.message, "Character deletion failed", 127);
    }
    
    send(client_fd, &response, sizeof(response), 0);
    
    // If successful, send updated character list
    if (success) {
        handle_character_list_request(client_fd, account_id, world_id);
    }
}