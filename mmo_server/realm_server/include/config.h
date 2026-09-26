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

    /** One entry per configured world, allocated from the world table.
     *
     * Heap rather than a fixed array: the roster comes from worlds.conf at
     * startup, so how many worlds exist is a deployment decision. The array is
     * allocated once, before the monitor thread publishes anything into it,
     * and freed at shutdown; only its contents change under the lock.
     */
    WorldServer* world_servers;
    int num_world_servers;
    pthread_mutex_t world_servers_lock;
} ServerConfig;

/** Track the realm's current aggregate player count. */
typedef struct {
    int current_players; 
} ServerState;

extern ServerConfig g_server;
extern ServerState g_state;

/** Signal number that requested shutdown; 0 until one arrives.
 *
 * Written by the handler, which does flag writes only -- see config.c.
 * The main loop reports it once it wakes. */
extern volatile sig_atomic_t g_shutdown_signal;

void signal_handler(int signum);
void setup_signals(void);
int create_tcp_server_socket(int port);
int set_config(const char* filepath);

#endif