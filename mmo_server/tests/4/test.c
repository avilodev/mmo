#include "types.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <time.h>
#include <sys/time.h>
#include <errno.h>
#include <pthread.h>

// Equipment slot definitions (from items_database.h)
typedef enum {
    SLOT_NONE = 0,
    SLOT_HELMET = 1,
    SLOT_GLOVES = 2,
    SLOT_CHEST = 3,
    SLOT_LEGGINGS = 4,
    SLOT_BOOTS = 5,
    SLOT_MAIN_HAND = 6,
    SLOT_OFF_HAND = 7,
    SLOT_TWO_HANDED = 8
} EquipSlot;

// Color codes for better output
#define GREEN "\033[0;32m"
#define RED "\033[0;31m"
#define YELLOW "\033[0;33m"
#define BLUE "\033[0;34m"
#define CYAN "\033[0;36m"
#define MAGENTA "\033[0;35m"
#define RESET "\033[0m"
#define BOLD "\033[1m"

// Test statistics
typedef struct {
    int total_steps;
    int passed_steps;
    int failed_steps;
    struct timeval start_time;
    struct timeval end_time;
    pthread_mutex_t lock;
} TestStats;

TestStats g_stats = {0};

// Thread-safe stats updates
void stats_init(void) {
    memset(&g_stats, 0, sizeof(g_stats));
    pthread_mutex_init(&g_stats.lock, NULL);
    gettimeofday(&g_stats.start_time, NULL);
}

void stats_finish(void) {
    gettimeofday(&g_stats.end_time, NULL);
}

void stats_inc_passed(void) {
    pthread_mutex_lock(&g_stats.lock);
    g_stats.passed_steps++;
    pthread_mutex_unlock(&g_stats.lock);
}

void stats_inc_failed(void) {
    pthread_mutex_lock(&g_stats.lock);
    g_stats.failed_steps++;
    pthread_mutex_unlock(&g_stats.lock);
}

void stats_inc_total(void) {
    pthread_mutex_lock(&g_stats.lock);
    g_stats.total_steps++;
    pthread_mutex_unlock(&g_stats.lock);
}

double stats_elapsed_time(void) {
    return (g_stats.end_time.tv_sec - g_stats.start_time.tv_sec) + 
           (g_stats.end_time.tv_usec - g_stats.start_time.tv_usec) / 1000000.0;
}

void print_header(const char* title) {
    printf("\n" BLUE BOLD "╔═══════════════════════════════════════════════════════════════╗\n");
    printf("║ %-61s ║\n", title);
    printf("╚═══════════════════════════════════════════════════════════════╝" RESET "\n\n");
}

void print_step(int step, int total, const char* description) {
    stats_inc_total();
    printf(CYAN "[Step %d/%d] %s" RESET "\n", step, total, description);
}

void print_success(const char* msg) {
    stats_inc_passed();
    printf(GREEN "  ✓ SUCCESS: %s" RESET "\n", msg);
}

void print_error(const char* msg) {
    stats_inc_failed();
    printf(RED "  ✗ FAILED: %s" RESET "\n", msg);
}

void print_info(const char* msg) {
    printf(YELLOW "  → %s" RESET "\n", msg);
}

void print_detail(const char* key, const char* value) {
    printf("     %s: " BOLD "%s" RESET "\n", key, value);
}

void print_detail_int(const char* key, int value) {
    printf("     %s: " BOLD "%d" RESET "\n", key, value);
}

void print_detail_uint(const char* key, uint32_t value) {
    printf("     %s: " BOLD "%u" RESET "\n", key, value);
}

int connect_to_server(const char* host, int port, const char* server_name) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        return -1;
    }
    
    struct sockaddr_in server_addr = {0};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, host, &server_addr.sin_addr) <= 0) {
        close(sockfd);
        return -1;
    }
    
    if (connect(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        close(sockfd);
        return -1;
    }
    
    return sockfd;
}

ssize_t safe_recv(int sockfd, void* buf, size_t len, const char* context) {
    ssize_t bytes = recv(sockfd, buf, len, 0);
    return bytes;
}

// Helper to get class name
const char* get_class_name(uint32_t class_id) {
    switch (class_id) {
        case 1: return "Gladiator";
        case 2: return "Ninja";
        case 3: return "Landweaver";
        case 4: return "Spirit";
        default: return "Unknown";
    }
}

// Helper to get race name
const char* get_race_name(uint32_t race_id) {
    switch (race_id) {
        case 1: return "Human";
        case 2: return "Pyseck";
        case 3: return "Infor";
        default: return "Unknown";
    }
}

int main(int argc, char** argv) {
    stats_init();
    
    char username[32] = "Panda";
    char password[64] = "Panda123";
    uint32_t selected_class = 0;
    uint32_t selected_race = 0;
    
    // Parse command line arguments
    if (argc > 1) {
        strncpy(username, argv[1], 31);
        username[31] = '\0';
    } else {
        //snprintf(username, sizeof(username), "ItemTest%d_%ld", getpid(), time(NULL) % 10000);
    }
    
    if (argc > 2) {
        strncpy(password, argv[2], 63);
        password[63] = '\0';
    }
    
    if (argc > 3) {
        selected_class = atoi(argv[3]);
        if (selected_class < 1 || selected_class > 4) {
            printf("Invalid class ID. Using default (1).\n");
            selected_class = 1;
        }
    }
    
    if (argc > 4) {
        selected_race = atoi(argv[4]);
        if (selected_race < 1 || selected_race > 3) {
            printf("Invalid race ID. Using default (1).\n");
            selected_race = 1;
        }
    }
    
    print_header("MMO ITEM SYSTEM INTEGRATION TEST");
    printf("Test User: " BOLD "%s" RESET "\n", username);
    printf("Password: " BOLD "%s" RESET "\n", password);
    printf("Timestamp: " BOLD "%ld" RESET "\n", time(NULL));
    
    // ==========================================
    // STEP 1: REGISTER & LOGIN
    // ==========================================
    print_step(1, 10, "Authentication (Register + Login)");
    
    print_info("Attempting registration...");
    int login_fd = connect_to_server("127.0.0.1", LOGIN_SERVER_PORT, "Login Server");
    if (login_fd < 0) {
        print_error("Failed to connect to login server");
        stats_finish();
        return 1;
    }
    print_success("Connected to Login Server");
    
    AuthLoginPacket reg_pkt = {0};
    reg_pkt.header.type = PACKET_AUTH_REGISTER;
    strncpy(reg_pkt.username, username, 31);
    strncpy(reg_pkt.password, password, 63);
    
    if (send(login_fd, &reg_pkt, sizeof(reg_pkt), 0) <= 0) {
        print_error("Failed to send registration packet");
        close(login_fd);
        stats_finish();
        return 1;
    }
    
    AuthLoginResponsePacket reg_resp;
    ssize_t bytes = safe_recv(login_fd, &reg_resp, sizeof(reg_resp), "registration");
    if (bytes > 0) {
        if (reg_resp.success) {
            print_success("Registration successful");
        } else {
            print_info("Registration failed (user may already exist)");
            print_detail("Error", reg_resp.error_message);
        }
    }
    close(login_fd);
    
    // Now login
    print_info("Attempting login...");
    login_fd = connect_to_server("127.0.0.1", LOGIN_SERVER_PORT, "Login Server");
    if (login_fd < 0) {
        print_error("Failed to connect to login server");
        stats_finish();
        return 1;
    }
    
    AuthLoginPacket login_pkt = {0};
    login_pkt.header.type = PACKET_AUTH_LOGIN;
    strncpy(login_pkt.username, username, 31);
    strncpy(login_pkt.password, password, 63);
    
    if (send(login_fd, &login_pkt, sizeof(login_pkt), 0) <= 0) {
        print_error("Failed to send login packet");
        close(login_fd);
        stats_finish();
        return 1;
    }
    
    AuthLoginResponsePacket login_resp;
    bytes = safe_recv(login_fd, &login_resp, sizeof(login_resp), "login");
    
    if (bytes <= 0 || !login_resp.success) {
        print_error("Login failed");
        if (bytes > 0) {
            print_detail("Error", login_resp.error_message);
        }
        close(login_fd);
        stats_finish();
        return 1;
    }
    
    uint32_t player_id = ntohl(login_resp.assigned_player_id);
    char session_key[32];
    memcpy(session_key, login_resp.header.session_key, 32);
    
    print_success("Login successful");
    print_detail_uint("Player ID", player_id);
    
    close(login_fd);
    sleep(1);
    
    // ==========================================
    // STEP 2: CONNECT TO REALM SERVER
    // ==========================================
    print_step(2, 10, "Realm Server Connection");
    
    int realm_fd = connect_to_server("127.0.0.1", REALM_SERVER_PORT, "Realm Server");
    if (realm_fd < 0) {
        print_error("Failed to connect to realm server");
        stats_finish();
        return 1;
    }
    print_success("Connected to Realm Server");
    
    RealmConnectPacket realm_conn = {0};
    realm_conn.header.type = PACKET_REALM_CONNECT;
    realm_conn.header.player_id = htonl(player_id);
    memcpy(realm_conn.header.session_key, session_key, 32);
    
    if (send(realm_fd, &realm_conn, sizeof(realm_conn), 0) <= 0) {
        print_error("Failed to send realm connect packet");
        close(realm_fd);
        stats_finish();
        return 1;
    }
    
    RealmConnectAckPacket realm_ack;
    bytes = safe_recv(realm_fd, &realm_ack, sizeof(realm_ack), "realm authentication");
    
    if (bytes <= 0 || !realm_ack.success) {
        print_error("Realm authentication failed");
        close(realm_fd);
        stats_finish();
        return 1;
    }
    
    print_success("Realm server authenticated");
    sleep(1);
    
    // ==========================================
    // STEP 3: GET WORLD LIST
    // ==========================================
    print_step(3, 10, "World List Retrieval");
    
    WorldListRequestPacket world_req = {0};
    world_req.header.type = PACKET_WORLD_LIST_REQUEST;
    world_req.header.player_id = htonl(player_id);
    
    if (send(realm_fd, &world_req, sizeof(world_req), 0) <= 0) {
        print_error("Failed to send world list request");
        close(realm_fd);
        stats_finish();
        return 1;
    }
    
    WorldListResponsePacket world_list;
    bytes = safe_recv(realm_fd, &world_list, sizeof(world_list), "world list");
    
    if (bytes <= 0) {
        print_error("Failed to receive world list");
        close(realm_fd);
        stats_finish();
        return 1;
    }
    
    print_success("World list received");
    print_detail_int("World Count", world_list.count);
    
    // Select first online world
    uint32_t selected_world = 0;
    char selected_world_name[64] = {0};
    
    for (int i = 0; i < world_list.count; i++) {
        if (world_list.worlds[i].status == 1) {
            selected_world = ntohl(world_list.worlds[i].world_id);
            strncpy(selected_world_name, world_list.worlds[i].name, 63);
            print_info("Selected world");
            print_detail("World Name", selected_world_name);
            print_detail_uint("World ID", selected_world);
            break;
        }
    }
    
    if (selected_world == 0) {
        print_error("No online worlds available");
        close(realm_fd);
        stats_finish();
        return 1;
    }
    
    sleep(1);
    
    // ==========================================
    // STEP 4: CHARACTER CUSTOMIZATION
    // ==========================================
    print_step(4, 10, "Character Customization");
    
    // If class/race not specified via command line, prompt user or use defaults
    if (selected_class == 0) {
        printf("\n" BOLD "Available Classes:" RESET "\n");
        printf("  1. Gladiator (Heavy melee, high defense)\n");
        printf("  2. Ninja (Fast melee, high agility)\n");
        printf("  3. Landweaver (Magic melee hybrid)\n");
        printf("  4. Spirit (Magic ranged, high intelligence)\n");
        
        selected_class = 1; // Default to Gladiator for automated testing
        print_info("Auto-selecting Gladiator for testing");
    }
    
    if (selected_race == 0) {
        printf("\n" BOLD "Available Races:" RESET "\n");
        printf("  1. Human (Balanced)\n");
        printf("  2. Pyseck (Agile)\n");
        printf("  3. Infor (Intelligent)\n");
        
        selected_race = 1; // Default to Human for automated testing
        print_info("Auto-selecting Human for testing");
    }
    
    print_success("Character customization complete");
    print_detail("Class", get_class_name(selected_class));
    print_detail("Race", get_race_name(selected_race));
    
    sleep(1);
    
    // ==========================================
    // STEP 5: CHARACTER CREATION
    // ==========================================
    print_step(5, 10, "Character Creation with Customization");
    
    print_info("Checking for existing characters...");
    CharacterListRequestPacket char_req = {0};
    char_req.header.type = PACKET_CHARACTER_LIST_REQUEST;
    char_req.header.player_id = htonl(player_id);
    char_req.world_id = htonl(selected_world);
    
    if (send(realm_fd, &char_req, sizeof(char_req), 0) <= 0) {
        print_error("Failed to send character list request");
        close(realm_fd);
        stats_finish();
        return 1;
    }
    
    CharacterListResponsePacket char_list;
    bytes = safe_recv(realm_fd, &char_list, sizeof(char_list), "character list");
    
    if (bytes <= 0) {
        print_error("Failed to receive character list");
        close(realm_fd);
        stats_finish();
        return 1;
    }
    
    uint32_t character_id = 0;
    char character_name[32] = {0};
    
    // Always create a new character for this test
    print_info("Creating new character with custom class and race...");
    
    CharacterCreateRequestPacket create_req = {0};
    create_req.header.type = PACKET_CHARACTER_CREATE_REQUEST;
    create_req.header.player_id = htonl(player_id);
    create_req.world_id = htonl(selected_world);
    
    snprintf(create_req.name, 31, "Panda", 
             get_class_name(selected_class), 
             get_race_name(selected_race));
    
    create_req.class_id = htonl(selected_class);
    create_req.race_id = htonl(selected_race);
    
    print_detail("Character Name", create_req.name);
    print_detail("Class", get_class_name(selected_class));
    print_detail("Race", get_race_name(selected_race));
    
    if (send(realm_fd, &create_req, sizeof(create_req), 0) <= 0) {
        print_error("Failed to send character creation request");
        close(realm_fd);
        stats_finish();
        return 1;
    }
    
    CharacterCreateResponsePacket create_resp;
    bytes = safe_recv(realm_fd, &create_resp, sizeof(create_resp), "character creation");
    
    if (bytes <= 0 || !create_resp.success) {
        print_error("Character creation failed");
        if (bytes > 0) {
            print_detail("Server Message", create_resp.message);
        }
        close(realm_fd);
        stats_finish();
        return 1;
    }
    
    character_id = ntohl(create_resp.character_id);
    strncpy(character_name, create_resp.character_name, 31);
    
    print_success("Character created successfully");
    print_detail("Character Name", character_name);
    print_detail_uint("Character ID", character_id);
    
    // IMPORTANT: Realm server automatically sends updated character list after creation
    // We need to consume this packet before moving to the next step
    print_info("Receiving updated character list...");
    CharacterListResponsePacket updated_list;
    bytes = safe_recv(realm_fd, &updated_list, sizeof(updated_list), "updated character list");
    if (bytes > 0) {
        print_detail_int("Characters in list", updated_list.count);
    }
    
    sleep(1);
    
    // ==========================================
    // STEP 6: ENTER WORLD
    // ==========================================
    print_step(6, 10, "Enter World (Ticket Generation)");
    
    EnterWorldPacket enter_req = {0};
    enter_req.header.type = PACKET_ENTER_WORLD;
    enter_req.header.player_id = htonl(player_id);
    enter_req.character_id = htonl(character_id);
    enter_req.world_id = htonl(selected_world);
    
    if (send(realm_fd, &enter_req, sizeof(enter_req), 0) <= 0) {
        print_error("Failed to send enter world request");
        close(realm_fd);
        stats_finish();
        return 1;
    }
    
    EnterWorldResponsePacket enter_resp;
    memset(&enter_resp, 0, sizeof(enter_resp));
    bytes = safe_recv(realm_fd, &enter_resp, sizeof(enter_resp), "enter world");
    
    printf("     DEBUG: Received %zd bytes\n", bytes);
    if (bytes > 0) {
        printf("     DEBUG: Packet type: %u\n", enter_resp.header.type);
        printf("     DEBUG: Success flag: %u\n", enter_resp.success);
        printf("     DEBUG: Message: '%s'\n", enter_resp.message);
        printf("     DEBUG: Ticket: '%s'\n", enter_resp.game_ticket);
        printf("     DEBUG: World IP: '%s'\n", enter_resp.world_ip);
        printf("     DEBUG: World Port: %u\n", ntohs(enter_resp.world_port));
    }
    
    if (bytes <= 0 || !enter_resp.success) {
        print_error("Failed to enter world");
        if (bytes > 0) {
            printf("     Server message: %s\n", enter_resp.message);
        }
        close(realm_fd);
        stats_finish();
        return 1;
    }
    
    print_success("Game ticket received");
    print_detail("World IP", enter_resp.world_ip);
    print_detail_int("World Port", ntohs(enter_resp.world_port));
    
    close(realm_fd);
    sleep(1);
    
    // ==========================================
    // STEP 7: CONNECT TO WORLD SERVER
    // ==========================================
    print_step(7, 10, "World Server Connection");
    
    int world_fd = connect_to_server(enter_resp.world_ip, ntohs(enter_resp.world_port), "World Server");
    if (world_fd < 0) {
        print_error("Failed to connect to world server");
        stats_finish();
        return 1;
    }
    print_success("Connected to World Server");
    
    WorldConnectPacket world_conn = {0};
    world_conn.header.type = PACKET_WORLD_CONNECT;
    world_conn.header.player_id = htonl(character_id);
    strncpy(world_conn.game_ticket, enter_resp.game_ticket, 63);
    world_conn.character_id = htonl(character_id);
    
    if (send(world_fd, &world_conn, sizeof(world_conn), 0) <= 0) {
        print_error("Failed to send world connect packet");
        close(world_fd);
        stats_finish();
        return 1;
    }
    
    WorldConnectAckPacket world_ack;
    bytes = safe_recv(world_fd, &world_ack, sizeof(world_ack), "world authentication");
    
    if (bytes <= 0 || !world_ack.success) {
        print_error("World server authentication failed");
        close(world_fd);
        stats_finish();
        return 1;
    }
    
    print_success("Connected to world server");
    sleep(1);
    
    // ==========================================
    // STEP 8: REQUEST PLAYER DATA
    // ==========================================
    print_step(8, 10, "Request Player Data (Items & Equipment)");
    
    RequestPlayerDataPacket data_req = {0};
    data_req.header.type = PACKET_REQUEST_PLAYER_DATA;
    data_req.header.player_id = htonl(character_id);
    
    if (send(world_fd, &data_req, sizeof(data_req), 0) <= 0) {
        print_error("Failed to send player data request");
        close(world_fd);
        stats_finish();
        return 1;
    }
    
    PlayerDataPacket player_data;
    bytes = safe_recv(world_fd, &player_data, sizeof(player_data), "player data");
    
    if (bytes <= 0) {
        print_error("Failed to receive player data");
        close(world_fd);
        stats_finish();
        return 1;
    }
    
    print_success("Player data received");
    print_detail_uint("Level", ntohl(player_data.level));
    print_detail_uint("Health", ntohl(player_data.health));
    print_detail_uint("Gold", ntohl(player_data.gold));
    
    // Display current equipment
    printf("\n" BOLD "     Current Equipment:" RESET "\n");
    printf("       Helmet:     %u (durability: %u)\n", ntohl(player_data.helmet), ntohl(player_data.helmet_durability));
    printf("       Chest:      %u (durability: %u)\n", ntohl(player_data.chest_armor), ntohl(player_data.chest_durability));
    printf("       Gloves:     %u (durability: %u)\n", ntohl(player_data.gloves), ntohl(player_data.gloves_durability));
    printf("       Leggings:   %u (durability: %u)\n", ntohl(player_data.leggings), ntohl(player_data.leggings_durability));
    printf("       Boots:      %u (durability: %u)\n", ntohl(player_data.boots), ntohl(player_data.boots_durability));
    printf("       Main Hand:  %u (durability: %u)\n", ntohl(player_data.main_hand), ntohl(player_data.main_hand_durability));
    printf("       Off Hand:   %u (durability: %u)\n", ntohl(player_data.second_hand), ntohl(player_data.second_hand_durability));
    
    // Count inventory items
    int inventory_count = 0;
    printf("\n" BOLD "     Inventory Items:" RESET "\n");
    for (int i = 0; i < 150; i++) {
        uint32_t item_id = ntohl(player_data.inventory[i]);
        if (item_id != 0) {
            printf("       Slot %3d: Item %u\n", i, item_id);
            inventory_count++;
        }
    }
    printf("       Total items in inventory: " BOLD "%d" RESET "\n", inventory_count);
    
    sleep(1);
    
    // ==========================================
    // STEP 9: TEST EQUIPMENT SYSTEM
    // ==========================================
    print_step(9, 10, "Equipment System Testing");
    
    // Find an item in inventory to equip
    uint32_t test_item_id = 0;
    uint8_t test_item_slot = 0;
    
    for (int i = 0; i < 150; i++) {
        uint32_t item_id = ntohl(player_data.inventory[i]);
        if (item_id != 0) {
            test_item_id = item_id;
            test_item_slot = i;
            break;
        }
    }
    
    if (test_item_id != 0) {
        print_info("Testing equipment functionality...");
        print_detail_uint("Test Item ID", test_item_id);
        print_detail_int("Inventory Slot", test_item_slot);
        
        // Determine equip slot based on item ID (simplified)
        uint8_t equip_slot = SLOT_MAIN_HAND; // Default
        
        // Based on your items.json:
        // 1000-1004 are weapons (main_hand or two_handed)
        // 2000-2004 are armor pieces
        if (test_item_id >= 2000 && test_item_id <= 2004) {
            // Armor pieces
            if (test_item_id == 2000) equip_slot = SLOT_HELMET;
            else if (test_item_id == 2001) equip_slot = SLOT_GLOVES;
            else if (test_item_id == 2002) equip_slot = SLOT_CHEST;
            else if (test_item_id == 2003) equip_slot = SLOT_LEGGINGS;
            else if (test_item_id == 2004) equip_slot = SLOT_BOOTS;
        } else if (test_item_id == 1003 || test_item_id == 1004 || test_item_id == 1005) {
            // Two-handed weapons
            equip_slot = SLOT_MAIN_HAND; // Server will handle two-handed logic
        } else if (test_item_id == 1001) {
            // Shield
            equip_slot = SLOT_OFF_HAND;
        }
        
        // Try to equip the item
        EquipItemPacket equip_pkt = {0};
        equip_pkt.header.type = PACKET_EQUIP_ITEM;
        equip_pkt.header.player_id = htonl(character_id);
        equip_pkt.item_id = htonl(test_item_id);
        equip_pkt.inventory_slot = test_item_slot;
        equip_pkt.equip_slot = equip_slot;
        
        if (send(world_fd, &equip_pkt, sizeof(equip_pkt), 0) <= 0) {
            print_error("Failed to send equip item packet");
        } else {
            EquipItemResponsePacket equip_resp;
            bytes = safe_recv(world_fd, &equip_resp, sizeof(equip_resp), "equip item");
            
            if (bytes > 0) {
                if (equip_resp.success) {
                    print_success("Item equipped successfully");
                    print_detail_uint("Equipped Item", ntohl(equip_resp.equipped_item));
                    print_detail("Server Message", equip_resp.message);
                    
                    // Try to unequip it
                    sleep(1);
                    print_info("Testing unequip functionality...");
                    
                    UnequipItemPacket unequip_pkt = {0};
                    unequip_pkt.header.type = PACKET_UNEQUIP_ITEM;
                    unequip_pkt.header.player_id = htonl(character_id);
                    unequip_pkt.equip_slot = equip_slot;
                    
                    if (send(world_fd, &unequip_pkt, sizeof(unequip_pkt), 0) <= 0) {
                        print_error("Failed to send unequip packet");
                    } else {
                        UnequipItemResponsePacket unequip_resp;
                        bytes = safe_recv(world_fd, &unequip_resp, sizeof(unequip_resp), "unequip item");
                        
                        if (bytes > 0 && unequip_resp.success) {
                            print_success("Item unequipped successfully");
                            print_detail_uint("Unequipped Item", ntohl(unequip_resp.unequipped_item));
                            print_detail_int("Returned to Slot", unequip_resp.inventory_slot);
                        } else {
                            print_error("Unequip failed");
                        }
                    }
                } else {
                    print_error("Equip failed");
                    print_detail("Server Message", equip_resp.message);
                }
            } else {
                print_error("No response from server");
            }
        }
    } else {
        print_info("No items in inventory to test equipment");
    }
    
    sleep(1);
    
    // ==========================================
    // STEP 10: TEST INVENTORY MANAGEMENT
    // ==========================================
    print_step(10, 10, "Inventory Management Testing");
    
    // Test moving items between slots
    if (inventory_count >= 2) {
        print_info("Testing inventory item movement...");
        
        // Find two occupied slots
        uint8_t slot1 = 255, slot2 = 255;
        for (int i = 0; i < 150; i++) {
            if (ntohl(player_data.inventory[i]) != 0) {
                if (slot1 == 255) slot1 = i;
                else if (slot2 == 255) {
                    slot2 = i;
                    break;
                }
            }
        }
        
        if (slot1 != 255 && slot2 != 255) {
            MoveItemPacket move_pkt = {0};
            move_pkt.header.type = PACKET_MOVE_ITEM;
            move_pkt.header.player_id = htonl(character_id);
            move_pkt.from_slot = slot1;
            move_pkt.to_slot = slot2;
            
            if (send(world_fd, &move_pkt, sizeof(move_pkt), 0) <= 0) {
                print_error("Failed to send move item packet");
            } else {
                MoveItemResponsePacket move_resp;
                bytes = safe_recv(world_fd, &move_resp, sizeof(move_resp), "move item");
                
                if (bytes > 0 && move_resp.success) {
                    print_success("Items swapped successfully");
                    print_detail_int("From Slot", move_resp.from_slot);
                    print_detail_int("To Slot", move_resp.to_slot);
                } else {
                    print_error("Move item failed");
                }
            }
        }
    } else {
        print_info("Not enough items to test inventory movement");
    }
    
    close(world_fd);
    
    // ==========================================
    // TEST SUMMARY
    // ==========================================
    stats_finish();
    
    print_header("ITEM SYSTEM TEST SUMMARY");
    
    pthread_mutex_lock(&g_stats.lock);
    int total = g_stats.total_steps;
    int passed = g_stats.passed_steps;
    int failed = g_stats.failed_steps;
    pthread_mutex_unlock(&g_stats.lock);
    
    printf("Total Test Steps: " BOLD "%d" RESET "\n", total);
    printf("Passed Steps: " GREEN BOLD "%d" RESET "\n", passed);
    printf("Failed Steps: " RED BOLD "%d" RESET "\n", failed);
    printf("Test Duration: " BOLD "%.2f seconds" RESET "\n\n", stats_elapsed_time());
    
    printf(BOLD "Character Summary:" RESET "\n");
    printf("  Name:  %s\n", character_name);
    printf("  Class: %s\n", get_class_name(selected_class));
    printf("  Race:  %s\n", get_race_name(selected_race));
    printf("  Items: %d in inventory\n\n", inventory_count);
    
    if (failed == 0) {
        printf(GREEN BOLD);
        printf("╔═══════════════════════════════════════════════════════════════╗\n");
        printf("║                                                               ║\n");
        printf("║           ✓ ITEM SYSTEM TEST PASSED!                         ║\n");
        printf("║                                                               ║\n");
        printf("║  All systems working correctly:                              ║\n");
        printf("║    • Character Customization  ✓                              ║\n");
        printf("║    • Item Database           ✓                              ║\n");
        printf("║    • Equipment System        ✓                              ║\n");
        printf("║    • Inventory Management    ✓                              ║\n");
        printf("║                                                               ║\n");
        printf("╚═══════════════════════════════════════════════════════════════╝\n");
        printf(RESET "\n");
        
        printf(CYAN "Usage: ./test_items_enhanced [username] [password] [class_id] [race_id]\n");
        printf("  Classes: 1=Gladiator, 2=Ninja, 3=Landweaver, 4=Spirit\n");
        printf("  Races: 1=Human, 2=Pyseck, 3=Infor\n" RESET);
        
        return 0;
    } else {
        printf(RED BOLD);
        printf("╔═══════════════════════════════════════════════════════════════╗\n");
        printf("║                                                               ║\n");
        printf("║              ✗ ITEM SYSTEM TEST FAILED                       ║\n");
        printf("║                                                               ║\n");
        printf("║  Please check the error messages above for details.          ║\n");
        printf("║                                                               ║\n");
        printf("╚═══════════════════════════════════════════════════════════════╝\n");
        printf(RESET "\n");
        return 1;
    }
}