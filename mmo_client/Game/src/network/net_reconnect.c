/**
 * @file
 * Recover a dropped connection without ending the session.
 */

#include "net_internal.h"
#include "net_connect.h"
#include "net_reconnect.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** First wait after a drop, in seconds. */
#define BACKOFF_FIRST_SECONDS 1.0

/** Ceiling on the wait between attempts.
 *
 * The old code retried on a flat five-second timer, which is both too eager
 * for a server that is restarting and too patient for a momentary blip. This
 * doubles from one second up to here, so a blip is recovered almost at once
 * and a long outage is not hammered.
 */
#define BACKOFF_MAX_SECONDS 30.0

/** Attempts before the player is told to deal with it themselves.
 *
 * Not unlimited: a client that reconnects forever against a world that has
 * been taken down is a client sitting on a frozen scene pretending otherwise.
 */
#define MAX_ATTEMPTS 8

/** Seconds to wait for the realm's enter-world answer before retrying. */
#define TICKET_TIMEOUT_SECONDS 10.0

static struct {
    ReconnectPhase phase;

    /* What a live session was, remembered while it was live. */
    int      have_session;
    /** Sized from GameState.realm_ip, which is what fills it. */
    char     realm_ip[16];
    uint16_t realm_port;
    uint32_t account_id;
    char     session_key_hex[65];
    uint32_t world_id;
    uint32_t character_id;

    int    attempts;
    double next_attempt_at;
    double backoff;
    double stage_deadline;

    char status[128];
} g_rc;

/** Decode the hexadecimal session key the launcher handed over. */
static void hex_to_binary(const char* hex, char* bin, size_t bin_size) {
    size_t hex_len = strlen(hex);
    size_t bytes   = hex_len / 2;
    if (bytes > bin_size) bytes = bin_size;

    for (size_t i = 0; i < bytes; i++) {
        char pair[3] = { hex[i * 2], hex[i * 2 + 1], '\0' };
        bin[i] = (char)strtol(pair, NULL, 16);
    }
}

/**
 * Remember what a live world session was, so it can be rebuilt.
 */
void net_reconnect_remember_world(const char* realm_ip, uint16_t realm_port,
                                  uint32_t account_id, const char* session_key_hex,
                                  uint32_t world_id, uint32_t character_id) {
    memset(&g_rc, 0, sizeof(g_rc));

    snprintf(g_rc.realm_ip, sizeof(g_rc.realm_ip), "%s", realm_ip ? realm_ip : "");
    snprintf(g_rc.session_key_hex, sizeof(g_rc.session_key_hex), "%s",
             session_key_hex ? session_key_hex : "");
    g_rc.realm_port   = realm_port;
    g_rc.account_id   = account_id;
    g_rc.world_id     = world_id;
    g_rc.character_id = character_id;

    /* Every field must be present: a half-remembered session produces a
     * recovery attempt that cannot succeed and a status line that says it is
     * trying. */
    g_rc.have_session = (g_rc.realm_ip[0] != '\0' &&
                         g_rc.session_key_hex[0] != '\0' &&
                         account_id != 0 && world_id != 0 && character_id != 0);
    g_rc.phase = RECONNECT_IDLE;

    if (!g_rc.have_session)
        NET_WARN("[RECONNECT] Session details incomplete; a dropped connection "
                 "will not be recoverable\n");
}

/** Forget the remembered session. */
void net_reconnect_forget(void) {
    memset(&g_rc, 0, sizeof(g_rc));
}

/** Schedule the next attempt and grow the backoff. */
static void schedule_retry(double now) {
    if (g_rc.backoff <= 0.0) g_rc.backoff = BACKOFF_FIRST_SECONDS;
    else                     g_rc.backoff *= 2.0;
    if (g_rc.backoff > BACKOFF_MAX_SECONDS) g_rc.backoff = BACKOFF_MAX_SECONDS;

    /* A little jitter, so a world that drops a thousand players at once does
     * not get all thousand back in the same millisecond. */
    double jitter = ((double)(rand() % 1000) / 1000.0) * 0.25 * g_rc.backoff;

    g_rc.next_attempt_at = now + g_rc.backoff + jitter;
    g_rc.phase = RECONNECT_WAITING;

    snprintf(g_rc.status, sizeof(g_rc.status),
             "Connection lost — retrying in %.0fs (attempt %d of %d)",
             g_rc.backoff + jitter, g_rc.attempts + 1, MAX_ATTEMPTS);
}

/** Report that the connection has dropped and recovery should begin. */
void net_reconnect_begin(double now) {
    if (!g_rc.have_session) {
        g_rc.phase = RECONNECT_GIVEN_UP;
        snprintf(g_rc.status, sizeof(g_rc.status),
                 "Connection lost. Please restart the client.");
        return;
    }
    if (g_rc.phase != RECONNECT_IDLE) return;   // already recovering

    g_rc.attempts = 0;
    g_rc.backoff  = 0.0;
    network_connect_abort();
    schedule_retry(now);
}

/** Give up, telling the player rather than looping silently. */
static void give_up(void) {
    g_rc.phase = RECONNECT_GIVEN_UP;
    snprintf(g_rc.status, sizeof(g_rc.status),
             "Could not reconnect after %d attempts. Please restart the client.",
             MAX_ATTEMPTS);
    NET_WARN("[RECONNECT] Giving up after %d attempts\n", MAX_ATTEMPTS);
}

/** Begin one attempt: back to the realm first, because the ticket is spent. */
static void start_attempt(double now) {
    g_rc.attempts++;
    if (g_rc.attempts > MAX_ATTEMPTS) { give_up(); return; }

    char binary_key[32] = {0};
    hex_to_binary(g_rc.session_key_hex, binary_key, sizeof(binary_key));

    snprintf(g_rc.status, sizeof(g_rc.status),
             "Reconnecting to the realm (attempt %d of %d)...",
             g_rc.attempts, MAX_ATTEMPTS);

    if (!network_begin_realm_connect(g_rc.realm_ip, g_rc.realm_port,
                                     binary_key, g_rc.account_id)) {
        schedule_retry(now);
        return;
    }
    g_rc.phase = RECONNECT_REALM;
}

/**
 * Advance recovery by one frame's worth.
 */
ReconnectPhase net_reconnect_update(double now) {
    switch (g_rc.phase) {
        case RECONNECT_IDLE:
        case RECONNECT_GIVEN_UP:
            return g_rc.phase;

        case RECONNECT_WAITING:
            if (now >= g_rc.next_attempt_at) start_attempt(now);
            return g_rc.phase;

        case RECONNECT_REALM: {
            NetConnectPhase p = network_connect_poll();
            if (p == NET_CONNECT_PENDING) return g_rc.phase;
            if (p != NET_CONNECT_SUCCEEDED) { schedule_retry(now); return g_rc.phase; }

            /* The realm is back. The old world ticket is single-use and
             * sixty-second-lived, so ask for a new one rather than reusing it. */
            snprintf(g_rc.status, sizeof(g_rc.status), "Requesting a new world ticket...");
            if (!network_request_enter_world(g_rc.character_id, g_rc.world_id)) {
                schedule_retry(now);
                return g_rc.phase;
            }
            g_rc.stage_deadline = now + TICKET_TIMEOUT_SECONDS;
            g_rc.phase = RECONNECT_TICKET;
            return g_rc.phase;
        }

        case RECONNECT_TICKET: {
            network_update();

            EnterWorldResponsePacket response;
            if (network_get_enter_world_response(&response)) {
                if (!response.success) {
                    response.message[sizeof(response.message) - 1] = '\0';
                    NET_WARN("[RECONNECT] Realm refused world entry: %s\n", response.message);
                    schedule_retry(now);
                    return g_rc.phase;
                }

                /* Copied at the wire field's own width. It was declared 16
                 * bytes and truncated with "%.15s", which was correct only
                 * while the field itself was 16 -- and it is the address a
                 * recovery dials, so a world named rather than numbered was
                 * one this path could never get back to. */
                char world_host[sizeof(response.world_host)];
                snprintf(world_host, sizeof(world_host), "%s", response.world_host);

                snprintf(g_rc.status, sizeof(g_rc.status), "Rejoining the world...");
                if (!network_begin_world_connect(world_host, ntohs(response.world_port),
                                                 response.game_ticket, g_rc.character_id)) {
                    schedule_retry(now);
                    return g_rc.phase;
                }
                g_rc.phase = RECONNECT_WORLD;
                return g_rc.phase;
            }

            if (!g_net.connected || now > g_rc.stage_deadline) {
                schedule_retry(now);
            }
            return g_rc.phase;
        }

        case RECONNECT_WORLD: {
            NetConnectPhase p = network_connect_poll();
            if (p == NET_CONNECT_PENDING) return g_rc.phase;
            if (p != NET_CONNECT_SUCCEEDED) { schedule_retry(now); return g_rc.phase; }

            /* Back in. Reset the backoff so the next unrelated drop starts
             * from one second rather than from wherever this one ended. */
            g_rc.attempts = 0;
            g_rc.backoff  = 0.0;
            g_rc.phase    = RECONNECT_IDLE;
            snprintf(g_rc.status, sizeof(g_rc.status), "Reconnected");

            network_set_character_id(g_rc.character_id);
            network_request_character_data(g_rc.character_id, g_rc.world_id);
            return g_rc.phase;
        }
    }
    return g_rc.phase;
}

/** The current phase. */
ReconnectPhase net_reconnect_phase(void) { return g_rc.phase; }

/** A short line describing the current state. */
const char* net_reconnect_status(void) { return g_rc.status; }

/** Attempts made since the connection dropped. */
int net_reconnect_attempts(void) { return g_rc.attempts; }

/** Abandon recovery and reset the backoff. */
void net_reconnect_cancel(void) {
    network_connect_abort();
    g_rc.phase    = RECONNECT_IDLE;
    g_rc.attempts = 0;
    g_rc.backoff  = 0.0;
    g_rc.status[0] = '\0';
}

/** Seconds until the next attempt, or 0 when one is in flight or none is due. */
double net_reconnect_seconds_until_retry(double now) {
    if (g_rc.phase != RECONNECT_WAITING) return 0.0;
    double remaining = g_rc.next_attempt_at - now;
    return remaining > 0.0 ? remaining : 0.0;
}
