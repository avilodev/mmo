/**
 * @file
 * Carry friend events from the realm to the world servers over Redis pub/sub.
 *
 * See friend_bus.h for what travels here and why this direction is allowed to
 * be lossy when the other one is not.
 */
#include "friend_bus.h"
#include "session.h"
#include "log.h"

#include <errno.h>
#include <hiredis/hiredis.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/** Seconds the subscriber blocks before checking whether it should stop.
 *
 * Also the shutdown latency. Short enough that a world server does not appear
 * to hang on exit, long enough that a quiet bus is not a busy loop.
 */
#define BUS_POLL_SECONDS 1

#define FIELD_SEP '|'

/* --- Encoding -------------------------------------------------------------
 *
 * Same shape as presence.c's: field-separated, name last and taken verbatim to
 * the end, so a separator inside a character name cannot forge the numeric
 * fields around it.
 */

/** Read one unsigned field and advance past its separator. */
static int take_uint(const char** cursor, unsigned long* out) {
    const char* s = *cursor;
    if (!s || !*s) return 0;

    char* end = NULL;
    unsigned long v = strtoul(s, &end, 10);

    if (end == s) return 0;
    if (*end != FIELD_SEP) return 0;

    *out = v;
    *cursor = end + 1;
    return 1;
}

size_t friend_event_encode(const FriendEvent* e, char* out, size_t out_size) {
    if (!e || !out || out_size == 0) return 0;

    int n = snprintf(out, out_size, "%d%c%u%c%u%c%u%c%u%c%u%c%s",
                     (int)e->type,      FIELD_SEP,
                     e->account_id,     FIELD_SEP,
                     e->character_id,   FIELD_SEP,
                     (unsigned)e->action, FIELD_SEP,
                     (unsigned)e->result, FIELD_SEP,
                     e->peer_account,   FIELD_SEP,
                     e->peer_name);

    /* Refused rather than truncated, for the same reason a mutation is: a
     * clipped account id is still a valid number, and acting on one is worse
     * than dropping the message. */
    if (n < 0 || (size_t)n >= out_size) return 0;
    return (size_t)n;
}

int friend_event_decode(const char* s, FriendEvent* out) {
    if (!s || !out) return 0;

    const char* cursor = s;
    unsigned long type = 0, account = 0, character = 0, action = 0, result = 0, peer = 0;

    if (!take_uint(&cursor, &type))      return 0;
    if (!take_uint(&cursor, &account))   return 0;
    if (!take_uint(&cursor, &character)) return 0;
    if (!take_uint(&cursor, &action))    return 0;
    if (!take_uint(&cursor, &result))    return 0;
    if (!take_uint(&cursor, &peer))      return 0;

    /* Validated against the enum rather than cast into it. This selects a branch
     * in the world's event handler and arrives from a process that may be a
     * different build. */
    if (type >= (unsigned long)FRIEND_EVENT_COUNT_) return 0;
    if (action > 255 || result > 255)               return 0;

    memset(out, 0, sizeof(*out));
    out->type         = (FriendEventType)type;
    out->account_id   = (uint32_t)account;
    out->character_id = (uint32_t)character;
    out->action       = (uint8_t)action;
    out->result       = (uint8_t)result;
    out->peer_account = (uint32_t)peer;
    snprintf(out->peer_name, sizeof(out->peer_name), "%s", cursor ? cursor : "");

    return 1;
}

/* --- Publishing ---------------------------------------------------------- */

int friend_event_publish(uint32_t world_id, const FriendEvent* e) {
    /* A world of zero is refused rather than turned into a broadcast. An event
     * with nowhere to go means the caller failed to resolve a recipient, and
     * sending it to every world would put one player's business on nine servers
     * that have no use for it. */
    if (!world_id || !e) return 0;

    char payload[256];
    if (!friend_event_encode(e, payload, sizeof(payload))) {
        LOG_WARN("friend bus: could not encode an event for account %u", e->account_id);
        return 0;
    }

    if (!redis_pool_acquire()) return 0;
    redisReply* reply = redis_command_locked("PUBLISH friend:events:%u %s",
                                            world_id, payload);
    int ok = reply && reply->type != REDIS_REPLY_ERROR;
    if (reply) freeReplyObject(reply);
    redis_pool_release();

    return ok;
}

/* --- Subscriber ---------------------------------------------------------- */

static pthread_t       g_bus_thread;
static atomic_int      g_bus_running   = 0;
static atomic_int      g_bus_connected = 0;
static uint32_t        g_bus_world     = 0;
static FriendEventFn   g_on_event      = NULL;
static PresenceEventFn g_on_presence   = NULL;
static void*           g_bus_user      = NULL;

/** Open a dedicated connection and subscribe to both channels.
 *
 * Dedicated because a subscribed connection may issue nothing but subscribe
 * commands until it unsubscribes -- it cannot be shared with the pool, and a
 * pooled connection put into subscriber mode would break every other caller.
 *
 * @return The connection, or NULL when Redis is unreachable.
 */
static redisContext* bus_connect(void) {
    const char* host = getenv("REDIS_HOST");
    if (!host || !*host) host = "127.0.0.1";

    const char* port_raw = getenv("REDIS_PORT");
    int port = port_raw ? atoi(port_raw) : 6379;
    if (port <= 0 || port > 65535) port = 6379;

    const struct timeval connect_timeout = { .tv_sec = 2, .tv_usec = 0 };
    redisContext* ctx = redisConnectWithTimeout(host, port, connect_timeout);

    if (!ctx || ctx->err) {
        if (ctx) redisFree(ctx);
        return NULL;
    }

    const char* password = getenv("MMO_REDIS_PASSWORD");
    if (password && *password) {
        const char* user = getenv("MMO_REDIS_USER");
        redisReply* reply = user && *user
            ? (redisReply*)redisCommand(ctx, "AUTH %s %s", user, password)
            : (redisReply*)redisCommand(ctx, "AUTH %s", password);

        int ok = reply && reply->type != REDIS_REPLY_ERROR;
        if (!ok) LOG_ERROR("friend bus: AUTH rejected");
        if (reply) freeReplyObject(reply);
        if (!ok) { redisFree(ctx); return NULL; }
    }

    /* Both channels on one connection: this world's own events, and the shared
     * presence stream every world hears. */
    redisReply* sub = (redisReply*)redisCommand(ctx, "SUBSCRIBE friend:events:%u %s",
                                                g_bus_world, PRESENCE_CHANNEL);
    if (!sub || sub->type == REDIS_REPLY_ERROR) {
        LOG_ERROR("friend bus: SUBSCRIBE refused");
        if (sub) freeReplyObject(sub);
        redisFree(ctx);
        return NULL;
    }
    freeReplyObject(sub);

    /* A read timeout is what makes this thread stoppable. Without it the thread
     * would sit in redisGetReply() forever on a quiet bus and shutdown would
     * hang until the next message arrived. */
    const struct timeval read_timeout = { .tv_sec = BUS_POLL_SECONDS, .tv_usec = 0 };
    redisSetTimeout(ctx, read_timeout);

    return ctx;
}

/** Dispatch one `message` reply to the right callback. */
static void bus_dispatch(redisReply* reply) {
    /* A push is [ "message", channel, payload ]. Anything else is a
     * subscribe/unsubscribe confirmation, which is not an error. */
    if (!reply || reply->type != REDIS_REPLY_ARRAY || reply->elements != 3) return;

    redisReply* kind    = reply->element[0];
    redisReply* channel = reply->element[1];
    redisReply* payload = reply->element[2];

    if (!kind || !kind->str || strcmp(kind->str, "message") != 0) return;
    if (!channel || !channel->str || !payload || !payload->str)   return;

    if (strcmp(channel->str, PRESENCE_CHANNEL) == 0) {
        PresenceRecord rec;
        if (presence_decode(payload->str, &rec) && g_on_presence)
            g_on_presence(&rec, g_bus_user);
        return;
    }

    FriendEvent e;
    if (friend_event_decode(payload->str, &e) && g_on_event)
        g_on_event(&e, g_bus_user);
}

/** Subscribe, then read until asked to stop, reconnecting when the link drops. */
static void* bus_thread(void* arg) {
    (void)arg;

    redisContext* ctx = NULL;

    while (atomic_load(&g_bus_running)) {
        if (!ctx) {
            ctx = bus_connect();
            if (!ctx) {
                atomic_store(&g_bus_connected, 0);
                /* Backing off by sleeping in one-second steps rather than one
                 * long sleep, so a stop request is still answered promptly. */
                for (int i = 0; i < 3 && atomic_load(&g_bus_running); i++) sleep(1);
                continue;
            }
            atomic_store(&g_bus_connected, 1);
            LOG_INFO("[FRIENDS] event bus subscribed for world %u", g_bus_world);
        }

        void* raw = NULL;
        if (redisGetReply(ctx, &raw) == REDIS_OK) {
            bus_dispatch((redisReply*)raw);
            if (raw) freeReplyObject((redisReply*)raw);
            continue;
        }

        /* A read timeout is the ordinary quiet case, not a fault. hiredis
         * reports it by setting ctx->err, which would otherwise make the
         * context permanently unusable -- so it is cleared here and the loop
         * goes back to waiting. Any other error really is a dropped link. */
        if (ctx->err == REDIS_ERR_IO && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            ctx->err = 0;
            ctx->errstr[0] = '\0';
            continue;
        }

        LOG_WARN("[FRIENDS] event bus link lost (%s); reconnecting",
                 ctx->errstr[0] ? ctx->errstr : "unknown");
        redisFree(ctx);
        ctx = NULL;
        atomic_store(&g_bus_connected, 0);
    }

    if (ctx) redisFree(ctx);
    atomic_store(&g_bus_connected, 0);
    return NULL;
}

int friend_bus_start(uint32_t world_id, FriendEventFn on_event,
                     PresenceEventFn on_presence, void* user) {
    if (atomic_load(&g_bus_running)) return 1;   /* idempotent */

    g_bus_world   = world_id;
    g_on_event    = on_event;
    g_on_presence = on_presence;
    g_bus_user    = user;

    atomic_store(&g_bus_running, 1);

    if (pthread_create(&g_bus_thread, NULL, bus_thread, NULL) != 0) {
        LOG_ERROR("[FRIENDS] could not start the event bus thread");
        atomic_store(&g_bus_running, 0);
        return 0;
    }
    return 1;
}

void friend_bus_stop(void) {
    if (!atomic_exchange(&g_bus_running, 0)) return;   /* was not running */

    /* Joined rather than detached: the callbacks reach into world state, and a
     * thread still dispatching into a half-torn-down server is the kind of
     * shutdown crash that only happens on the live host. */
    pthread_join(g_bus_thread, NULL);
    atomic_store(&g_bus_connected, 0);
}

int friend_bus_connected(void) {
    return atomic_load(&g_bus_connected);
}
