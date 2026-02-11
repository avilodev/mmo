#include "session_registry.h"
#include <string.h>
#include <stdio.h>

SessionRegistry g_session_registry;

void session_registry_init(void) {
    memset(&g_session_registry, 0, sizeof(g_session_registry));
    pthread_rwlock_init(&g_session_registry.lock, NULL);
}

int session_registry_add(int fd, uint32_t account_id, uint32_t character_id) {
    pthread_rwlock_wrlock(&g_session_registry.lock);
    
    // Check if account already logged in
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (g_session_registry.entries[i].active && 
            g_session_registry.entries[i].account_id == account_id) {
            pthread_rwlock_unlock(&g_session_registry.lock);
            printf("Account %u already logged in (fd=%d)\n", 
                   account_id, g_session_registry.entries[i].fd);
            return -1;  // Dual-login prevented!
        }
    }
    
    // Find empty slot
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!g_session_registry.entries[i].active) {
            g_session_registry.entries[i].fd = fd;
            g_session_registry.entries[i].account_id = account_id;
            g_session_registry.entries[i].character_id = character_id;
            g_session_registry.entries[i].connect_time = time(NULL);
            g_session_registry.entries[i].last_activity = time(NULL);
            g_session_registry.entries[i].active = 1;
            
            pthread_rwlock_unlock(&g_session_registry.lock);
            printf("Session added: fd=%d, account=%u, character=%u\n",
                   fd, account_id, character_id);
            return 0;
        }
    }
    
    pthread_rwlock_unlock(&g_session_registry.lock);
    printf("Session registry full!\n");
    return -1;  // Server full
}

void session_registry_remove(int fd) {
    pthread_rwlock_wrlock(&g_session_registry.lock);
    
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (g_session_registry.entries[i].active && 
            g_session_registry.entries[i].fd == fd) {
            printf("Session removed: fd=%d, account=%u, character=%u\n",
                   fd, 
                   g_session_registry.entries[i].account_id,
                   g_session_registry.entries[i].character_id);
            g_session_registry.entries[i].active = 0;
            break;
        }
    }
    
    pthread_rwlock_unlock(&g_session_registry.lock);
}

SessionEntry* session_find_by_fd(int fd) {
    pthread_rwlock_rdlock(&g_session_registry.lock);
    
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (g_session_registry.entries[i].active && 
            g_session_registry.entries[i].fd == fd) {
            pthread_rwlock_unlock(&g_session_registry.lock);
            return &g_session_registry.entries[i];
        }
    }
    
    pthread_rwlock_unlock(&g_session_registry.lock);
    return NULL;
}

SessionEntry* session_find_by_account(uint32_t account_id) {
    pthread_rwlock_rdlock(&g_session_registry.lock);
    
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (g_session_registry.entries[i].active && 
            g_session_registry.entries[i].account_id == account_id) {
            pthread_rwlock_unlock(&g_session_registry.lock);
            return &g_session_registry.entries[i];
        }
    }
    
    pthread_rwlock_unlock(&g_session_registry.lock);
    return NULL;
}

SessionEntry* session_find_by_character(uint32_t character_id) {
    pthread_rwlock_rdlock(&g_session_registry.lock);
    
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (g_session_registry.entries[i].active && 
            g_session_registry.entries[i].character_id == character_id) {
            pthread_rwlock_unlock(&g_session_registry.lock);
            return &g_session_registry.entries[i];
        }
    }
    
    pthread_rwlock_unlock(&g_session_registry.lock);
    return NULL;
}

void session_update_activity(int fd) {
    pthread_rwlock_wrlock(&g_session_registry.lock);
    
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (g_session_registry.entries[i].active && 
            g_session_registry.entries[i].fd == fd) {
            g_session_registry.entries[i].last_activity = time(NULL);
            break;
        }
    }
    
    pthread_rwlock_unlock(&g_session_registry.lock);
}