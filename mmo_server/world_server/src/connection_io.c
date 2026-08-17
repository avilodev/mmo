/**
 * @file
 * Queue nonblocking world-server writes in descriptor-indexed connection slots.
 */

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

/** Maximum unsent data retained for one connection. */
#define CONNECTION_IO_MAX_QUEUED_BYTES (1024U * 1024U)
#define CONNECTION_IO_MIN_SLOTS        1024
#define CONNECTION_IO_MAX_SLOTS        65536   // see packet_limiter.c on why this is capped

/** Own one queued write and its current transmission offset. */
typedef struct PendingWrite {
    struct PendingWrite* next;
    size_t len;
    size_t offset;
    uint8_t data[];
} PendingWrite;

/** Synchronize the outbound state associated with one descriptor. */
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

/** Free every pending write and reset queue accounting while the slot is locked. */
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

/**
 * Copy a write onto a locked connection queue within its byte limit.
 *
 * @return      Nonzero when the data is queued, otherwise zero and the slot is marked failed.
 */
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

/** Allocate and initialize output slots according to the descriptor limit. */
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

/** Clear all output queues and deactivate their slots. */
void connection_io_shutdown(void) {
    if (!g_outputs) return;
    for (int i = 0; i < g_slot_count; i++) {
        pthread_mutex_lock(&g_outputs[i].lock);
        clear_queue(&g_outputs[i]);
        g_outputs[i].active = 0;
        pthread_mutex_unlock(&g_outputs[i].lock);
    }
}

/**
 * Activate a descriptor slot after clearing any recycled backlog.
 *
 * @return      Nonzero on success, otherwise zero for an unavailable slot.
 */
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

/** Deactivate a descriptor slot and release its queued writes. */
void connection_io_unregister(int fd) {
    ConnectionOutput* output = slot_for(fd);
    if (!output) return;

    pthread_mutex_lock(&output->lock);
    output->active = 0;
    output->failed = 0;
    clear_queue(output);
    pthread_mutex_unlock(&output->lock);
}

/**
 * Send a complete framed packet immediately or queue its unsent suffix.
 *
 * @return      len when accepted, -1 for a socket or queue failure, or -2 for an unregistered descriptor.
 */
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

/**
 * Drain queued output until complete, blocked, or failed.
 *
 * @return      Zero when the slot remains usable, otherwise -1.
 */
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

/**
 * Report whether an active descriptor has queued output.
 *
 * @return      Nonzero when output is pending, otherwise zero.
 */
int connection_io_has_pending(int fd) {
    ConnectionOutput* output = slot_for(fd);
    if (!output) return 0;

    pthread_mutex_lock(&output->lock);
    int pending = output->active && output->head != NULL;
    pthread_mutex_unlock(&output->lock);
    return pending;
}

/**
 * Report whether an active descriptor's output path has failed.
 *
 * @return      Nonzero when failed, otherwise zero.
 */
int connection_io_failed(int fd) {
    ConnectionOutput* output = slot_for(fd);
    if (!output) return 0;

    pthread_mutex_lock(&output->lock);
    int failed = output->active && output->failed;
    pthread_mutex_unlock(&output->lock);
    return failed;
}
