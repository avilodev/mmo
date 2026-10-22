/**
 * @file
 * Track authenticated world sessions by descriptor, account, and character.
 *
 * The table is indexed by descriptor and carries two open-addressed indexes for the
 * identifier lookups, so add, remove, and all three finds are O(1). The previous
 * version was a 10,000-entry array scanned linearly under a process-wide exclusive
 * rwlock, and session_update_activity() ran that scan on every inbound packet from
 * every client to refresh a field nothing ever read.
 */
#include "session_registry.h"
#include "log.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/resource.h>

/** Size the descriptor table from the descriptor limit, as connection_io.c does:
 *  every fd the process can hold must have a slot. */
#define SESSION_MIN_SLOTS   1024
#define SESSION_MAX_SLOTS   65536

/** Give the identifier indexes twice the descriptor capacity, so they can never
 *  exceed a 50% load factor and linear probe chains stay short. */
#define SESSION_INDEX_HEADROOM  2
#define SESSION_INDEX_MIN_CAP   2048

#define IDX_EMPTY      (-1)
#define IDX_TOMBSTONE  (-2)

/** Map one identifier to the descriptor holding its session. */
typedef struct {
    uint32_t key;
    int32_t  fd;    // >= 0 live, IDX_EMPTY, or IDX_TOMBSTONE
} IndexEntry;

/** Hold one open-addressed identifier index sized at startup. */
typedef struct {
    IndexEntry* slots;
    int         cap;         // power of two
    uint32_t    mask;        // cap - 1
    uint32_t    shift;       // 32 - log2(cap), for the multiplicative hash
    int         tombstones;
} IdIndex;

static SessionEntry*    g_entries    = NULL;
static int              g_slot_count = 0;
static IdIndex          g_by_account;
static IdIndex          g_by_character;
static pthread_rwlock_t g_lock = PTHREAD_RWLOCK_INITIALIZER;

/** Take the high bits of a multiplicative hash; the low bits of one are poorly mixed. */
static inline uint32_t index_hash(const IdIndex* ix, uint32_t key) {
    return (key * 2654435769u) >> ix->shift;
}

static void index_reset(IdIndex* ix) {
    for (int i = 0; i < ix->cap; i++) {
        ix->slots[i].key = 0;
        ix->slots[i].fd  = IDX_EMPTY;
    }
    ix->tombstones = 0;
}

/**
 * Allocate one index large enough that it can never exceed half capacity.
 *
 * @return Nonzero on success.
 */
static int index_alloc(IdIndex* ix, int descriptor_slots) {
    int cap = SESSION_INDEX_MIN_CAP;
    uint32_t shift = 32;
    for (uint32_t bits = 0; bits < 32; bits++) {
        if ((1 << bits) >= SESSION_INDEX_MIN_CAP &&
            (1 << bits) >= descriptor_slots * SESSION_INDEX_HEADROOM) {
            cap   = 1 << bits;
            shift = 32 - bits;
            break;
        }
    }

    ix->slots = calloc((size_t)cap, sizeof(IndexEntry));
    if (!ix->slots) return 0;

    ix->cap   = cap;
    ix->mask  = (uint32_t)cap - 1u;
    ix->shift = shift;
    index_reset(ix);
    return 1;
}

/**
 * Find an identifier in an open-addressed index.
 *
 * The caller must hold at least the registry read lock.
 *
 * @return The descriptor, or -1 when absent.
 */
static int index_find(const IdIndex* ix, uint32_t key) {
    if (!ix->slots || key == 0) return -1;

    uint32_t pos = index_hash(ix, key);
    for (int probe = 0; probe < ix->cap; probe++) {
        const IndexEntry* e = &ix->slots[pos];
        if (e->fd == IDX_EMPTY) return -1;                 // chain ends: miss
        if (e->fd >= 0 && e->key == key) return e->fd;
        pos = (pos + 1) & ix->mask;                        // tombstone or collision
    }
    return -1;
}

/**
 * Insert or rebind an identifier in an open-addressed index.
 *
 * The caller must hold the registry write lock.
 */
static void index_insert(IdIndex* ix, uint32_t key, int fd) {
    if (!ix->slots || key == 0) return;

    uint32_t pos = index_hash(ix, key);
    int reuse = -1;
    for (int probe = 0; probe < ix->cap; probe++) {
        IndexEntry* e = &ix->slots[pos];
        if (e->fd == IDX_TOMBSTONE) {
            // probe past tombstones so a live duplicate is never shadowed
            if (reuse < 0) reuse = (int)pos;
        } else if (e->fd == IDX_EMPTY) {
            if (reuse >= 0) { ix->tombstones--; pos = (uint32_t)reuse; }
            ix->slots[pos].key = key;
            ix->slots[pos].fd  = fd;
            return;
        } else if (e->key == key) {
            e->fd = fd;          // rebind an identifier to a new descriptor
            return;
        }
        pos = (pos + 1) & ix->mask;
    }
    // Unreachable while capacity exceeds the descriptor limit, but never corrupt.
    LOG_ERROR("[SESSION] index full inserting key %u", key);
}

/**
 * Rebuild both identifier indexes from the live descriptor table.
 *
 * The caller must hold the registry write lock.
 */
static void index_rebuild_all(void) {
    index_reset(&g_by_account);
    index_reset(&g_by_character);
    for (int fd = 0; fd < g_slot_count; fd++) {
        if (!g_entries[fd].active) continue;
        index_insert(&g_by_account,   g_entries[fd].account_id,   fd);
        index_insert(&g_by_character, g_entries[fd].character_id, fd);
    }
}

/**
 * Remove an identifier from an open-addressed index.
 *
 * The caller must hold the registry write lock.
 *
 * @return Nonzero when tombstone pressure now warrants a rebuild.
 */
static int index_remove(IdIndex* ix, uint32_t key) {
    if (!ix->slots || key == 0) return 0;

    uint32_t pos = index_hash(ix, key);
    for (int probe = 0; probe < ix->cap; probe++) {
        IndexEntry* e = &ix->slots[pos];
        if (e->fd == IDX_EMPTY) break;
        if (e->fd >= 0 && e->key == key) {
            e->fd  = IDX_TOMBSTONE;
            e->key = 0;
            ix->tombstones++;
            break;
        }
        pos = (pos + 1) & ix->mask;
    }
    return ix->tombstones > ix->cap / 4;
}

/**
 * Deactivate one descriptor's session and unindex its identifiers.
 *
 * The caller must hold the registry write lock.
 */
static void entry_clear(int fd) {
    SessionEntry* e = &g_entries[fd];
    if (!e->active) return;

    int churn = index_remove(&g_by_account, e->account_id);
    churn |= index_remove(&g_by_character, e->character_id);

    memset(e, 0, sizeof(*e));

    // rebuild after sustained login and logout churn
    if (churn) index_rebuild_all();
}

/** Allocate the descriptor table and identifier indexes. */
void session_registry_init(void) {
    if (g_entries) return;

    struct rlimit rl;
    long wanted = SESSION_MAX_SLOTS;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY)
        wanted = (long)rl.rlim_cur;

    if (wanted < SESSION_MIN_SLOTS) wanted = SESSION_MIN_SLOTS;
    if (wanted > SESSION_MAX_SLOTS) wanted = SESSION_MAX_SLOTS;

    g_entries = calloc((size_t)wanted, sizeof(SessionEntry));
    if (!g_entries ||
        !index_alloc(&g_by_account,   (int)wanted) ||
        !index_alloc(&g_by_character, (int)wanted)) {
        LOG_ERROR("[SESSION] could not allocate registry for %ld descriptors", wanted);
        session_registry_shutdown();
        return;
    }

    g_slot_count = (int)wanted;
    LOG_INFO("[SESSION] registry ready: %d descriptor slots, %d index buckets",
             g_slot_count, g_by_account.cap);
}

/** Release the descriptor table and identifier indexes. */
void session_registry_shutdown(void) {
    free(g_entries);
    free(g_by_account.slots);
    free(g_by_character.slots);
    g_entries    = NULL;
    g_slot_count = 0;
    memset(&g_by_account,   0, sizeof(g_by_account));
    memset(&g_by_character, 0, sizeof(g_by_character));
}

/**
 * Register an authenticated session, shutting down any stale session for the account.
 *
 * The stale session's handler retains ownership of its descriptor and performs the
 * eventual close.
 *
 * @return      Zero on success, or -1 when the descriptor is out of range.
 */
int session_registry_add(int fd, uint32_t account_id, uint32_t character_id) {
    if (!g_entries || fd < 0 || fd >= g_slot_count) {
        LOG_ERROR("[SESSION] fd=%d outside the %d-slot registry", fd, g_slot_count);
        return -1;
    }

    pthread_rwlock_wrlock(&g_lock);

    // If the account is already logged in, kick the stale session so the new
    // login can proceed.  This handles half-open TCP connections where the
    // client closed the socket but the server never received the FIN/RST.
    int old_fd = index_find(&g_by_account, account_id);
    if (old_fd >= 0 && old_fd != fd) {
        LOG_WARN_RL(5, 60, "Account %u already logged in (fd=%d) — kicking stale session for new login (fd=%d)", account_id, old_fd, fd);
        // Clear before closing so the old handler's client_done sees nothing to
        // clean up (session already gone).
        entry_clear(old_fd);
        pthread_rwlock_unlock(&g_lock);
        // shutdown() wakes the old handler.  That handler owns the fd and
        // is solely responsible for close(); closing it here as well can
        // close an unrelated connection if Linux reuses the fd first.
        shutdown(old_fd, SHUT_RDWR);
        pthread_rwlock_wrlock(&g_lock);
    }

    // A descriptor reused after an unclean teardown, or re-authenticated in place,
    // must not leave its previous identifiers pointing here.
    entry_clear(fd);

    g_entries[fd].fd           = fd;
    g_entries[fd].account_id   = account_id;
    g_entries[fd].character_id = character_id;
    g_entries[fd].connect_time = time(NULL);
    g_entries[fd].active       = 1;

    index_insert(&g_by_account,   account_id,   fd);
    index_insert(&g_by_character, character_id, fd);

    pthread_rwlock_unlock(&g_lock);

    LOG_DEBUG("Session added: fd=%d, account=%u, character=%u", fd, account_id, character_id);
    return 0;
}

/** Mark the session associated with a descriptor inactive. */
void session_registry_remove(int fd) {
    if (!g_entries || fd < 0 || fd >= g_slot_count) return;

    pthread_rwlock_wrlock(&g_lock);
    if (g_entries[fd].active) {
        LOG_DEBUG("Session removed: fd=%d, account=%u, character=%u", fd, g_entries[fd].account_id, g_entries[fd].character_id);
        entry_clear(fd);
    }
    pthread_rwlock_unlock(&g_lock);
}

/**
 * Copy a session selected by descriptor while holding the registry read lock.
 *
 * @param out  Receives a detached copy when non-NULL.
 * @return     Nonzero when a session is found, otherwise zero.
 */
int session_find_by_fd(int fd, SessionEntry* out) {
    if (!g_entries || fd < 0 || fd >= g_slot_count) return 0;

    int found = 0;
    pthread_rwlock_rdlock(&g_lock);
    if (g_entries[fd].active) {
        if (out) *out = g_entries[fd];
        found = 1;
    }
    pthread_rwlock_unlock(&g_lock);
    return found;
}

/**
 * Copy a session selected by account identifier while holding the registry read lock.
 *
 * @param out  Receives a detached copy when non-NULL.
 * @return     Nonzero when a session is found, otherwise zero.
 */
int session_find_by_account(uint32_t account_id, SessionEntry* out) {
    if (!g_entries) return 0;

    int found = 0;
    pthread_rwlock_rdlock(&g_lock);
    int fd = index_find(&g_by_account, account_id);
    if (fd >= 0 && g_entries[fd].active) {
        if (out) *out = g_entries[fd];
        found = 1;
    }
    pthread_rwlock_unlock(&g_lock);
    return found;
}

/**
 * Copy a session selected by character identifier while holding the registry read lock.
 *
 * @param out  Receives a detached copy when non-NULL.
 * @return     Nonzero when a session is found, otherwise zero.
 */
int session_find_by_character(uint32_t character_id, SessionEntry* out) {
    if (!g_entries) return 0;

    int found = 0;
    pthread_rwlock_rdlock(&g_lock);
    int fd = index_find(&g_by_character, character_id);
    if (fd >= 0 && g_entries[fd].active) {
        if (out) *out = g_entries[fd];
        found = 1;
    }
    pthread_rwlock_unlock(&g_lock);
    return found;
}
