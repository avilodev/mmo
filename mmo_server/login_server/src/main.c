#include "types.h"
#include "session.h"
#include "auth.h"
#include "users_database.h"
#include "config.h"
#include "routes.h"
#include "patch_notes.h"
#include "tls.h"

#include <stdlib.h>
#include <errno.h>
#include <poll.h>
#include <netinet/tcp.h>
 
ServerConfig g_server;
SSL_CTX* g_tls_ctx = NULL;

void* client_handler_thread(void* arg) {
    int client_fd = *(int*)arg;
    free(arg);

    printf("Login client connected: fd %d\n", client_fd);

    // 30-second recv timeout — applies to both the TLS handshake and
    // subsequent reads via the underlying socket (#6)
    struct timeval tv = {.tv_sec = 30, .tv_usec = 0};
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // TLS handshake
    SSL* ssl = tls_accept(g_tls_ctx, client_fd);
    if (!ssl) {
        close(client_fd);
        return NULL;
    }
    tls_set_conn(ssl);

    // Reassembly buffer: accumulate until we have a full packet (#4)
    uint8_t buffer[4096];
    ssize_t buf_len = 0;

    while (1) {
        ssize_t bytes = tls_recv(client_fd, buffer + buf_len,
                                 sizeof(buffer) - (size_t)buf_len, 0);
        if (bytes <= 0) {
            if (bytes == 0)
                printf("Login client fd %d disconnected\n", client_fd);
            else
                printf("Login client fd %d recv error/timeout\n", client_fd);
            break;
        }
        buf_len += bytes;

        // Wait until we have at least the fixed header
        if (buf_len < MIN_HEADER_SIZE)
            continue;

        PacketHeader* header = (PacketHeader*)buffer;
        ssize_t expected = MIN_HEADER_SIZE + (ssize_t)ntohs(header->payload_size);

        if (buf_len < expected)
            continue;  // Wait for the rest of the payload

        printf("Received %zd bytes from login client fd %d, packet type %d\n",
               buf_len, client_fd, header->type);

        route_packet(client_fd, buffer, expected);
        break;  // Login server handles exactly one packet per connection
    }

    tls_close(ssl);
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
    
    patch_notes_init();

    // Initialize TLS — cert/key relative to server working directory
    g_tls_ctx = tls_server_init("./certs/server.crt", "./certs/server.key");
    if (!g_tls_ctx) {
        printf("Failed to initialize TLS context.\n");
        printf("Generate a self-signed cert with:\n");
        printf("  mkdir -p certs && openssl req -x509 -newkey rsa:2048 \\\n");
        printf("    -keyout certs/server.key -out certs/server.crt \\\n");
        printf("    -days 3650 -nodes -subj \"/CN=mmo-login\"\n");
        return 1;
    }

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
    tls_server_cleanup(g_tls_ctx);
    printf("Login Server stopped\n");

    return 0;
}