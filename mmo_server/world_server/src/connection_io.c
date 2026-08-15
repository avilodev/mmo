#include "connection_io.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#define CONNECTION_IO_SLOTS 1000
#define CONNECTION_IO_MAX_QUEUED_BYTES (1024U * 1024U)

typedef struct PendingWrite {
    struct PendingWrite* next;
    size_t len;
    size_t offset;
    uint8_t data[];
} PendingWrite;

typedef struct {
    pthread_mutex_t lock;
    int fd;
    int active;
    int failed;
    size_t queued_bytes;
    PendingWrite* head;
    PendingWrite* tail;
} ConnectionOutput;

static ConnectionOutput g_outputs[CONNECTION_IO_SLOTS];
static pthread_mutex_t g_outputs_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_initialized = 0;

static ConnectionOutput* find_locked(int fd) {
    for (int i = 0; i < CONNECTION_IO_SLOTS; i++) {
        if (g_outputs[i].active && g_outputs[i].fd == fd) return &g_outputs[i];
    }
    return NULL;
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
    pthread_mutex_lock(&g_outputs_lock);
    if (!g_initialized) {
        memset(g_outputs, 0, sizeof(g_outputs));
        for (int i = 0; i < CONNECTION_IO_SLOTS; i++) {
            pthread_mutex_init(&g_outputs[i].lock, NULL);
            g_outputs[i].fd = -1;
        }
        g_initialized = 1;
    }
    pthread_mutex_unlock(&g_outputs_lock);
}

void connection_io_shutdown(void) {
    pthread_mutex_lock(&g_outputs_lock);
    if (!g_initialized) {
        pthread_mutex_unlock(&g_outputs_lock);
        return;
    }
    for (int i = 0; i < CONNECTION_IO_SLOTS; i++) {
        pthread_mutex_lock(&g_outputs[i].lock);
        clear_queue(&g_outputs[i]);
        g_outputs[i].active = 0;
        g_outputs[i].fd = -1;
        pthread_mutex_unlock(&g_outputs[i].lock);
    }
    pthread_mutex_unlock(&g_outputs_lock);
}

int connection_io_register(int fd) {
    pthread_mutex_lock(&g_outputs_lock);
    if (find_locked(fd)) {
        pthread_mutex_unlock(&g_outputs_lock);
        return 1;
    }
    for (int i = 0; i < CONNECTION_IO_SLOTS; i++) {
        ConnectionOutput* output = &g_outputs[i];
        if (!output->active) {
            pthread_mutex_lock(&output->lock);
            clear_queue(output);
            output->fd = fd;
            output->active = 1;
            output->failed = 0;
            pthread_mutex_unlock(&output->lock);
            pthread_mutex_unlock(&g_outputs_lock);
            return 1;
        }
    }
    pthread_mutex_unlock(&g_outputs_lock);
    return 0;
}

void connection_io_unregister(int fd) {
    pthread_mutex_lock(&g_outputs_lock);
    ConnectionOutput* output = find_locked(fd);
    if (output) {
        pthread_mutex_lock(&output->lock);
        output->active = 0;
        output->fd = -1;
        output->failed = 0;
        clear_queue(output);
        pthread_mutex_unlock(&output->lock);
    }
    pthread_mutex_unlock(&g_outputs_lock);
}

ssize_t connection_io_send(int fd, const void* data, size_t len) {
    pthread_mutex_lock(&g_outputs_lock);
    ConnectionOutput* output = find_locked(fd);
    if (!output) {
        pthread_mutex_unlock(&g_outputs_lock);
        return -2;
    }
    pthread_mutex_lock(&output->lock);
    pthread_mutex_unlock(&g_outputs_lock);

    if (output->failed) {
        pthread_mutex_unlock(&output->lock);
        return -1;
    }

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
        fprintf(stderr, "[NET] queued send failed on fd %d: %s\n", fd, strerror(errno));
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
    pthread_mutex_lock(&g_outputs_lock);
    ConnectionOutput* output = find_locked(fd);
    if (!output) {
        pthread_mutex_unlock(&g_outputs_lock);
        return -1;
    }
    pthread_mutex_lock(&output->lock);
    pthread_mutex_unlock(&g_outputs_lock);

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
            fprintf(stderr, "[NET] queued flush failed on fd %d: %s\n", fd, strerror(errno));
        }
        output->failed = 1;
    }

    int ok = !output->failed;
    pthread_mutex_unlock(&output->lock);
    return ok ? 0 : -1;
}

int connection_io_has_pending(int fd) {
    pthread_mutex_lock(&g_outputs_lock);
    ConnectionOutput* output = find_locked(fd);
    if (!output) {
        pthread_mutex_unlock(&g_outputs_lock);
        return 0;
    }
    pthread_mutex_lock(&output->lock);
    pthread_mutex_unlock(&g_outputs_lock);
    int pending = output->head != NULL;
    pthread_mutex_unlock(&output->lock);
    return pending;
}

int connection_io_failed(int fd) {
    pthread_mutex_lock(&g_outputs_lock);
    ConnectionOutput* output = find_locked(fd);
    if (!output) {
        pthread_mutex_unlock(&g_outputs_lock);
        return 0;
    }
    pthread_mutex_lock(&output->lock);
    pthread_mutex_unlock(&g_outputs_lock);
    int failed = output->failed;
    pthread_mutex_unlock(&output->lock);
    return failed;
}
