#define _POSIX_C_SOURCE 200809L

#include "types.h"

#include "ability_def.h"
#include "items_database.h"
#include "player_data.h"
#include "players_database.h"
#include "class_stats.h"
#include "player_level.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define SAVE_INTERVAL_SECONDS 120 

// Global active players array
ActivePlayer active_players[MAX_PLAYERS];
pthread_mutex_t active_players_lock = PTHREAD_MUTEX_INITIALIZER;

static pthread_t g_save_thread;
static pthread_mutex_t g_save_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_save_cond = PTHREAD_COND_INITIALIZER;
static volatile int g_save_thread_running = 0;

int playerdata_init(const char* conn_str) {
    printf("Initializing player data system...\n");
    printf("PostgreSQL connection: %s\n", conn_str);
    
    // Initialize the character database
    if (!character_database_init(conn_str)) {
        fprintf(stderr, "Failed to initialize character database\n");
        return 0;
    }
    
    // Initialize active players array
    memset(active_players, 0, sizeof(active_players));
    for (int i = 0; i < MAX_PLAYERS; i++) {
        pthread_mutex_init(&active_players[i].lock, NULL);
    }
    
    printf("Player data system initialized successfully\n");
    return 1;
}

void playerdata_close(void) {
    printf("Closing player data system...\n");
    
    // Save all active players before shutdown
    pthread_mutex_lock(&active_players_lock);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (active_players[i].is_loaded) {
            pthread_mutex_lock(&active_players[i].lock);
            playerdata_save(&active_players[i]);
            pthread_mutex_unlock(&active_players[i].lock);
        }
        pthread_mutex_destroy(&active_players[i].lock);
    }
    pthread_mutex_unlock(&active_players_lock);
    
    // Close database connection
    character_database_close();
    
    printf("Player data system closed\n");
}

int playerdata_load(uint32_t character_id, ActivePlayer* player) {
    if (!player) {
        fprintf(stderr, "playerdata_load: NULL player pointer\n"); 
        return 0;
    }
    
    // Get full character data from database
    CharacterInfo char_info;
    memset(&char_info, 0, sizeof(char_info));
    
    if (!character_get_full_data(character_id, &char_info)) {
        fprintf(stderr, "Failed to load character %u from database\n", character_id); 
        return 0;
    }

    printf("[LOAD DEBUG] char_id=%u from DB: pos_x=%f, pos_y=%f\n",
       character_id, char_info.pos_x, char_info.pos_y);
    
    // Copy data into ActivePlayer structure
    player->character_id = char_info.character_id;
    strncpy(player->username, char_info.name, sizeof(player->username) - 1);
    player->username[sizeof(player->username) - 1] = '\0';
    
    player->pos_x = char_info.pos_x;
    player->pos_y = char_info.pos_y;
    
    // ============================================================
    // FIX: Set default spawn position if position is (0,0)
    // This handles new characters that haven't been placed yet
    // ============================================================
    if (player->pos_x == 0.0f && player->pos_y == 0.0f) {
        // Default spawn point (center of your walkable area)
        player->pos_x = 1608.0f;  // Or wherever your spawn should be
        player->pos_y = 1108.0f;
        player->is_dirty = 1;  // Mark for save so this persists
        printf("[SPAWN] New character %u spawned at default location (%.1f, %.1f)\n",
               character_id, player->pos_x, player->pos_y);
    }

    player->vel_x = 0.0f;
    player->vel_y = 0.0f;
    
    player->level = char_info.level;
    player->health = char_info.health;
    player->max_health = char_info.max_health;

    player->health = char_info.health;
    player->max_health = char_info.max_health;
    player->mana = char_info.mana;
    player->max_mana = char_info.max_mana;
    player->experience = char_info.experience;
    player->gold = char_info.gold;
    
    player->player_class = char_info.player_class;

    // --- Compute all derived stats from class + level + equipment ---
    player_apply_class_stats(player);

    // For first login (DB has default health=100), use the class max
    if (player->health <= 0 || player->health > player->max_health) {
        player->health = player->max_health;
    }
    // Start at full mana (or restore from DB)
    if (player->mana <= 0 || player->mana > player->max_mana) {
        player->mana = player->max_mana;
    }
    // Start at full mana
    player->mana = player->max_mana;

    // --- Assign abilities for this class + level ---
    {
        uint16_t class_abilities[10];
        int total = ability_get_class_abilities(player->player_class, class_abilities, 10);

        player->ability_count = 0;
        memset(player->ability_cooldowns, 0, sizeof(player->ability_cooldowns));

        for (int a = 0; a < total && player->ability_count < 5; a++) {
            const AbilityDef* ab = ability_get(class_abilities[a]);
            if (ab && player->level >= ab->unlock_level) {
                player->ability_slots[player->ability_count] = ab->id;
                player->ability_cooldowns[player->ability_count] = 0.0f;
                player->ability_count++;
            }
        }

        printf("[ABILITIES] Player %u (class %u, level %d): %d abilities assigned\n",
               player->character_id, player->player_class, player->level,
               player->ability_count);
    }

    player->player_race = char_info.player_race;
    
    // Copy equipment data
    player->helmet = char_info.helmet;
    player->gloves = char_info.gloves;
    player->chest_armor = char_info.chest_armor;
    player->leggings = char_info.leggings;
    player->boots = char_info.boots;
    player->main_hand = char_info.main_hand;
    player->second_hand = char_info.second_hand;
    player->blessing = char_info.blessing;
    
    // Copy inventory data
    memcpy(player->inventory, char_info.inventory, sizeof(player->inventory));

    // Now that equipment is loaded, recalculate stats with gear bonuses
    player_apply_equipment_bonuses(player);

    player->is_loaded = 1;
    player->is_dirty = player->is_dirty ? 1 : 0;  // Keep dirty flag if we set spawn
    player->last_save = time(NULL);
    player->last_activity = time(NULL);
    gettimeofday(&player->last_move_tv, NULL);
    
    printf("Loaded character %u: %s (level %d) at pos=(%.2f, %.2f)\n", 
           character_id, player->username, player->level, player->pos_x, player->pos_y);
    
    return 1;
}

int playerdata_save(ActivePlayer* player) {
    if (!player || !player->is_loaded) {
        fprintf(stderr, "playerdata_save: Invalid player or not loaded\n");
        return 0;
    }
    
    // Prepare character info structure for database
    CharacterInfo char_info = {0};
    char_info.character_id = player->character_id;
    strncpy(char_info.name, player->username, sizeof(char_info.name) - 1);
    char_info.level = player->level;
    char_info.pos_x = player->pos_x;
    char_info.pos_y = player->pos_y;
    char_info.health = player->health;
    char_info.max_health = player->max_health;
    char_info.mana = player->mana;
    char_info.max_mana = player->max_mana;
    char_info.experience = player->experience;
    char_info.gold = player->gold;
    
    // Copy equipment data
    char_info.helmet = player->helmet;
    char_info.gloves = player->gloves;
    char_info.chest_armor = player->chest_armor;
    char_info.leggings = player->leggings;
    char_info.boots = player->boots;
    char_info.main_hand = player->main_hand;
    char_info.second_hand = player->second_hand;
    char_info.blessing = player->blessing;
    
    // Copy inventory data
    memcpy(char_info.inventory, player->inventory, sizeof(char_info.inventory));

    // DEBUG — print what we're about to save
    printf("[SAVE DEBUG] char %u: player->pos = (%f, %f), char_info->pos = (%f, %f)\n",
           player->character_id,
           player->pos_x, player->pos_y,
           char_info.pos_x, char_info.pos_y);
    
    // Save to database
    if (!character_update_full_data(&char_info)) {
        fprintf(stderr, "Failed to save character %u to database\n", player->character_id);
        return 0;
    }
    
    player->is_dirty = 0;
    player->last_save = time(NULL);
    
    printf("Saved character %u to database\n", player->character_id);
    return 1;
}

int player_add_active(uint32_t character_id, int client_fd) {
    pthread_mutex_lock(&active_players_lock);
    
    // Find an empty slot
    int slot = -1;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!active_players[i].is_loaded) {
            slot = i;
            break;
        }
    }
    
    if (slot == -1) {
        pthread_mutex_unlock(&active_players_lock);
        fprintf(stderr, "No available slots for new player\n");
        return 0;
    }
    
    // Initialize the player slot
    pthread_mutex_lock(&active_players[slot].lock);

    // Save the mutex, clear everything else, restore the mutex
    pthread_mutex_t saved_lock = active_players[slot].lock;
    memset(&active_players[slot], 0, sizeof(ActivePlayer));
    active_players[slot].lock = saved_lock;

    active_players[slot].character_id = character_id;
    active_players[slot].client_fd = client_fd;
    
    // Load player data from database
    if (!playerdata_load(character_id, &active_players[slot])) {
        pthread_mutex_unlock(&active_players[slot].lock);
        pthread_mutex_unlock(&active_players_lock);
        fprintf(stderr, "Failed to load player data for character %u\n", character_id);
        return 0;
    }
    
    pthread_mutex_unlock(&active_players[slot].lock);
    pthread_mutex_unlock(&active_players_lock);
    
    printf("Added active player: char_id=%u, fd=%d, slot=%d\n", 
           character_id, client_fd, slot);
    return 1;
}

ActivePlayer* player_find_active(uint32_t character_id) {
    pthread_mutex_lock(&active_players_lock);
    
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (active_players[i].is_loaded && 
            active_players[i].character_id == character_id) {
            pthread_mutex_unlock(&active_players_lock);
            return &active_players[i];
        }
    }
    
    pthread_mutex_unlock(&active_players_lock);
    return NULL;
}

void player_remove_active(uint32_t character_id) {
    pthread_mutex_lock(&active_players_lock);
    
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (active_players[i].is_loaded && 
            active_players[i].character_id == character_id) {
            
            pthread_mutex_lock(&active_players[i].lock);
            
            if (active_players[i].is_dirty) {
                playerdata_save(&active_players[i]);
            }
            
            pthread_mutex_t saved_lock = active_players[i].lock;
            memset(&active_players[i], 0, sizeof(ActivePlayer));
            active_players[i].lock = saved_lock;
            
            pthread_mutex_unlock(&active_players[i].lock);
            
            printf("Removed active player: char_id=%u, slot=%d\n", character_id, i);
            break;
        }
    }
    
    pthread_mutex_unlock(&active_players_lock);
}

void player_send_data_response(int client_fd, uint32_t character_id) {
    ActivePlayer* player = player_find_active(character_id);
    if (!player) {
        fprintf(stderr, "Cannot send data for inactive player %u\n", character_id);
        return;
    }
    
    pthread_mutex_lock(&player->lock);
    
    // ALLOCATE ON HEAP instead of stack
    CharacterInfo* response = malloc(sizeof(CharacterInfo));
    if (!response) {
        fprintf(stderr, "Failed to allocate response packet\n");
        pthread_mutex_unlock(&player->lock);
        return;
    }
    
    memset(response, 0, sizeof(CharacterInfo));
    
    response->header.type = PACKET_PLAYER_DATA_RESPONSE;
    response->header.player_id = htonl(character_id);
    response->header.payload_size = htons(sizeof(CharacterInfo) - sizeof(PacketHeader));
    
    // Basic stats
    response->level = htonl(player->level);
    response->health = htonl(player->health);
    response->max_health = htonl(player->max_health);
    response->mana = htonl((uint32_t)player->mana);
    response->max_mana = htonl((uint32_t)player->max_mana);
    response->experience = htonll(player->experience);
    response->gold = htonl(player->gold);
    
    response->pos_x = player->pos_x;
    response->pos_y = player->pos_y;

    strncpy(response->name, player->username, sizeof(response->name) - 1);
    response->name[sizeof(response->name) - 1] = '\0';
    response->player_class = htonl(player->player_class);
    response->player_race  = htonl(player->player_race);
    
    // Equipment
    response->helmet = htonl(player->helmet);
    response->gloves = htonl(player->gloves);
    response->chest_armor = htonl(player->chest_armor);
    response->leggings = htonl(player->leggings);
    response->boots = htonl(player->boots);
    response->main_hand = htonl(player->main_hand);
    response->second_hand = htonl(player->second_hand);
    response->blessing = htons(player->blessing);
    
    // Inventory
    for (int i = 0; i < 150; i++) {
        response->inventory[i] = htonl(player->inventory[i]);
    }
    
    pthread_mutex_unlock(&player->lock);

    // Send packet
    send(client_fd, response, sizeof(CharacterInfo), 0);
    free(response);

    // NOTE: player_send_stats() acquires player->lock internally,
    // so we MUST call it AFTER unlocking above to avoid deadlock.
    player_send_stats(client_fd, player);
    
    printf("Sent player data for character %u\n", character_id);
}

void* periodic_save_thread(void* arg) {
    (void)arg;
    g_save_thread_running = 1;

    while (1) {

        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += SAVE_INTERVAL_SECONDS;

        pthread_mutex_lock(&g_save_mutex);
        while (g_save_thread_running) {
            int ret = pthread_cond_timedwait(&g_save_cond, &g_save_mutex, &deadline);
            if (ret == ETIMEDOUT || !g_save_thread_running) break;
        }
        pthread_mutex_unlock(&g_save_mutex);

        // If we were told to stop, exit immediately
        if (!g_save_thread_running) break;

        // Do the save pass
        pthread_mutex_lock(&active_players_lock);
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (!active_players[i].is_loaded) continue;

            pthread_mutex_lock(&active_players[i].lock);
            if (active_players[i].is_dirty) {
                printf("Periodic save: character %u\n", active_players[i].character_id);
                playerdata_save(&active_players[i]);
            }
            pthread_mutex_unlock(&active_players[i].lock);
        }
        pthread_mutex_unlock(&active_players_lock);
    }

    printf("Periodic save thread exiting\n");
    return NULL;
}

int playerdata_start_save_thread(void) {
    if (g_save_thread_running) return 1;

    if (pthread_create(&g_save_thread, NULL, periodic_save_thread, NULL) != 0) {
        fprintf(stderr, "Failed to create periodic save thread\n");
        return 0;
    }
    return 1;
}

void playerdata_stop_save_thread(void) {
    if (!g_save_thread_running) return;

    pthread_mutex_lock(&g_save_mutex);
    g_save_thread_running = 0;
    pthread_cond_signal(&g_save_cond);
    pthread_mutex_unlock(&g_save_mutex);

    pthread_join(g_save_thread, NULL);
}
