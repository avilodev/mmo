/**
 * @file
 * Check the /healthz and /metrics endpoint over a real socket.
 *
 * The endpoint exists so something outside the process can answer "is this
 * service healthy" without a human reading a log, so what is tested here is
 * what such a caller depends on: the status codes, the Prometheus text shape,
 * and that the single serving thread goes back to accepting after each
 * connection rather than answering once and stopping.
 */
#include "metrics_server.h"
#include "log.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int g_healthy = 1;

static size_t provider(char* out, size_t size, void* user) {
    (void)user;
    size_t used = 0;
    metrics_write(out, size, &used, "mmo_test_value", "A test value", "gauge", 42.0);
    metrics_write(out, size, &used, "mmo_test_other", "Another", "counter", 7.5);
    return used;
}

static int health(char* reason, size_t size, void* user) {
    (void)user;
    if (g_healthy) return 1;
    snprintf(reason, size, "deliberately sick");
    return 0;
}

static int scrape(int port, const char* path, char* out, size_t out_size) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(fd, (struct sockaddr*)&a, sizeof(a)) != 0) { close(fd); return 0; }

    char req[256];
    int n = snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: x\r\n\r\n", path);
    if (write(fd, req, (size_t)n) != n) { close(fd); return 0; }

    size_t used = 0;
    ssize_t got;
    while (used + 1 < out_size && (got = read(fd, out + used, out_size - used - 1)) > 0)
        used += (size_t)got;
    out[used] = '\0';
    close(fd);
    return 1;
}

int main(void) {
    log_init();
    printf("=== metrics endpoint ===\n");
    const int port = 19731;
    if (!metrics_server_start("test", "127.0.0.1", port, provider, health, NULL)) {
        printf("FAIL: could not start\n");
        return 1;
    }

    char buf[8192];
    int failures = 0;
    #define CHECK(c, what) do { if (c) printf("  ok   %s\n", what); \
        else { printf("  FAIL %s\n", what); failures++; } } while (0)

    CHECK(scrape(port, "/metrics", buf, sizeof(buf)), "the endpoint answers /metrics");
    CHECK(strstr(buf, "200 OK") != NULL, "with 200");
    CHECK(strstr(buf, "mmo_test_value 42") != NULL, "and the provider's value");
    CHECK(strstr(buf, "# TYPE mmo_test_other counter") != NULL, "and its TYPE line");

    CHECK(scrape(port, "/healthz", buf, sizeof(buf)), "the endpoint answers /healthz");
    CHECK(strstr(buf, "200 OK") != NULL, "healthy is 200");

    g_healthy = 0;
    CHECK(scrape(port, "/healthz", buf, sizeof(buf)), "and answers when unhealthy");
    CHECK(strstr(buf, "503") != NULL, "unhealthy is 503");
    CHECK(strstr(buf, "deliberately sick") != NULL, "with the reason in the body");

    CHECK(scrape(port, "/wat", buf, sizeof(buf)), "an unknown path answers");
    CHECK(strstr(buf, "404") != NULL, "with 404");

    /* Successive scrapes on one endpoint: the thread must go back to accepting. */
    g_healthy = 1;
    int all = 1;
    for (int i = 0; i < 20; i++) {
        if (!scrape(port, "/metrics", buf, sizeof(buf)) || !strstr(buf, "mmo_test_value")) all = 0;
    }
    CHECK(all, "twenty consecutive scrapes all answer");

    metrics_server_stop();
    if (failures) { printf("\n%d FAILED\n", failures); return 1; }
    printf("\nAll metrics endpoint checks passed\n");
    return 0;
}
