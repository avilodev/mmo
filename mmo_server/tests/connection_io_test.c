#include "connection_io.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define PACKET_COUNT 256
#define PACKET_SIZE 1024

int main(void) {
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        perror("socketpair");
        return 1;
    }

    int send_buffer = 1024;
    setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF, &send_buffer, sizeof(send_buffer));

    connection_io_init();
    if (!connection_io_register(sockets[0])) return 1;

    uint8_t packet[PACKET_SIZE];
    int saw_pending = 0;
    for (uint32_t sequence = 0; sequence < PACKET_COUNT; sequence++) {
        memset(packet, (int)(sequence & 0xff), sizeof(packet));
        memcpy(packet, &sequence, sizeof(sequence));
        ssize_t accepted = connection_io_send(sockets[0], packet, sizeof(packet));
        if (accepted != (ssize_t)sizeof(packet)) {
            fprintf(stderr, "send/queue failed at packet %u (result=%zd)\n",
                    sequence, accepted);
            return 1;
        }
        if (connection_io_has_pending(sockets[0])) saw_pending = 1;
    }

    if (!saw_pending) {
        fprintf(stderr, "test did not force a partial write\n");
        return 1;
    }

    size_t expected_bytes = (size_t)PACKET_COUNT * PACKET_SIZE;
    uint8_t* received = malloc(expected_bytes);
    if (!received) return 1;
    size_t received_bytes = 0;

    while (received_bytes < expected_bytes) {
        if (connection_io_flush(sockets[0]) != 0) {
            fprintf(stderr, "flush failed\n");
            return 1;
        }
        ssize_t count = recv(sockets[1], received + received_bytes,
                             expected_bytes - received_bytes, MSG_DONTWAIT);
        if (count > 0) received_bytes += (size_t)count;
        else if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            perror("recv");
            return 1;
        }
    }

    for (uint32_t sequence = 0; sequence < PACKET_COUNT; sequence++) {
        uint8_t* actual = received + (size_t)sequence * PACKET_SIZE;
        uint32_t actual_sequence = 0;
        memcpy(&actual_sequence, actual, sizeof(actual_sequence));
        if (actual_sequence != sequence) {
            fprintf(stderr, "packet order mismatch: expected %u, got %u\n",
                    sequence, actual_sequence);
            return 1;
        }
        for (size_t i = sizeof(actual_sequence); i < PACKET_SIZE; i++) {
            if (actual[i] != (uint8_t)(sequence & 0xff)) {
                fprintf(stderr, "packet %u corrupted at byte %zu\n", sequence, i);
                return 1;
            }
        }
    }

    free(received);
    connection_io_unregister(sockets[0]);
    connection_io_shutdown();
    close(sockets[0]);
    close(sockets[1]);
    puts("connection_io_test: PASS");
    return 0;
}
