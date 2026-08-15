#include "config.h"

void signal_handler(int sig) {
    (void)sig;
    printf("\nReceived shutdown signal\n");
    g_server.running = 0;
}

void setup_signals(void) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);
}

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
