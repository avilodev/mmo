// ============================================================================
// connection_io.c — Per-connection outbound write queue.
//
// A send either goes out immediately or gets queued and drained when the socket
// reports writable. That keeps a slow client from blocking a broadcast for
// everyone else.
//
// Slots are indexed directly by file descriptor. The previous version scanned a
// fixed array under one global mutex on every call, which put an O(n) walk and
// a process-wide lock on the hottest path in the server — every broadcast, to
// every player, every tick. Indexing by fd makes lookup a single load and
// leaves the global lock out of the send path entirely.
//
// Threading: unlike the packet limiter, this genuinely needs per-slot locks.
// Broadcast threads (combat, npc, projectile) write to connections they do not
// own, so a slot can be touched by several threads at once.
// ============================================================================

#include "connection_io.h"
#include "log.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>

#define CONNECTION_IO_MAX_QUEUED_BYTES (1024U * 1024U)
#define CONNECTION_IO_MIN_SLOTS        1024
#define CONNECTION_IO_MAX_SLOTS        65536   // see packet_limiter.c on why this is capped

typedef struct PendingWrite {
    struct PendingWrite* next;
    size_t len;
    size_t offset;
    uint8_t data[];
} PendingWrite;

typedef struct {
    pthread_mutex_t lock;
    int active;
    int failed;
    size_t queued_bytes;
    PendingWrite* head;
    PendingWrite* tail;
} ConnectionOutput;

static ConnectionOutput* g_outputs = NULL;
static int g_slot_count = 0;

// Resolve an fd to its slot. No global lock: the table is allocated once at
// startup and never moves, so only the slot itself needs protecting.
static ConnectionOutput* slot_for(int fd) {
    if (!g_outputs || fd < 0 || fd >= g_slot_count) return NULL;
    return &g_outputs[fd];
}

static void clear_queue(ConnectionOutput* output) {
    PendingWrite* write = output->head;
    while (write) {
        PendingWrite* next = write->next;
        free(write);
        write = next;
    }
    output->head = NULL;
    output->tail = NULL;
    output->queued_bytes = 0;
}

static int append_locked(ConnectionOutput* output, const void* data, size_t len) {
    if (len == 0) return 1;
    if (len > CONNECTION_IO_MAX_QUEUED_BYTES - output->queued_bytes) {
        output->failed = 1;
        return 0;
    }

    PendingWrite* write = malloc(sizeof(*write) + len);
    if (!write) {
        output->failed = 1;
        return 0;
    }
    write->next = NULL;
    write->len = len;
    write->offset = 0;
    memcpy(write->data, data, len);

    if (output->tail) output->tail->next = write;
    else output->head = write;
    output->tail = write;
    output->queued_bytes += len;
    return 1;
}

void connection_io_init(void) {
    if (g_outputs) return;

    // Sized from the descriptor limit for the same reason as the packet
    // limiter: every fd the process can hold must have a slot.
    struct rlimit rl;
    long wanted = CONNECTION_IO_MIN_SLOTS;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY)
        wanted = (long)rl.rlim_cur;
    else
        wanted = CONNECTION_IO_MAX_SLOTS;

    if (wanted < CONNECTION_IO_MIN_SLOTS) wanted = CONNECTION_IO_MIN_SLOTS;
    if (wanted > CONNECTION_IO_MAX_SLOTS) wanted = CONNECTION_IO_MAX_SLOTS;

    g_outputs = calloc((size_t)wanted, sizeof(ConnectionOutput));
    if (!g_outputs) {
        LOG_ERROR("[NET] could not allocate %ld connection slots", wanted);
        return;
    }
    for (long i = 0; i < wanted; i++)
        pthread_mutex_init(&g_outputs[i].lock, NULL);

    g_slot_count = (int)wanted;
    LOG_INFO("[NET] connection_io ready: %d slots", g_slot_count);
}

void connection_io_shutdown(void) {
    if (!g_outputs) return;
    for (int i = 0; i < g_slot_count; i++) {
        pthread_mutex_lock(&g_outputs[i].lock);
        clear_queue(&g_outputs[i]);
        g_outputs[i].active = 0;
        pthread_mutex_unlock(&g_outputs[i].lock);
    }
}

int connection_io_register(int fd) {
    ConnectionOutput* output = slot_for(fd);
    if (!output) {
        LOG_ERROR("[NET] fd %d outside connection table (%d)", fd, g_slot_count);
        return 0;
    }

    pthread_mutex_lock(&output->lock);
    // A recycled descriptor must never inherit the previous connection's
    // backlog, so the queue is cleared on the way in as well as out.
    clear_queue(output);
    output->active = 1;
    output->failed = 0;
    pthread_mutex_unlock(&output->lock);
    return 1;
}

void connection_io_unregister(int fd) {
    ConnectionOutput* output = slot_for(fd);
    if (!output) return;

    pthread_mutex_lock(&output->lock);
    output->active = 0;
    output->failed = 0;
    clear_queue(output);
    pthread_mutex_unlock(&output->lock);
}

ssize_t connection_io_send(int fd, const void* data, size_t len) {
    ConnectionOutput* output = slot_for(fd);
    if (!output) return -2;

    pthread_mutex_lock(&output->lock);

    if (!output->active) {
        pthread_mutex_unlock(&output->lock);
        return -2;
    }
    if (output->failed) {
        pthread_mutex_unlock(&output->lock);
        return -1;
    }

    // Anything already queued must go first, or packets arrive out of order.
    if (output->head) {
        int accepted = append_locked(output, data, len);
        pthread_mutex_unlock(&output->lock);
        return accepted ? (ssize_t)len : -1;
    }

    ssize_t sent = send(fd, data, len, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (sent == (ssize_t)len) {
        pthread_mutex_unlock(&output->lock);
        return sent;
    }
    if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        LOG_ERROR("[NET] queued send failed on fd %d: %s", fd, strerror(errno));
        output->failed = 1;
        pthread_mutex_unlock(&output->lock);
        return -1;
    }

    size_t offset = sent > 0 ? (size_t)sent : 0;
    int accepted = append_locked(output, (const uint8_t*)data + offset, len - offset);
    pthread_mutex_unlock(&output->lock);
    return accepted ? (ssize_t)len : -1;
}

int connection_io_flush(int fd) {
    ConnectionOutput* output = slot_for(fd);
    if (!output) return -1;

    pthread_mutex_lock(&output->lock);
    if (!output->active) {
        pthread_mutex_unlock(&output->lock);
        return -1;
    }

    while (output->head && !output->failed) {
        PendingWrite* write = output->head;
        size_t remaining = write->len - write->offset;
        ssize_t sent = send(fd, write->data + write->offset, remaining,
                            MSG_NOSIGNAL | MSG_DONTWAIT);
        if (sent > 0) {
            write->offset += (size_t)sent;
            output->queued_bytes -= (size_t)sent;
            if (write->offset == write->len) {
                output->head = write->next;
                if (!output->head) output->tail = NULL;
                free(write);
            }
            continue;
        }
        if (sent < 0 && errno == EINTR) continue;
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (sent < 0) {
            LOG_ERROR("[NET] queued flush failed on fd %d: %s", fd, strerror(errno));
        }
        output->failed = 1;
    }

    int ok = !output->failed;
    pthread_mutex_unlock(&output->lock);
    return ok ? 0 : -1;
}

int connection_io_has_pending(int fd) {
    ConnectionOutput* output = slot_for(fd);
    if (!output) return 0;

    pthread_mutex_lock(&output->lock);
    int pending = output->active && output->head != NULL;
    pthread_mutex_unlock(&output->lock);
    return pending;
}

int connection_io_failed(int fd) {
    ConnectionOutput* output = slot_for(fd);
    if (!output) return 0;

    pthread_mutex_lock(&output->lock);
    int failed = output->active && output->failed;
    pthread_mutex_unlock(&output->lock);
    return failed;
}
