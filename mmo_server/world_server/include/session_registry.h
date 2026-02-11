#ifndef SESSION_REGISTRY_H
#define SESSION_REGISTRY_H

#include <stdint.h>
#include <pthread.h>
#include <time.h>

#define MAX_SESSIONS 10000

typedef struct {
    int fd;
    uint32_t account_id;
    uint32_t character_id;
    time_t connect_time;
    time_t last_activity;
    uint8_t active;
} SessionEntry;

typedef struct {
    SessionEntry entries[MAX_SESSIONS];
    pthread_rwlock_t lock;
} SessionRegistry;

extern SessionRegistry g_session_registry;

// Initialize the registry
void session_registry_init(void);

// Add a new session (returns 0 on success, -1 if account already logged in)
int session_registry_add(int fd, uint32_t account_id, uint32_t character_id);

// Remove a session
void session_registry_remove(int fd);

// Find session by fd
SessionEntry* session_find_by_fd(int fd);

// Find session by account (for dual-login prevention)
SessionEntry* session_find_by_account(uint32_t account_id);

// Find session by character (for messaging)
SessionEntry* session_find_by_character(uint32_t character_id);

// Update last activity (for timeout detection)
void session_update_activity(int fd);

#endif