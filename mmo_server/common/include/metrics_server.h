#ifndef METRICS_SERVER_H
#define METRICS_SERVER_H

/** @file A small HTTP endpoint every service can expose its own numbers on.
 *
 * Until this existed the only quantitative signal any service produced was the
 * world's `[TICK]` line, printed to a log every ten seconds. Nothing could
 * answer "is this world healthy right now" without a human reading text, and
 * nothing outside the process could answer it at all -- no load balancer, no
 * restart policy, no alert, no dashboard.
 *
 * Two routes:
 *
 *   GET /healthz   200 with "ok" while the service considers itself healthy,
 *                  503 with a one-line reason when it does not. For liveness
 *                  and readiness probes, which want a status code and nothing
 *                  else.
 *
 *   GET /metrics   200 with Prometheus text exposition. For everything that
 *                  wants numbers over time.
 *
 * Deliberately not a general web server. One thread, one connection at a time,
 * a fixed request ceiling, no keep-alive, no request body, and only those two
 * paths. It exists to be scraped by something on the same private network, and
 * it defaults to binding loopback so that is the only thing that can reach it.
 *
 * The provider callback is how common/ stays ignorant of what a service
 * measures: each service hands over a function that writes its own text, and
 * this file knows nothing but how to serve it.
 */

#include <stddef.h>

/**
 * Write a service's current metrics into `out`.
 *
 * Called on the metrics thread, once per scrape. It must not block on anything
 * a game thread holds for long: read counters, snapshot them, and return.
 *
 * @param out       Buffer to write Prometheus text into.
 * @param out_size  Its capacity, including the terminator.
 * @param user      The pointer given to metrics_server_start().
 * @return          Bytes written, not counting the terminator.
 */
typedef size_t (*MetricsProvider)(char* out, size_t out_size, void* user);

/**
 * Report whether the service is healthy, and why not when it is not.
 *
 * @param reason      Buffer for a short one-line explanation on failure.
 * @param reason_size Its capacity.
 * @param user        The pointer given to metrics_server_start().
 * @return            Nonzero when healthy.
 */
typedef int (*HealthProvider)(char* reason, size_t reason_size, void* user);

/**
 * Start the endpoint on its own thread.
 *
 * @param service_name  Used in log lines and as the `service` metric label.
 * @param bind_addr     Address to bind, or NULL/"" for 127.0.0.1. Pass
 *                      "0.0.0.0" deliberately and only behind a firewall.
 * @param port          TCP port; 0 disables the endpoint entirely.
 * @param metrics       Metrics writer. Required.
 * @param health        Health check, or NULL to always report healthy.
 * @param user          Passed to both callbacks.
 * @return              1 when listening, 0 when disabled or unable to bind.
 */
int metrics_server_start(const char* service_name,
                         const char* bind_addr, int port,
                         MetricsProvider metrics, HealthProvider health,
                         void* user);

/** Stop the endpoint and join its thread. Safe when never started. */
void metrics_server_stop(void);

/**
 * Resolve the port from the environment.
 *
 * Reads `$MMO_METRICS_PORT`. When it is set, the service's own offset is added
 * to it so one variable configures the whole stack without three ports having
 * to be chosen by hand: the login server takes offset 0, the realm 1, and each
 * world its world id. Unset or zero disables the endpoint.
 *
 * @param offset  Added to the configured base port.
 * @return        The port to listen on, or 0 when disabled.
 */
int metrics_port_from_env(int offset);

/** Resolve the bind address from `$MMO_METRICS_BIND`, defaulting to loopback. */
const char* metrics_bind_from_env(void);

/* --- Helpers for writing the text ---------------------------------------- */

/**
 * Append one Prometheus metric with its HELP and TYPE lines.
 *
 * Silently writes nothing when the buffer cannot hold the whole metric, so a
 * truncated scrape is never a half-written line a parser would reject.
 *
 * @param out     Buffer being built.
 * @param size    Its capacity.
 * @param used    In/out cursor; advanced by what was written.
 * @param name    Metric name, already prefixed.
 * @param help    One-line description.
 * @param type    "gauge" or "counter".
 * @param value   The value.
 */
void metrics_write(char* out, size_t size, size_t* used,
                   const char* name, const char* help, const char* type,
                   double value);

#endif // METRICS_SERVER_H
