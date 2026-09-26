/**
 * @file
 * Configure login-server signals and its TCP listening socket.
 */
#include "config.h"
#include "log.h"

/** Record which signal asked for the shutdown, for the main thread to report. */
volatile sig_atomic_t g_shutdown_signal = 0;

/** Request shutdown. Flag writes only.
 *
 * This used to call LOG_INFO(), which reaches vsnprintf(), a mutex and
 * write(2) -- none of them async-signal-safe. A signal arriving while any
 * thread held the log's lock deadlocked the handler on a non-recursive mutex
 * its own thread already owned: the handler never returned, so running was
 * never cleared and the main loop span forever, and the log stayed locked for
 * every other thread too. Under load that is likely rather than exotic, which
 * is why Ctrl+C left this process alive for the supervisor to force-kill while
 * the world server -- whose handler was already reduced to flag writes --
 * exited cleanly. The main loop reports the signal once it wakes.
 */
void signal_handler(int sig) {
    g_shutdown_signal = sig;
    g_server.running = 0;
}

/** Install shutdown handlers and ignore broken-pipe signals. */
void setup_signals(void) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);
}

/**
 * Create, bind, and listen on a reusable IPv4 TCP socket.
 *
 * @return      The listening descriptor, or -1 when socket setup fails.
 */
int create_tcp_server_socket(int port) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return -1;
    }
    
    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);
    
    if (bind(sockfd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("bind");
        close(sockfd);
        return -1;
    }
    
    if (listen(sockfd, MAX_PENDING_CONNECTIONS) < 0) {
        perror("listen");
        close(sockfd);
        return -1;
    }
    
    return sockfd;
}
