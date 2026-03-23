#ifndef CONFIG_H
#define CONFIG_H

#include "types.h"

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
} ServerConfig;

typedef struct {
    _Atomic int current_players;  // Incremented/decremented by client handler threads concurrently

    //ticks....
} ServerState;

extern ServerConfig g_server;
extern ServerState g_state;

int create_tcp_server_socket(int port);
int set_config(const char* filepath);

#endif