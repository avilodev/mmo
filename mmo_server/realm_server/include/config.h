#ifndef CONFIG_H
#define CONFIG_H

#include "types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ctype.h>

/** Hold realm listener state and the synchronized world-server registry. */
typedef struct {
    char name[32];
    int tcp_sockfd;
    int port;
    int running;
    pthread_t accept_thread;
    
    pthread_t world_monitor_thread;
    WorldServer world_servers[MAX_WORLDS];
    int num_world_servers;
    pthread_mutex_t world_servers_lock;
} ServerConfig;

/** Track the realm's current aggregate player count. */
typedef struct {
    int current_players; 
} ServerState;

extern ServerConfig g_server;
extern ServerState g_state;

void signal_handler(int signum);
void setup_signals(void);
int create_tcp_server_socket(int port);
int set_config(const char* filepath);

#endif