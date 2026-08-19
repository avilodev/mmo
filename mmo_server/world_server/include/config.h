#ifndef CONFIG_H
#define CONFIG_H

#include "types.h"
#include "packet_limiter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <ctype.h>
#include <signal.h>
#include <stdatomic.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/** Hold world-server identity, listener state, and packet-limit overrides. */
typedef struct {
    int tcp_sockfd;
    int running;
    pthread_t accept_thread;

    char server_name[32];
    char region[32];
    char ip[16];
    uint16_t port;
    uint16_t max_players;
    bool hardcore;

    /** Retain compiled packet budgets for override fields left at zero. */
    PacketLimitOverrides limits;
} ServerConfig;

/** Track aggregate world state shared by client-handler threads. */
typedef struct {
    _Atomic int current_players;  /**< Updated concurrently by client-handler threads. */

} ServerState;

extern ServerConfig g_server;
extern ServerState g_state;

int create_tcp_server_socket(int port);
int set_config(const char* filepath);

#endif