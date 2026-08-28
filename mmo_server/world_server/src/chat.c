/**
 * @file
 * Route player chat off the network loop threads.
 */

#define _POSIX_C_SOURCE 200809L

#include "chat.h"

#include "config.h"
#include "log.h"
#include "player_data.h"
#include "friends.h"
#include "types.h"
#include "utils.h"
#include "str_fixed.h"

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern ActivePlayer active_players[];

/** Interest radius for CHAT_CHANNEL_LOCAL, in world pixels. */
#define CHAT_LOCAL_RANGE 800.0f

/** Seconds between global messages from one character, by default.
 *
 * Global chat is the one channel whose cost does not fall off with distance or
 * party size: every message reaches every player. It therefore needs a budget
 * of its own rather than sharing the generic packet allowance, which is priced
 * for packets that cost one send. Overridable with $MMO_CHAT_GLOBAL_COOLDOWN.
 */
#define CHAT_GLOBAL_COOLDOWN_DEFAULT 3.0

/** Queued messages tolerated before the oldest global message is dropped.
 *
 * Not a structural ceiling -- the queue is a growable ring and reaches this
 * only under sustained abuse -- but a policy one, so a flood cannot turn into
 * unbounded memory. Overridable with $MMO_CHAT_QUEUE_LIMIT.
 */
#define CHAT_QUEUE_LIMIT_DEFAULT 4096

/** Slots the queue starts with; it doubles as needed. */
#define CHAT_QUEUE_INITIAL 64

/** One message waiting to be delivered. */
typedef struct {
    ChatMessagePacket packet;      /**< Already byte-ordered and terminated. */
    uint8_t           channel;
    uint32_t          sender_id;
    uint32_t          sender_account;      /**< Whose blocks recipients check. */
    int               sender_fd;
    float             sender_x, sender_y;
    uint32_t          sender_party;
    char              whisper_target[32];  /**< Empty unless channel is whisper. */
} ChatJob;

/** A growable ring of pending messages. */
static ChatJob*        g_queue    = NULL;
static size_t          g_capacity = 0;
static size_t          g_head     = 0;   /**< Next job to deliver. */
static size_t          g_count    = 0;
static pthread_mutex_t g_lock     = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_ready    = PTHREAD_COND_INITIALIZER;

static pthread_t   g_thread;
static int         g_thread_started = 0;
static _Atomic int g_running        = 0;
static _Atomic uint64_t g_dropped   = 0;

static double g_global_cooldown = CHAT_GLOBAL_COOLDOWN_DEFAULT;
static size_t g_queue_limit     = CHAT_QUEUE_LIMIT_DEFAULT;

/** Per-character cooldown state for global chat.
 *
 * Keyed by the character's registry slot rather than its identifier: the slot
 * is what the rest of this file already has in hand, it is dense, and a slot
 * being recycled to another character is exactly when the cooldown should
 * reset anyway.
 */
static double          g_last_global[MAX_PLAYERS];
static pthread_mutex_t g_cooldown_lock = PTHREAD_MUTEX_INITIALIZER;

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/** Read a double from the environment, or return a default. */
static double env_double(const char* name, double fallback, double low, double high) {
    const char* raw = getenv(name);
    if (!raw || !*raw) return fallback;

    char* end = NULL;
    double parsed = strtod(raw, &end);
    if (end == raw || *end || parsed < low || parsed > high) {
        LOG_ERROR("[CHAT] %s='%s' is out of range [%g, %g]; using %g",
                  name, raw, low, high, fallback);
        return fallback;
    }
    return parsed;
}

/* --- The queue ----------------------------------------------------------- */

/** Push one job, growing the ring. Caller holds g_lock.
 *
 * @return 1 when queued, or 0 when the allocation failed.
 */
static int queue_push_locked(const ChatJob* job) {
    if (g_count == g_capacity) {
        size_t grown = g_capacity ? g_capacity * 2 : CHAT_QUEUE_INITIAL;
        ChatJob* bigger = malloc(grown * sizeof(*bigger));
        if (!bigger) return 0;

        /* Unroll the ring into the new buffer so head is zero again. */
        for (size_t i = 0; i < g_count; i++)
            bigger[i] = g_queue[(g_head + i) % g_capacity];

        free(g_queue);
        g_queue    = bigger;
        g_capacity = grown;
        g_head     = 0;
    }

    g_queue[(g_head + g_count) % g_capacity] = *job;
    g_count++;
    return 1;
}

/** Pop one job. Caller holds g_lock. @return 1 when `out` was filled. */
static int queue_pop_locked(ChatJob* out) {
    if (g_count == 0) return 0;
    *out = g_queue[g_head];
    g_head = (g_head + 1) % g_capacity;
    g_count--;
    return 1;
}

/* --- Delivery ------------------------------------------------------------ */

/** Deliver one queued message to whoever should receive it.
 *
 * Runs on the dispatch thread. This is the part that is O(online players), and
 * the whole reason it is not on a network loop thread.
 */
static void deliver(const ChatJob* job) {
    int* recipients = malloc(sizeof(int) * MAX_PLAYERS);
    if (!recipients) {
        LOG_ERROR("[CHAT] out of memory selecting recipients");
        return;
    }
    int recipient_count = 0;

    ChatMessagePacket msg = job->packet;

    /* A whisper goes to exactly one player, so it never needs the roster.
     *
     * It used to take the registry read lock and walk every online player,
     * locking each one's slot until a name matched -- O(online) mutex
     * operations to deliver one line to one person, and the cost peaked
     * precisely when the world was busiest. One index lookup answers it now. */
    if (job->channel == CHAT_CHANNEL_WHISPER) {
        /* Resolved to a character rather than straight to a descriptor, because
         * the block is held against the sender's account and answering that
         * needs the target's account. */
        uint32_t target_id = player_find_by_name(job->whisper_target);
        int      target_fd = -1;
        uint32_t target_account = 0;

        if (target_id) {
            ActivePlayer* t = player_acquire(target_id);
            if (t) {
                target_fd      = t->client_fd;
                target_account = t->account_id;
                player_release(t);
            }
        }

        /* A whisper to somebody who has blocked you is dropped, and the sender
         * is told nothing -- not even that it failed. The echo below still
         * prints, so their client shows the line they sent exactly as it would
         * have. Telling them would make the block a detector, and a detector
         * is what turns one blocked account into two. */
        if (target_fd != -1 &&
            world_friends_is_blocked(target_account, job->sender_account)) {
            LOG_DEBUG("[WHISPER] account %u is blocked by %u; whisper swallowed",
                      job->sender_account, target_account);
            target_fd = -2;
        }

        if (target_fd == -1) {
            LOG_WARN_RL(5, 60, "[WHISPER] Target '%s' not online", job->whisper_target);
        } else if (target_fd == -2) {
            /* Swallowed. The sender's own echo still goes out. */
            ChatMessagePacket echo = msg;
            memset(echo.sender_name, 0, sizeof(echo.sender_name));
            snprintf(echo.sender_name, sizeof(echo.sender_name), "-> %.28s",
                     job->whisper_target);
            server_send(job->sender_fd, &echo, sizeof(echo));
        } else {
            server_send(target_fd, &msg, sizeof(msg));
            // label the sender's whisper echo with its target
            ChatMessagePacket echo = msg;
            memset(echo.sender_name, 0, sizeof(echo.sender_name));
            snprintf(echo.sender_name, sizeof(echo.sender_name), "-> %.28s",
                     job->whisper_target);
            server_send(job->sender_fd, &echo, sizeof(echo));
        }
        free(recipients);
        return;
    }

    player_registry_rdlock();
    int n_slots = 0;
    const int* slots = player_active_list_locked(&n_slots);

    for (int s = 0; s < n_slots && recipient_count < MAX_PLAYERS; s++) {
        int i = slots[s];
        if (!active_players[i].is_loaded) continue;

        pthread_mutex_lock(&active_players[i].lock);
        int      fd            = active_players[i].client_fd;
        float    dx            = active_players[i].pos_x - job->sender_x;
        float    dy            = active_players[i].pos_y - job->sender_y;
        uint32_t their_party   = active_players[i].party_id;
        uint32_t their_account = active_players[i].account_id;
        pthread_mutex_unlock(&active_players[i].lock);

        /* Whoever has blocked the sender does not receive the line, on any
         * channel. Checked outside the slot lock, against an index rather than
         * Redis, which is what makes it affordable once per recipient per
         * message on the global channel. */
        if (world_friends_is_blocked(their_account, job->sender_account)) continue;

        int wants = 0;
        if (job->channel == CHAT_CHANNEL_LOCAL)
            wants = (dx * dx + dy * dy) <= CHAT_LOCAL_RANGE * CHAT_LOCAL_RANGE;
        else if (job->channel == CHAT_CHANNEL_GLOBAL)
            wants = 1;
        else if (job->channel == CHAT_CHANNEL_PARTY)
            wants = (job->sender_party != 0 && their_party == job->sender_party);

        if (wants) recipients[recipient_count++] = fd;
    }

    player_registry_unlock();

    for (int r = 0; r < recipient_count; r++)
        server_send(recipients[r], &msg, sizeof(msg));

    free(recipients);
}

/** Drain the queue until shutdown. */
static void* chat_dispatch_thread(void* arg) {
    (void)arg;

    while (atomic_load(&g_running)) {
        ChatJob job;
        int have = 0;

        pthread_mutex_lock(&g_lock);
        while (g_count == 0 && atomic_load(&g_running)) {
            struct timespec wait;
            clock_gettime(CLOCK_REALTIME, &wait);
            wait.tv_sec += 1;   /* so shutdown is noticed without a signal */
            pthread_cond_timedwait(&g_ready, &g_lock, &wait);
        }
        have = queue_pop_locked(&job);
        pthread_mutex_unlock(&g_lock);

        if (have) deliver(&job);
    }

    /* Deliver what is already queued rather than discarding it: a player's
     * last message before a shutdown is still worth sending. */
    for (;;) {
        ChatJob job;
        pthread_mutex_lock(&g_lock);
        int have = queue_pop_locked(&job);
        pthread_mutex_unlock(&g_lock);
        if (!have) break;
        deliver(&job);
    }

    return NULL;
}

/* --- Lifecycle ----------------------------------------------------------- */

/**
 * Start the chat dispatch thread.
 *
 * @return 1 on success, or 0 when the thread cannot be created.
 */
int chat_init(void) {
    if (g_thread_started) return 1;

    g_global_cooldown = env_double("MMO_CHAT_GLOBAL_COOLDOWN",
                                   CHAT_GLOBAL_COOLDOWN_DEFAULT, 0.0, 3600.0);

    const char* limit = getenv("MMO_CHAT_QUEUE_LIMIT");
    if (limit && *limit) {
        long parsed = strtol(limit, NULL, 10);
        if (parsed >= 16 && parsed <= 1000000) g_queue_limit = (size_t)parsed;
        else LOG_ERROR("[CHAT] MMO_CHAT_QUEUE_LIMIT='%s' is out of range; using %zu",
                       limit, g_queue_limit);
    }

    for (int i = 0; i < MAX_PLAYERS; i++) g_last_global[i] = 0.0;

    atomic_store(&g_running, 1);
    if (pthread_create(&g_thread, NULL, chat_dispatch_thread, NULL) != 0) {
        atomic_store(&g_running, 0);
        LOG_ERROR("[CHAT] could not start the dispatch thread: %s", strerror(errno));
        return 0;
    }
    g_thread_started = 1;

    LOG_INFO("[CHAT] dispatch thread started (global cooldown %.1fs, queue limit %zu)",
             g_global_cooldown, g_queue_limit);
    return 1;
}

/** Stop the dispatch thread after delivering what is already queued. */
void chat_shutdown(void) {
    if (!g_thread_started) return;

    atomic_store(&g_running, 0);
    pthread_mutex_lock(&g_lock);
    pthread_cond_broadcast(&g_ready);
    pthread_mutex_unlock(&g_lock);

    pthread_join(g_thread, NULL);
    g_thread_started = 0;

    pthread_mutex_lock(&g_lock);
    free(g_queue);
    g_queue    = NULL;
    g_capacity = 0;
    g_head     = 0;
    g_count    = 0;
    pthread_mutex_unlock(&g_lock);

    LOG_INFO("[CHAT] dispatch thread stopped");
}

/** Messages waiting to be delivered. */
size_t chat_queue_depth(void) {
    pthread_mutex_lock(&g_lock);
    size_t depth = g_count;
    pthread_mutex_unlock(&g_lock);
    return depth;
}

/** Messages dropped because the queue was over its backpressure limit. */
uint64_t chat_dropped_count(void) {
    return atomic_load(&g_dropped);
}

/* --- The loop-thread half ------------------------------------------------ */

/** Apply the global-chat cooldown for one character.
 *
 * @return 1 when the message may proceed, or 0 when it is too soon.
 */
static int global_cooldown_allows(int slot, double now) {
    if (g_global_cooldown <= 0.0) return 1;
    if (slot < 0 || slot >= MAX_PLAYERS) return 1;

    int allowed;
    pthread_mutex_lock(&g_cooldown_lock);
    if (now - g_last_global[slot] >= g_global_cooldown) {
        g_last_global[slot] = now;
        allowed = 1;
    } else {
        allowed = 0;
    }
    pthread_mutex_unlock(&g_cooldown_lock);
    return allowed;
}

/** Queue one job, applying backpressure. */
static void enqueue(const ChatJob* job) {
    pthread_mutex_lock(&g_lock);

    if (g_count >= g_queue_limit) {
        /* Drop the oldest rather than the newest: under a flood the backlog is
         * the flood, and a reader would rather see the most recent traffic. */
        ChatJob discarded;
        queue_pop_locked(&discarded);
        atomic_fetch_add(&g_dropped, 1);
        LOG_WARN_RL(5, 60, "[CHAT] queue at its %zu-message limit; dropping the "
                           "oldest message (%llu dropped so far)",
                    g_queue_limit, (unsigned long long)atomic_load(&g_dropped));
    }

    if (!queue_push_locked(job)) {
        atomic_fetch_add(&g_dropped, 1);
        LOG_ERROR("[CHAT] out of memory queueing a message");
    } else {
        pthread_cond_signal(&g_ready);
    }

    pthread_mutex_unlock(&g_lock);
}

/**
 * Validate one CHAT_SEND and hand it to the dispatcher.
 */
void chat_handle_send(int client_fd, uint32_t character_id,
                      uint8_t* buffer, ssize_t bytes) {
    if (bytes < (ssize_t)sizeof(ChatSendPacket)) {
        LOG_WARN_RL(5, 60, "Invalid chat packet size");
        return;
    }

    ChatSendPacket* chat = (ChatSendPacket*)buffer;

    // Sanitize: ensure null termination
    chat->message[MAX_CHAT_MESSAGE - 1] = '\0';

    // Reject empty messages
    if (chat->message[0] == '\0') return;

    ChatJob job;
    memset(&job, 0, sizeof(job));
    job.channel   = chat->channel;
    job.sender_id = character_id;
    job.sender_fd = client_fd;

    // Get sender info — acquire lock, extract needed fields, release immediately
    char sender_name[32];
    int  slot = player_slot_of(character_id);

    ActivePlayer* sender = player_acquire(character_id);
    if (!sender) return;
    snprintf(sender_name, sizeof(sender_name), "%s", sender->username);
    job.sender_x       = sender->pos_x;
    job.sender_y       = sender->pos_y;
    job.sender_party   = sender->party_id;
    /* Carried on the job, not looked up at delivery: a block is against the
     * account, the sender may log out before the queue drains, and by then
     * their slot may belong to somebody else. */
    job.sender_account = sender->account_id;
    player_release(sender);

    if (job.channel == CHAT_CHANNEL_GLOBAL &&
        !global_cooldown_allows(slot, now_seconds())) {
        LOG_DEBUG("[CHAT] %s is inside the global cooldown; dropping", sender_name);
        return;
    }

    // Build broadcast packet
    ChatMessagePacket* msg = &job.packet;
    msg->header.type = PACKET_CHAT_MESSAGE;
    msg->header.player_id = htonl(character_id);
    msg->header.payload_size = htons(sizeof(ChatMessagePacket) - sizeof(PacketHeader));
    msg->sender_id = htonl(character_id);
    msg->channel = job.channel;
    STR_COPY_FIELD(msg->sender_name, sender_name);
    STR_COPY_FIELD(msg->message, chat->message);

    LOG_DEBUG("[CHAT] %s (ch=%u): %s", msg->sender_name, job.channel, msg->message);

    if (job.channel == CHAT_CHANNEL_WHISPER) {
        // split "TargetName message" whisper framing
        const char* space = strchr(msg->message, ' ');
        if (!space || space == msg->message || *(space + 1) == '\0') {
            // Malformed — no target name or no body; silently drop
            return;
        }

        size_t name_len = (size_t)(space - msg->message);
        if (name_len >= sizeof(job.whisper_target)) name_len = sizeof(job.whisper_target) - 1;
        memcpy(job.whisper_target, msg->message, name_len);
        job.whisper_target[name_len] = '\0';

        // Rewrite the message to just the body text
        char body[MAX_CHAT_MESSAGE];
        STR_COPY_FIELD(body, space + 1);
        memset(msg->message, 0, sizeof(msg->message));
        STR_COPY_FIELD(msg->message, body);

        LOG_DEBUG("[WHISPER] %s -> %s: %s", sender_name, job.whisper_target, msg->message);
    }

    enqueue(&job);
}
