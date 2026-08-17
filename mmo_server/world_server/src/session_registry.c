/**
 * @file
 * Track authenticated world sessions by descriptor, account, and character.
 */
#include "session_registry.h"
#include "log.h"
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>

SessionRegistry g_session_registry;

/** Initialize the process-wide session registry and its read-write lock. */
void session_registry_init(void) {
    memset(&g_session_registry, 0, sizeof(g_session_registry));
    pthread_rwlock_init(&g_session_registry.lock, NULL);
}

/**
 * Register an authenticated session, shutting down any stale session for the account.
 *
 * The stale session's handler retains ownership of its descriptor and performs the eventual close.
 *
 * @return      Zero on success, or -1 when no registry slot remains.
 */
int session_registry_add(int fd, uint32_t account_id, uint32_t character_id) {
    pthread_rwlock_wrlock(&g_session_registry.lock);

    // If the account is already logged in, kick the stale session so the new
    // login can proceed.  This handles half-open TCP connections where the
    // client closed the socket but the server never received the FIN/RST.
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (g_session_registry.entries[i].active &&
            g_session_registry.entries[i].account_id == account_id) {
            int old_fd = g_session_registry.entries[i].fd;
            LOG_WARN_RL(5, 60, "Account %u already logged in (fd=%d) — kicking stale session for new login (fd=%d)", account_id, old_fd, fd);
            // Mark inactive before closing so the old handler's client_done
            // sees nothing to clean up (session already gone).
            g_session_registry.entries[i].active = 0;
            pthread_rwlock_unlock(&g_session_registry.lock);
            // shutdown() wakes the old handler.  That handler owns the fd and
            // is solely responsible for close(); closing it here as well can
            // close an unrelated connection if Linux reuses the fd first.
            shutdown(old_fd, SHUT_RDWR);
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
            LOG_DEBUG("Session added: fd=%d, account=%u, character=%u", fd, account_id, character_id);
            return 0;
        }
    }
    
    pthread_rwlock_unlock(&g_session_registry.lock);
    LOG_DEBUG("Session registry full!");
    return -1;  // Server full
}

/** Mark the session associated with a descriptor inactive. */
void session_registry_remove(int fd) {
    pthread_rwlock_wrlock(&g_session_registry.lock);
    
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (g_session_registry.entries[i].active && 
            g_session_registry.entries[i].fd == fd) {
            LOG_DEBUG("Session removed: fd=%d, account=%u, character=%u", fd, g_session_registry.entries[i].account_id, g_session_registry.entries[i].character_id);
            g_session_registry.entries[i].active = 0;
            break;
        }
    }
    
    pthread_rwlock_unlock(&g_session_registry.lock);
}

/**
 * Copy a session selected by descriptor while holding the registry read lock.
 *
 * @param out  Receives a detached copy when non-NULL.
 * @return     Nonzero when a session is found, otherwise zero.
 */
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

/**
 * Copy a session selected by account identifier while holding the registry read lock.
 *
 * @param out  Receives a detached copy when non-NULL.
 * @return     Nonzero when a session is found, otherwise zero.
 */
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

/**
 * Copy a session selected by character identifier while holding the registry read lock.
 *
 * @param out  Receives a detached copy when non-NULL.
 * @return     Nonzero when a session is found, otherwise zero.
 */
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

/** Refresh the last-activity timestamp for a descriptor's session. */
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
