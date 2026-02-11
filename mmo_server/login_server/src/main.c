#include "types.h"
#include "session.h"
#include "auth.h"
#include "users_database.h"
#include "config.h"
#include "routes.h"

#include <stdlib.h>
#include <errno.h>
#include <poll.h>
#include <netinet/tcp.h>
 
ServerConfig g_server;

void* client_handler_thread(void* arg) {
    int client_fd = *(int*)arg;
    free(arg);

    printf("Login client connected: fd %d\n", client_fd);
    
    // Read packet header first to determine type
    uint8_t buffer[4096]; 
    ssize_t bytes = recv(client_fd, buffer, sizeof(buffer), 0);
    
    if (bytes > 0) {
        printf("Received %zd bytes from client\n", bytes);
        
        // Route based on packet type
        if (bytes >= MIN_HEADER_SIZE) {
            PacketHeader* header = (PacketHeader*)buffer;
            printf("Packet type: %d\n", header->type);
            
            // Route to appropriate handler
            route_packet(client_fd, buffer, bytes);
        } else {
            printf("Packet too small: %zd bytes\n", bytes);
        }
    } else {
        printf("recv failed or connection closed: %zd\n", bytes);
    }
    
    close(client_fd);
    printf("Login client disconnected: fd %d\n", client_fd);
    return NULL;
}

void* accept_thread_func(void* arg) {
    (void)arg;
    
    printf("Accept thread started\n");
    
    struct pollfd pfd;
    pfd.fd = g_server.tcp_sockfd;
    pfd.events = POLLIN;
    
    while (g_server.running) {
        int ret = poll(&pfd, 1, 1000);
        
        if (ret < 0) {
            if (errno == EINTR) continue;
            printf("poll error: %s\n", strerror(errno));
            break;
        }
        
        if (ret == 0) continue;
        if (!g_server.running) break;
        
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        
        int client_fd = accept(g_server.tcp_sockfd, (struct sockaddr*)&client_addr, &addr_len);
        
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            if (errno == EBADF) break;
            printf("accept failed: %s\n", strerror(errno));
            continue;
        }
        
        printf("New login from %s:%d (fd: %d)\n", 
               inet_ntoa(client_addr.sin_addr), 
               ntohs(client_addr.sin_port), 
               client_fd);
        
        pthread_t thread;
        int* client_fd_ptr = malloc(sizeof(int));
        *client_fd_ptr = client_fd;
        
        if (pthread_create(&thread, NULL, client_handler_thread, client_fd_ptr) != 0) {
            printf("Failed to create handler thread\n");
            close(client_fd);
            free(client_fd_ptr);
            continue;
        }
        
        pthread_detach(thread);
        
        session_cleanup_expired();
    }
    
    printf("Accept thread exiting\n");
    return NULL;
}

int main(int argc, char** argv) {
    printf("=== LOGIN SERVER (Two-Stage Auth) ===\n");
    printf("PID: %d\n", getpid());
    
    // Initialize database
    if (!db_init(USERS_DB)) {
        printf("Failed to initialize database\n");
        return 1;
    }
    
    if (!session_init()) {
        printf("Failed to initialize session system\n");
        printf("Make sure Redis is running: redis-cli PING\n");
        db_close();
        return 1;
    }
    printf("✓ Session system initialized\n");
    printf("✓ Two-stage authentication enabled\n");
    printf("  - PACKET_AUTH_LOGIN (3) = Validate credentials\n");
    printf("  - PACKET_START_GAME_REQUEST (6) = Create session\n");
    
    memset(&g_server, 0, sizeof(g_server));
    g_server.port = LOGIN_SERVER_PORT;
    g_server.running = 1;
    
    if (argc > 1) {
        g_server.port = atoi(argv[1]); 
        if (g_server.port <= 0) {
            printf("Invalid port: %s\n", argv[1]);
            db_close();
            return 1;
        }
    }
    
    setup_signals();
    
    g_server.tcp_sockfd = create_tcp_server_socket(g_server.port);
    if (g_server.tcp_sockfd < 0) {
        printf("Failed to create server socket\n");
        db_close();
        return 1;
    }
    
    if (pthread_create(&g_server.accept_thread, NULL, accept_thread_func, NULL) != 0) {
        printf("Failed to create accept thread\n");
        close(g_server.tcp_sockfd);
        db_close();
        return 1;
    }
    
    printf("Login Server running on port %d. Press Ctrl+C to stop.\n", g_server.port);
    
    while (g_server.running) {
        sleep(1);
    }
    
    printf("\nShutting down...\n");
    close(g_server.tcp_sockfd);
    pthread_join(g_server.accept_thread, NULL);
    
    session_close();
    db_close();
    printf("Login Server stopped\n");
    
    return 0;
}