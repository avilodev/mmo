#ifndef CONFIG_H
#define CONFIG_H

#include "types.h"
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <pthread.h>
#include <math.h>

/** Hold the login server socket, lifecycle flag, and accept thread. */
typedef struct {
    int tcp_sockfd;
    int port;
    int running;
    pthread_t accept_thread;
} ServerConfig;

extern ServerConfig g_server;

void signal_handler(int sig);
void setup_signals(void);
int create_tcp_server_socket(int port);

#endif
