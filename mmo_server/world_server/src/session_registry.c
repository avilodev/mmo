#include "session_registry.h"
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>

SessionRegistry g_session_registry;

void session_registry_init(void) {
    memset(&g_session_registry, 0, sizeof(g_session_registry));
    pthread_rwlock_init(&g_session_registry.lock, NULL);
}

int session_registry_add(int fd, uint32_t account_id, uint32_t character_id) {
    pthread_rwlock_wrlock(&g_session_registry.lock);

    // If the account is already logged in, kick the stale session so the new
    // login can proceed.  This handles half-open TCP connections where the
    // client closed the socket but the server never received the FIN/RST.
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (g_session_registry.entries[i].active &&
            g_session_registry.entries[i].account_id == account_id) {
            int old_fd = g_session_registry.entries[i].fd;
            printf("Account %u already logged in (fd=%d) — kicking stale session for new login (fd=%d)\n",
                   account_id, old_fd, fd);
            // Mark inactive before closing so the old handler's client_done
            // sees nothing to clean up (session already gone).
            g_session_registry.entries[i].active = 0;
            pthread_rwlock_unlock(&g_session_registry.lock);
            // Closing the old fd causes the old client_handler_thread's
            // recv() to return an error, which breaks its poll loop and
            // triggers player save + player_remove_active.
            shutdown(old_fd, SHUT_RDWR);
            close(old_fd);
            pthread_rwlock_wrlock(&g_session_registry.lock);
            break;
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

// Returns 1 and copies entry into *out while holding the read lock.
// Returns 0 if not found. The caller owns the copy — no pointer into the
// live registry is ever exposed, so there is no use-after-unlock race.
int session_find_by_fd(int fd, SessionEntry* out) {
    pthread_rwlock_rdlock(&g_session_registry.lock);
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (g_session_registry.entries[i].active &&
            g_session_registry.entries[i].fd == fd) {
            if (out) *out = g_session_registry.entries[i];
            pthread_rwlock_unlock(&g_session_registry.lock);
            return 1;
        }
    }
    pthread_rwlock_unlock(&g_session_registry.lock);
    return 0;
}

int session_find_by_account(uint32_t account_id, SessionEntry* out) {
    pthread_rwlock_rdlock(&g_session_registry.lock);
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (g_session_registry.entries[i].active &&
            g_session_registry.entries[i].account_id == account_id) {
            if (out) *out = g_session_registry.entries[i];
            pthread_rwlock_unlock(&g_session_registry.lock);
            return 1;
        }
    }
    pthread_rwlock_unlock(&g_session_registry.lock);
    return 0;
}

int session_find_by_character(uint32_t character_id, SessionEntry* out) {
    pthread_rwlock_rdlock(&g_session_registry.lock);
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (g_session_registry.entries[i].active &&
            g_session_registry.entries[i].character_id == character_id) {
            if (out) *out = g_session_registry.entries[i];
            pthread_rwlock_unlock(&g_session_registry.lock);
            return 1;
        }
    }
    pthread_rwlock_unlock(&g_session_registry.lock);
    return 0;
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