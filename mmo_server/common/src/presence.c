/**
 * @file
 * Carry account presence, the friends cache, and the friend mutation bus over Redis.
 *
 * See presence.h for what each of the three layers is for, and for why the
 * mutation bus lives here rather than on the world-to-realm heartbeat link.
 *
 * ## Why presence is a string and not a hash
 *
 * friends_design.md specified `presence:<account>` as a HASH. It is a plain
 * string here holding the same fields in the same encoding the publish uses,
 * for one reason: reading twenty friends' presence has to be ONE round trip,
 * and there is no batched HGETALL. `MGET` is exactly that command for strings.
 * A hash would have meant twenty round trips, or reaching past
 * redis_command_locked() into the raw context to pipeline by hand -- and that
 * function is the only thing in this process that heals a broken Redis context
 * and retries, which is why nothing here bypasses it.
 *
 * The fields are unchanged; only their container is.
 */
#include "presence.h"
#include "session.h"
#include "log.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/** Seconds a cached friend set stands before it must be rebuilt.
 *
 * The realm updates this cache on every mutation, so it should never be wrong.
 * The TTL is what bounds how long it stays wrong if one of those updates is
 * lost -- a cache with no expiry heals only when someone notices.
 */
#define FRIENDS_CACHE_TTL_SECONDS 3600

/** Accounts read in one MGET.
 *
 * A cap on the command string, not on the caller: presence_get_many() loops.
 * MAX_FRIENDS is 100, so one batch covers an ordinary panel open.
 */
#define PRESENCE_BATCH_MAX 128

/** Member standing in for "this set is cached and it is empty".
 *
 * Redis has no empty set -- removing the last member deletes the key -- so
 * without a sentinel a friendless player is indistinguishable from an uncached
 * one, and every panel they open misses the cache and goes to the realm.
 * Account id 0 is never a real account, which is what makes it usable here.
 */
#define FRIENDS_CACHE_SENTINEL "0|"

/* --- Encoding -------------------------------------------------------------
 *
 * Field-separated text. The name is always LAST and is taken verbatim to the
 * end of the string, so a separator inside a character name cannot forge the
 * numeric fields around it -- the decoder has stopped looking for separators by
 * the time it reaches the name.
 */

#define FIELD_SEP '|'

/** Read one unsigned field and advance past its separator.
 *
 * @param cursor  Points at the field; advanced past the separator on success.
 * @param out     Receives the value.
 * @return        1 when a field was read, 0 when the input is exhausted or the
 *                field is not a terminated number.
 */
static int take_uint(const char** cursor, unsigned long* out) {
    const char* s = *cursor;
    if (!s || !*s) return 0;

    char* end = NULL;
    unsigned long v = strtoul(s, &end, 10);

    if (end == s) return 0;                  /* no digits consumed */
    if (*end != FIELD_SEP) return 0;         /* not a terminated field */

    *out = v;
    *cursor = end + 1;
    return 1;
}

/** Read one signed field and advance past its separator. */
static int take_int(const char** cursor, long* out) {
    const char* s = *cursor;
    if (!s || !*s) return 0;

    char* end = NULL;
    long v = strtol(s, &end, 10);

    if (end == s) return 0;
    if (*end != FIELD_SEP) return 0;

    *out = v;
    *cursor = end + 1;
    return 1;
}

/** Copy the rest of the message into a bounded name field.
 *
 * Always terminates and never writes past `size`. An over-long name is
 * truncated rather than refused: the message is otherwise well-formed, and
 * dropping a whole mutation because somebody's name is long would be worse
 * than showing a clipped name.
 */
static void take_name(const char* rest, char* out, size_t size) {
    if (!rest) { out[0] = '\0'; return; }
    snprintf(out, size, "%s", rest);
}

size_t friend_mutation_encode(const FriendMutation* m, char* out, size_t out_size) {
    if (!m || !out || out_size == 0) return 0;

    int n = snprintf(out, out_size, "%d%c%u%c%u%c%u%c%u%c%s",
                     (int)m->op,             FIELD_SEP,
                     m->actor_account,       FIELD_SEP,
                     m->target_account,      FIELD_SEP,
                     m->origin_world_id,     FIELD_SEP,
                     m->actor_character_id,  FIELD_SEP,
                     m->target_name);

    /* Refused rather than truncated. A truncated mutation decodes into a
     * different mutation -- a clipped account id is still a valid number --
     * and applying one of those to the friend graph is worse than dropping it. */
    if (n < 0 || (size_t)n >= out_size) return 0;
    return (size_t)n;
}

int friend_mutation_decode(const char* s, FriendMutation* out) {
    if (!s || !out) return 0;

    const char* cursor = s;
    long op = 0;
    unsigned long actor = 0, target = 0, world = 0, character = 0;

    if (!take_int(&cursor, &op))         return 0;
    if (!take_uint(&cursor, &actor))     return 0;
    if (!take_uint(&cursor, &target))    return 0;
    if (!take_uint(&cursor, &world))     return 0;
    if (!take_uint(&cursor, &character)) return 0;

    /* Validated against the enum, not merely cast into it. This value selects a
     * branch in the realm's mutation handler, and it arrives from another
     * process that may be a different build. */
    if (op < 0 || op >= (long)FRIEND_OP_COUNT_) return 0;

    memset(out, 0, sizeof(*out));
    out->op                 = (FriendOp)op;
    out->actor_account      = (uint32_t)actor;
    out->target_account     = (uint32_t)target;
    out->origin_world_id    = (uint32_t)world;
    out->actor_character_id = (uint32_t)character;
    take_name(cursor, out->target_name, sizeof(out->target_name));

    return 1;
}

size_t presence_encode(const PresenceRecord* rec, char* out, size_t out_size) {
    if (!rec || !out || out_size == 0) return 0;

    int n = snprintf(out, out_size, "%u%c%u%c%u%c%d%c%lld%c%s",
                     rec->account_id,   FIELD_SEP,
                     rec->world_id,     FIELD_SEP,
                     rec->character_id, FIELD_SEP,
                     rec->online ? 1 : 0, FIELD_SEP,
                     (long long)rec->since, FIELD_SEP,
                     rec->character_name);

    if (n < 0 || (size_t)n >= out_size) return 0;
    return (size_t)n;
}

int presence_decode(const char* s, PresenceRecord* out) {
    if (!s || !out) return 0;

    const char* cursor = s;
    unsigned long account = 0, world = 0, character = 0;
    long online = 0, since = 0;

    if (!take_uint(&cursor, &account))   return 0;
    if (!take_uint(&cursor, &world))     return 0;
    if (!take_uint(&cursor, &character)) return 0;
    if (!take_int(&cursor, &online))     return 0;
    if (!take_int(&cursor, &since))      return 0;

    memset(out, 0, sizeof(*out));
    out->account_id   = (uint32_t)account;
    out->world_id     = (uint32_t)world;
    out->character_id = (uint32_t)character;
    out->online       = online ? 1 : 0;
    out->since        = (int64_t)since;
    take_name(cursor, out->character_name, sizeof(out->character_name));

    return 1;
}

/* --- Presence ------------------------------------------------------------ */

/** Issue one already-built command line and free its reply.
 *
 * hiredis splits the string it is handed on whitespace into arguments, which is
 * what lets a command with a variable number of keys be built up first and sent
 * as one call. That is only safe while the string contains no '%' and nothing a
 * player controls, so EVERY caller of this builds it from a fixed key prefix and
 * decimal ids and nothing else. Anything carrying a character name must go
 * through redis_command_locked() with placeholders instead -- see presence_set().
 *
 * @return 1 when Redis answered without an error reply.
 */
static int redis_do_raw(const char* cmd) {
    if (!cmd || !*cmd) return 0;
    if (!redis_pool_acquire()) return 0;

    /* Handed to hiredis AS the format, deliberately, so it splits on whitespace
     * into one argument per word. Passing it through a "%s" placeholder instead
     * would send the entire line as a single argument -- Redis would see one
     * command named "SADD friends:12 5 6" and answer with an error. */
    redisReply* reply = redis_command_locked(cmd);
    int ok = reply && reply->type != REDIS_REPLY_ERROR;
    if (reply) freeReplyObject(reply);

    redis_pool_release();
    return ok;
}

static int redis_do(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

/** Format one command line and issue it. @return 1 when Redis accepted it. */
static int redis_do(const char* fmt, ...) {
    char cmd[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);

    if (n < 0 || (size_t)n >= sizeof(cmd)) return 0;
    return redis_do_raw(cmd);
}

int presence_set(uint32_t account_id, uint32_t world_id, uint32_t character_id,
                 const char* character_name) {
    if (!account_id) return 0;

    PresenceRecord rec = {
        .account_id   = account_id,
        .world_id     = world_id,
        .character_id = character_id,
        .since        = (int64_t)time(NULL),
        .online       = 1,
    };
    snprintf(rec.character_name, sizeof(rec.character_name), "%s",
             character_name ? character_name : "");

    char payload[256];
    if (!presence_encode(&rec, payload, sizeof(payload))) {
        LOG_WARN("presence: could not encode a record for account %u", account_id);
        return 0;
    }

    if (!redis_pool_acquire()) return 0;

    /* A "%s" placeholder, not a pre-built command string: the payload carries a
     * character name, and a name containing a space would otherwise be split
     * into two arguments by hiredis and corrupt the command. */
    redisReply* reply = redis_command_locked("SET presence:%u %s EX %d",
                                            account_id, payload, PRESENCE_TTL_SECONDS);
    int ok = reply && reply->type != REDIS_REPLY_ERROR;
    if (reply) freeReplyObject(reply);
    redis_pool_release();

    if (!ok) {
        LOG_WARN("presence: could not write presence for account %u", account_id);
        return 0;
    }

    /* Commit, then publish. A dropped publish costs a friend a delayed "came
     * online" popup; a publish that outran a failed write would announce a
     * presence that is not there. */
    presence_publish(&rec);
    return 1;
}

int presence_clear(uint32_t account_id) {
    if (!account_id) return 0;

    int ok = redis_do("DEL presence:%u", account_id);

    /* Published whether or not the DEL reported success. The key expires on its
     * own, so the durable outcome is the same either way, and a friends list
     * showing somebody online who has left is the more visible failure. */
    PresenceRecord rec = { .account_id = account_id, .online = 0 };
    presence_publish(&rec);

    return ok;
}

int presence_clear_if_character(uint32_t account_id, uint32_t character_id) {
    if (!account_id) return 0;

    /* No character to compare against means the caller does not know who they
     * are clearing, and the unconditional clear is the honest answer. */
    if (!character_id) return presence_clear(account_id);

    PresenceRecord rec;
    if (!presence_get(account_id, &rec)) return 0;   /* already gone */

    /* Somebody else's session owns this key now -- a transfer that got there
     * first. Leaving it alone is the whole point of this function. */
    if (rec.character_id != character_id) return 0;

    return presence_clear(account_id);
}

int presence_refresh_many(const uint32_t* account_ids, int count) {
    if (!account_ids || count <= 0) return 0;

    /* One EXPIRE per account rather than a batched command.
     *
     * There is no batched EXPIRE, and the alternatives -- a Lua script built
     * around a variable key count, or hand-pipelining on the raw context --
     * both mean giving up redis_command_locked()'s context healing for a saving
     * that does not matter here: this runs on the heartbeat path, once every
     * fifteen seconds, and a full world of 800 players costs about 800 local
     * round trips of roughly 50us each. Nothing is waiting on it.
     */
    int refreshed = 0;
    for (int i = 0; i < count; i++) {
        if (!account_ids[i]) continue;
        if (redis_do("EXPIRE presence:%u %d", account_ids[i], PRESENCE_TTL_SECONDS))
            refreshed++;
    }
    return refreshed;
}

int presence_get(uint32_t account_id, PresenceRecord* out) {
    if (!account_id || !out) return 0;

    if (!redis_pool_acquire()) return 0;
    redisReply* reply = redis_command_locked("GET presence:%u", account_id);

    int online = 0;
    if (reply && reply->type == REDIS_REPLY_STRING && reply->str) {
        if (presence_decode(reply->str, out)) online = out->online ? 1 : 0;
    }
    if (reply) freeReplyObject(reply);
    redis_pool_release();

    if (!online) memset(out, 0, sizeof(*out));
    return online;
}

int presence_get_many(const uint32_t* account_ids, int count, PresenceRecord* out) {
    if (!account_ids || !out || count <= 0) return 0;

    memset(out, 0, sizeof(*out) * (size_t)count);
    for (int i = 0; i < count; i++) out[i].account_id = account_ids[i];

    int found = 0;

    for (int base = 0; base < count; base += PRESENCE_BATCH_MAX) {
        int batch = count - base;
        if (batch > PRESENCE_BATCH_MAX) batch = PRESENCE_BATCH_MAX;

        /* Built as one MGET so the whole batch is one round trip. Safe to hand
         * to hiredis as a command line: every byte comes from the fixed prefix
         * and decimal account ids. */
        char cmd[PRESENCE_BATCH_MAX * 24 + 8];
        size_t used = (size_t)snprintf(cmd, sizeof(cmd), "MGET");
        int    built = 0;

        for (int i = 0; i < batch; i++) {
            int n = snprintf(cmd + used, sizeof(cmd) - used, " presence:%u",
                             account_ids[base + i]);
            if (n < 0 || (size_t)n >= sizeof(cmd) - used) break;
            used += (size_t)n;
            built++;
        }
        if (!built) continue;

        if (!redis_pool_acquire()) return -1;
        redisReply* reply = redis_command_locked(cmd);

        if (!reply || reply->type != REDIS_REPLY_ARRAY) {
            if (reply) freeReplyObject(reply);
            redis_pool_release();
            return -1;
        }

        /* Indexed by position, never packed. MGET answers a nil for a missing
         * key, so element i is account_ids[base+i] whether or not that account
         * is online -- and a reader that compacted the array would hand the
         * caller the wrong friend's name under the right friend's row. */
        for (size_t i = 0; i < reply->elements && (int)i < built; i++) {
            redisReply* el = reply->element[i];
            if (!el || el->type != REDIS_REPLY_STRING || !el->str) continue;

            PresenceRecord decoded;
            if (!presence_decode(el->str, &decoded)) continue;
            if (decoded.account_id != account_ids[base + (int)i]) continue;

            out[base + (int)i] = decoded;
            if (decoded.online) found++;
        }

        freeReplyObject(reply);
        redis_pool_release();
    }

    return found;
}

int presence_publish(const PresenceRecord* rec) {
    if (!rec) return 0;

    char payload[256];
    if (!presence_encode(rec, payload, sizeof(payload))) return 0;

    if (!redis_pool_acquire()) return 0;
    redisReply* reply = redis_command_locked("PUBLISH %s %s", PRESENCE_CHANNEL, payload);
    int ok = reply && reply->type != REDIS_REPLY_ERROR;
    if (reply) freeReplyObject(reply);
    redis_pool_release();

    return ok;
}

/* --- Friends cache ------------------------------------------------------- */

int presence_friends_store(uint32_t account_id, const FriendCacheEntry* friends,
                           int count) {
    if (!account_id) return 0;
    if (count < 0 || !friends) count = 0;

    /* Replaced rather than merged: this is called when the realm has just read
     * the authoritative rows, so anything already in the key is by definition
     * older than what is being written. */
    if (!redis_do("DEL friends:%u", account_id)) return 0;

    /* The sentinel goes in first and unconditionally, so the key exists even
     * for an account with no friends at all. Every byte of it is fixed, so the
     * built-line form is safe here. */
    if (!redis_do("SADD friends:%u %s", account_id, FRIENDS_CACHE_SENTINEL)) return 0;

    /* One SADD per member, issued through a "%s" placeholder rather than as a
     * built command line.
     *
     * A member now carries a player-chosen name. redis_do() formats its line
     * and hands it to hiredis AS the format, which splits on whitespace -- so
     * a friend called "Two Words" would be stored as two corrupt members
     * instead of one good one. The placeholder makes the whole member a single
     * argument whatever is in it, the same reason presence_set() uses one.
     * It costs a round trip per friend on a path that runs when a panel opens.
     */
    if (!redis_pool_acquire()) return 0;

    for (int i = 0; i < count; i++) {
        if (!friends[i].account_id) continue;

        char member[16 + PRESENCE_NAME_LEN];
        snprintf(member, sizeof(member), "%u|%s", friends[i].account_id,
                 friends[i].name);

        redisReply* reply = redis_command_locked("SADD friends:%u %s",
                                                 account_id, member);
        if (reply) freeReplyObject(reply);
    }

    redis_pool_release();

    return redis_do("EXPIRE friends:%u %d", account_id, FRIENDS_CACHE_TTL_SECONDS);
}

int presence_friends_load(uint32_t account_id, FriendCacheEntry* out, int max) {
    if (!account_id) return -1;

    if (!redis_pool_acquire()) return -1;
    redisReply* reply = redis_command_locked("SMEMBERS friends:%u", account_id);

    int result = -1;

    if (reply && reply->type == REDIS_REPLY_ARRAY) {
        /* No members at all means no key, which means not cached. A cached
         * account always holds at least the sentinel. */
        if (reply->elements == 0) {
            result = -1;
        } else {
            int n = 0;
            for (size_t i = 0; i < reply->elements; i++) {
                redisReply* el = reply->element[i];
                if (!el || el->type != REDIS_REPLY_STRING || !el->str) continue;

                /* "id|name". The name is last and is taken verbatim to the end
                 * of the member, so a separator inside a character name cannot
                 * forge the id in front of it. */
                char* sep = strchr(el->str, FIELD_SEP);
                if (!sep) continue;

                unsigned long id = strtoul(el->str, NULL, 10);
                if (id == 0) continue;              /* the sentinel */

                if (out && n < max) {
                    out[n].account_id = (uint32_t)id;
                    snprintf(out[n].name, sizeof(out[n].name), "%s", sep + 1);
                }
                n++;
            }
            /* Clamped to what was actually written, so a caller with a short
             * buffer is never told about entries it cannot see. */
            result = (out && n > max) ? max : n;
        }
    }

    if (reply) freeReplyObject(reply);
    redis_pool_release();
    return result;
}

int presence_friends_drop(uint32_t account_id) {
    if (!account_id) return 0;
    return redis_do("DEL friends:%u", account_id);
}

/* --- Pending-request cache ----------------------------------------------- */

/** Seconds a cached request list stands. Matches the friends cache. */
#define REQUESTS_CACHE_TTL_SECONDS FRIENDS_CACHE_TTL_SECONDS

/** Marker holding the key open for an account with no pending requests.
 *
 * A Redis list with no elements does not exist, so without this a player with
 * nothing pending would miss the cache on every panel open. Decoded and
 * discarded on read: from_account 0 is never a real account.
 */
#define REQUESTS_CACHE_SENTINEL "0|0|"

int presence_requests_store(uint32_t account_id, const FriendRequestCache* reqs, int count) {
    if (!account_id) return 0;
    if (count < 0 || !reqs) count = 0;

    if (!redis_do("DEL friendreq:%u", account_id)) return 0;

    if (!redis_pool_acquire()) return 0;

    /* The sentinel is pushed first and unconditionally, so it is also element
     * zero on read -- which is what keeps the rest of the list in the order the
     * realm read them out of the database. */
    redisReply* first = redis_command_locked("RPUSH friendreq:%u %s",
                                            account_id, REQUESTS_CACHE_SENTINEL);
    int ok = first && first->type != REDIS_REPLY_ERROR;
    if (first) freeReplyObject(first);

    for (int i = 0; ok && i < count; i++) {
        char entry[128];
        /* Name last and verbatim, exactly as everything else on this bus, so a
         * separator inside a character name cannot forge the fields before it. */
        int n = snprintf(entry, sizeof(entry), "%u%c%lld%c%s",
                         reqs[i].from_account, FIELD_SEP,
                         (long long)reqs[i].created_at, FIELD_SEP,
                         reqs[i].from_name);
        if (n < 0 || (size_t)n >= sizeof(entry)) continue;

        /* A placeholder, not a built command line: this carries a name. */
        redisReply* reply = redis_command_locked("RPUSH friendreq:%u %s", account_id, entry);
        ok = reply && reply->type != REDIS_REPLY_ERROR;
        if (reply) freeReplyObject(reply);
    }

    redis_pool_release();
    if (!ok) return 0;

    return redis_do("EXPIRE friendreq:%u %d", account_id, REQUESTS_CACHE_TTL_SECONDS);
}

int presence_requests_load(uint32_t account_id, FriendRequestCache* out, int max) {
    if (!account_id) return -1;

    if (!redis_pool_acquire()) return -1;
    redisReply* reply = redis_command_locked("LRANGE friendreq:%u 0 -1", account_id);

    int result = -1;

    if (reply && reply->type == REDIS_REPLY_ARRAY) {
        /* An absent key answers an empty array, which is a cache miss. A cached
         * account always holds at least the sentinel. */
        if (reply->elements == 0) {
            result = -1;
        } else {
            int n = 0;
            for (size_t i = 0; i < reply->elements; i++) {
                redisReply* el = reply->element[i];
                if (!el || el->type != REDIS_REPLY_STRING || !el->str) continue;

                const char*   cursor = el->str;
                unsigned long from = 0;
                long          created = 0;

                if (!take_uint(&cursor, &from)) continue;
                if (!take_int(&cursor, &created)) continue;
                if (from == 0) continue;                  /* the sentinel */

                if (out && n < max) {
                    memset(&out[n], 0, sizeof(out[n]));
                    out[n].from_account = (uint32_t)from;
                    out[n].created_at   = (int64_t)created;
                    take_name(cursor, out[n].from_name, sizeof(out[n].from_name));
                }
                n++;
            }
            result = (out && n > max) ? max : n;
        }
    }

    if (reply) freeReplyObject(reply);
    redis_pool_release();
    return result;
}

int presence_requests_drop(uint32_t account_id) {
    if (!account_id) return 0;
    return redis_do("DEL friendreq:%u", account_id);
}

/* --- Mutation bus -------------------------------------------------------- */

/** The connection blocking reads own.
 *
 * Separate from the pool because BRPOP holds its connection for the whole wait.
 * A pooled connection parked in a five-second BRPOP is a connection every other
 * Redis user in the process is queueing behind.
 */
static redisContext*   g_consumer_ctx  = NULL;
static pthread_mutex_t g_consumer_lock = PTHREAD_MUTEX_INITIALIZER;

/** Open or reuse the dedicated connection.
 *
 * Reads the same environment session.c does rather than asking it, because
 * friends_design.md's one structural constraint on this work is that session.c
 * does not change. The duplication is four getenv calls.
 *
 * Caller must hold g_consumer_lock.
 */
static redisContext* consumer_ctx(void) {
    if (g_consumer_ctx && !g_consumer_ctx->err) return g_consumer_ctx;

    if (g_consumer_ctx) {
        redisFree(g_consumer_ctx);
        g_consumer_ctx = NULL;
    }

    const char* host = getenv("REDIS_HOST");
    if (!host || !*host) host = "127.0.0.1";

    const char* port_raw = getenv("REDIS_PORT");
    int port = port_raw ? atoi(port_raw) : 6379;
    if (port <= 0 || port > 65535) port = 6379;

    const struct timeval timeout = { .tv_sec = 2, .tv_usec = 0 };
    g_consumer_ctx = redisConnectWithTimeout(host, port, timeout);

    if (!g_consumer_ctx || g_consumer_ctx->err) {
        if (g_consumer_ctx) {
            LOG_WARN("presence: consumer connection to %s:%d failed: %s",
                     host, port, g_consumer_ctx->errstr);
            redisFree(g_consumer_ctx);
            g_consumer_ctx = NULL;
        }
        return NULL;
    }

    const char* password = getenv("MMO_REDIS_PASSWORD");
    if (password && *password) {
        const char* user = getenv("MMO_REDIS_USER");
        redisReply* reply = user && *user
            ? (redisReply*)redisCommand(g_consumer_ctx, "AUTH %s %s", user, password)
            : (redisReply*)redisCommand(g_consumer_ctx, "AUTH %s", password);

        int ok = reply && reply->type != REDIS_REPLY_ERROR;
        if (!ok) LOG_ERROR("presence: consumer AUTH rejected");
        if (reply) freeReplyObject(reply);

        if (!ok) {
            redisFree(g_consumer_ctx);
            g_consumer_ctx = NULL;
            return NULL;
        }
    }

    return g_consumer_ctx;
}

int friend_mutation_push(const FriendMutation* m) {
    if (!m) return 0;

    char payload[512];
    if (!friend_mutation_encode(m, payload, sizeof(payload))) {
        LOG_WARN("presence: could not encode a mutation from account %u", m->actor_account);
        return 0;
    }

    if (!redis_pool_acquire()) return 0;

    /* LPUSH here, RPOP at the other end: the queue is FIFO, so a burst of
     * requests from one player is applied in the order they clicked. */
    redisReply* reply = redis_command_locked("LPUSH %s %s",
                                            FRIEND_MUTATION_QUEUE, payload);
    int ok = reply && reply->type != REDIS_REPLY_ERROR;
    if (reply) freeReplyObject(reply);
    redis_pool_release();

    return ok;
}

int friend_mutation_pop(FriendMutation* out, int timeout_secs) {
    if (!out) return -1;

    pthread_mutex_lock(&g_consumer_lock);

    redisContext* ctx = consumer_ctx();
    if (!ctx) {
        pthread_mutex_unlock(&g_consumer_lock);
        return -1;
    }

    /* A BRPOP timeout of 0 blocks forever in Redis, which is not what a caller
     * asking for "no wait" means. Zero is served with the non-blocking RPOP. */
    redisReply* reply = timeout_secs > 0
        ? (redisReply*)redisCommand(ctx, "BRPOP %s %d", FRIEND_MUTATION_QUEUE, timeout_secs)
        : (redisReply*)redisCommand(ctx, "RPOP %s", FRIEND_MUTATION_QUEUE);

    if (!reply) {
        /* The context is broken now. Dropped here so the next call reconnects
         * rather than reusing it forever. */
        LOG_WARN("presence: mutation pop failed: %s", ctx->errstr);
        redisFree(g_consumer_ctx);
        g_consumer_ctx = NULL;
        pthread_mutex_unlock(&g_consumer_lock);
        return -1;
    }

    int result = 0;
    const char* payload = NULL;

    if (reply->type == REDIS_REPLY_STRING) {
        payload = reply->str;                          /* RPOP */
    } else if (reply->type == REDIS_REPLY_ARRAY && reply->elements == 2) {
        redisReply* el = reply->element[1];            /* BRPOP: [key, value] */
        if (el && el->type == REDIS_REPLY_STRING) payload = el->str;
    }

    if (payload) {
        if (friend_mutation_decode(payload, out)) {
            result = 1;
        } else {
            /* Dropped, not retried. A message this end cannot parse will not
             * parse on the next attempt either, and leaving it at the head of
             * the queue would stall every mutation behind it. */
            LOG_WARN("presence: discarding an undecodable mutation");
            result = 0;
        }
    }

    freeReplyObject(reply);
    pthread_mutex_unlock(&g_consumer_lock);
    return result;
}

void presence_consumer_close(void) {
    pthread_mutex_lock(&g_consumer_lock);
    if (g_consumer_ctx) {
        redisFree(g_consumer_ctx);
        g_consumer_ctx = NULL;
    }
    pthread_mutex_unlock(&g_consumer_lock);
}
