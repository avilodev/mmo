#ifndef SESSION_REGISTRY_H
#define SESSION_REGISTRY_H

#include <stdint.h>
#include <pthread.h>
#include <time.h>

/** Bound simultaneously tracked world sessions. */
#define MAX_SESSIONS 10000

/** Record one connection's authenticated identity and activity timestamps. */
typedef struct {
    int fd;
    uint32_t account_id;
    uint32_t character_id;
    time_t connect_time;
    time_t last_activity;
    uint8_t active;
} SessionEntry;

/** Protect the fixed world-session table with a reader/writer lock. */
typedef struct {
    SessionEntry entries[MAX_SESSIONS];
    pthread_rwlock_t lock;
} SessionRegistry;

extern SessionRegistry g_session_registry;

void session_registry_init(void);

// return -1 when the account already has an active session
int session_registry_add(int fd, uint32_t account_id, uint32_t character_id);

void session_registry_remove(int fd);

// copy a matching entry and return without retaining the registry lock
int session_find_by_fd(int fd, SessionEntry* out);

// copy a matching entry and return without retaining the registry lock
int session_find_by_account(uint32_t account_id, SessionEntry* out);

// copy a matching entry and return without retaining the registry lock
int session_find_by_character(uint32_t character_id, SessionEntry* out);

void session_update_activity(int fd);

#endif