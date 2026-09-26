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

/** Signal number that requested shutdown; 0 until one arrives.
 *
 * Written by the handler, which does flag writes only -- see config.c.
 * The main loop reports it once it wakes. */
extern volatile sig_atomic_t g_shutdown_signal;

void signal_handler(int sig);
void setup_signals(void);
int create_tcp_server_socket(int port);

#endif
