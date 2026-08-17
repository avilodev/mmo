/**
 * @file
 * Exercise registration, realm selection, character entry, and world connectivity end to end.
 */

#include "types.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <time.h>

/**
 * Connect a TCP socket to an IPv4 server.
 *
 * @param host  Numeric IPv4 address.
 * @param port  TCP port in host byte order.
 * @return      Connected descriptor, or -1 on socket or connection failure.
 */
int connect_to_server(const char* host, int port) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return -1;
    }
    
    struct sockaddr_in server_addr = {0};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    inet_pton(AF_INET, host, &server_addr.sin_addr);
    
    if (connect(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect");
        close(sockfd);
        return -1;
    }
    
    return sockfd;
}

/**
 * Run the interactive end-to-end server connection test.
 *
 * @param argc  Command-line argument count.
 * @param argv  Command-line arguments; argv[1] optionally supplies the username.
 * @return      Zero on completion, or one when a required protocol step fails.
 */
int main(int argc, char** argv) {
    char username[32];
    char password[64] = "TestPass123";
    
    if (argc > 1) {
        strncpy(username, argv[1], 31);
    } else {
        snprintf(username, sizeof(username), "TestUser%ld", time(NULL) % 10000);
    }
    
    printf("=== MMO CLIENT TEST ===\n");
    printf("User: %s\n\n", username);
    
    printf("[1/5] Connecting to login server...\n");
    int login_fd = connect_to_server("127.0.0.1", LOGIN_SERVER_PORT);
    if (login_fd < 0) return 1;
    
    // Try register first
    AuthLoginPacket reg_pkt = {0};
    reg_pkt.header.type = PACKET_AUTH_REGISTER;
    strncpy(reg_pkt.username, username, 31);
    strncpy(reg_pkt.password, password, 63);
    send(login_fd, &reg_pkt, sizeof(reg_pkt), 0);
    
    AuthLoginResponsePacket reg_resp;
    recv(login_fd, &reg_resp, sizeof(reg_resp), 0);
    close(login_fd);
    
    // Now login
    login_fd = connect_to_server("127.0.0.1", LOGIN_SERVER_PORT);
    if (login_fd < 0) return 1;
    
    AuthLoginPacket login_pkt = {0};
    login_pkt.header.type = PACKET_AUTH_LOGIN;
    strncpy(login_pkt.username, username, 31);
    strncpy(login_pkt.password, password, 63);
    send(login_fd, &login_pkt, sizeof(login_pkt), 0);
    
    AuthLoginResponsePacket login_resp;
    if (recv(login_fd, &login_resp, sizeof(login_resp), 0) <= 0 || !login_resp.success) {
        printf("❌ Login failed\n");
        close(login_fd);
        return 1;
    }
    
    uint32_t player_id = ntohl(login_resp.assigned_player_id);
    char session_key[32];
    memcpy(session_key, login_resp.header.session_key, 32);
    
    printf("✅ Logged in! Player ID: %u\n\n", player_id);
    close(login_fd);
    
    printf("[2/5] Connecting to realm server...\n");
    int realm_fd = connect_to_server("127.0.0.1", REALM_SERVER_PORT);
    if (realm_fd < 0) return 1;
    
    RealmConnectPacket realm_conn = {0};
    realm_conn.header.type = PACKET_REALM_CONNECT;
    realm_conn.header.player_id = htonl(player_id);
    memcpy(realm_conn.header.session_key, session_key, 32);
    
    send(realm_fd, &realm_conn, sizeof(realm_conn), 0);
    
    RealmConnectAckPacket realm_ack;
    if (recv(realm_fd, &realm_ack, sizeof(realm_ack), 0) <= 0 || !realm_ack.success) {
        printf("❌ Realm auth failed\n");
        close(realm_fd);
        return 1;
    }
    
    printf("✅ Realm authenticated\n\n");
    
    printf("[3/5] Requesting world list...\n");
    
    WorldListRequestPacket world_req = {0};
    world_req.header.type = PACKET_WORLD_LIST_REQUEST;
    world_req.header.player_id = htonl(player_id);
    
    send(realm_fd, &world_req, sizeof(world_req), 0);
    
    WorldListResponsePacket world_list;
    if (recv(realm_fd, &world_list, sizeof(world_list), 0) <= 0) {
        printf("❌ Failed to get world list\n");
        close(realm_fd);
        return 1;
    }
    
    printf("✅ Received %d worlds:\n", world_list.count);
    for (int i = 0; i < world_list.count; i++) {
        printf("   [%d] %s (%s) - %s\n", 
               ntohl(world_list.worlds[i].world_id),
               world_list.worlds[i].name,
               world_list.worlds[i].region,
               world_list.worlds[i].status == 1 ? "Online" : "Offline");
    }
    printf("\n");
    
    // Pick first online world
    uint32_t selected_world = 0;
    char world_ip[16] = {0};
    uint16_t world_port = 0;
    
    for (int i = 0; i < world_list.count; i++) {
        if (world_list.worlds[i].status == 1) {
            selected_world = ntohl(world_list.worlds[i].world_id);
            strncpy(world_ip, world_list.worlds[i].ip, 15);
            world_port = ntohs(world_list.worlds[i].port);
            printf("Selected world: [%d] %s\n\n", selected_world, world_list.worlds[i].name);
            break;
        }
    }
    
    if (selected_world == 0) {
        printf("❌ No online worlds available\n");
        close(realm_fd);
        return 1;
    }
    
    printf("[4/5] Checking characters...\n");
    
    CharacterListRequestPacket char_req = {0};
    char_req.header.type = PACKET_CHARACTER_LIST_REQUEST;
    char_req.header.player_id = htonl(player_id);
    char_req.world_id = htonl(selected_world);
    
    send(realm_fd, &char_req, sizeof(char_req), 0);
    
    CharacterListResponsePacket char_list;
    if (recv(realm_fd, &char_list, sizeof(char_list), 0) <= 0) {
        printf("❌ Failed to get character list\n");
        close(realm_fd);
        return 1;
    }
    
    uint32_t character_id = 0;
    
    if (char_list.count > 0) {
        character_id = ntohl(char_list.characters[0].character_id);
        printf("✅ Using existing character: %s (ID: %u)\n\n", 
               char_list.characters[0].name, character_id);
    } else {
        printf("Creating new character...\n");
        
        CharacterCreateRequestPacket create_req = {0};
        create_req.header.type = PACKET_CHARACTER_CREATE_REQUEST;
        create_req.header.player_id = htonl(player_id);
        create_req.world_id = htonl(selected_world);
        snprintf(create_req.name, 31, "Hero%u", player_id);
        create_req.class_id = htonl(1);
        create_req.race_id = htonl(1);
        
        send(realm_fd, &create_req, sizeof(create_req), 0);
        
        CharacterCreateResponsePacket create_resp;
        if (recv(realm_fd, &create_resp, sizeof(create_resp), 0) <= 0 || !create_resp.success) {
            printf("❌ Character creation failed\n");
            close(realm_fd);
            return 1;
        }
        
        character_id = ntohl(create_resp.character_id);
        printf("✅ Created character: %s (ID: %u)\n\n", create_resp.character_name, character_id);
        
        // Receive updated character list
        recv(realm_fd, &char_list, sizeof(char_list), 0);
    }
    
    printf("[5/5] Entering world...\n");
    
    EnterWorldPacket enter_req = {0};
    enter_req.header.type = PACKET_ENTER_WORLD;
    enter_req.header.player_id = htonl(player_id);
    enter_req.character_id = htonl(character_id);
    enter_req.world_id = htonl(selected_world);
    
    send(realm_fd, &enter_req, sizeof(enter_req), 0);
    
    EnterWorldResponsePacket enter_resp;
    if (recv(realm_fd, &enter_resp, sizeof(enter_resp), 0) <= 0 || !enter_resp.success) {
        printf("❌ Failed to enter world\n");
        close(realm_fd);
        return 1;
    }
    
    printf("✅ Got game ticket for %s:%u\n\n", 
           enter_resp.world_ip, ntohs(enter_resp.world_port));
    
    close(realm_fd);
    
    printf("Connecting to world server...\n");
    int world_fd = connect_to_server(enter_resp.world_ip, ntohs(enter_resp.world_port));
    if (world_fd < 0) return 1;
    
    WorldConnectPacket world_conn = {0};
    world_conn.header.type = PACKET_WORLD_CONNECT;
    world_conn.header.player_id = htonl(character_id);
    strncpy(world_conn.game_ticket, enter_resp.game_ticket, 63);
    world_conn.character_id = htonl(character_id);
    
    send(world_fd, &world_conn, sizeof(world_conn), 0);
    
    WorldConnectAckPacket world_ack;
    if (recv(world_fd, &world_ack, sizeof(world_ack), 0) <= 0 || !world_ack.success) {
        printf("❌ World auth failed\n");
        close(world_fd);
        return 1;
    }
    
    printf("✅ Connected to world server!\n");
    printf("   Message: %s\n\n", world_ack.welcome_message);
    
    printf("╔═══════════════════════════════════════════════════╗\n");
    printf("║     SUCCESSFULLY CONNECTED TO WORLD SERVER        ║\n");
    printf("╚═══════════════════════════════════════════════════╝\n\n");
    printf("Staying connected for 10 seconds...\n");
    
    for (int i = 0; i < 10; i++) {
        sleep(1);
        
        PacketHeader ping = {0};
        ping.type = PACKET_PING;
        ping.player_id = htonl(character_id);
        
        if (send(world_fd, &ping, sizeof(ping), 0) <= 0) {
            printf("❌ Connection lost\n");
            break;
        }
        printf(".");
        fflush(stdout);
    }
    
    printf("\n\n✅ Test completed successfully!\n");
    close(world_fd);
    
    return 0;
}
