/**
 * @file
 * Route player chat off the network loop threads.
 */

#define _POSIX_C_SOURCE 200809L

#include "chat.h"
#include "outbox.h"

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

/** Queued messages tolerated before the oldest message is dropped.
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

// Read a size from the environment, or return a default.
static size_t env_size(const char* name, size_t fallback, size_t low, size_t high) {
	return (size_t)env_double(name, (double)fallback, (double)low, (double)high);
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

// Strips control bytes in place and trims the spaces off both ends.
static void clean_text(char* text) {
	size_t out = 0;
	size_t start = 0;
	size_t length;

	for(size_t i = 0; text[i] != '\0'; i++)
		if((unsigned char)text[i] >= 32 && (unsigned char)text[i] != 127)
			text[out++] = text[i];
	text[out] = '\0';

	while(text[start] == ' ')
		start++;
	length = strlen(text + start);
	while(length > 0 && text[start + length - 1] == ' ')
		length--;
	memmove(text, text + start, length);
	text[length] = '\0';
}

// Fills a whole outgoing chat packet, header included.
static void init_message(ChatMessagePacket* msg, uint8_t channel, uint8_t flags,
		uint32_t sender_id, const char* name, const char* text) {
	memset(msg, 0, sizeof(*msg));
	msg->header.type = PACKET_CHAT_MESSAGE;
	msg->header.player_id = htonl(sender_id);
	msg->header.payload_size = htons(sizeof(*msg) - sizeof(PacketHeader));
	msg->sender_id = htonl(sender_id);
	msg->channel = channel;
	msg->flags = flags;
	STR_COPY_FIELD(msg->sender_name, name);
	STR_COPY_FIELD(msg->message, text);
}

// Tells one connection something on the system channel.
static void send_notice(int fd, const char* text) {
	ChatMessagePacket notice;

	if(fd < 0)
		return;

	init_message(&notice, CHAT_CHANNEL_SYSTEM, 0, 0, "", text);
	outbox_send(fd, &notice, sizeof(notice));
}

// The sender's connection now, or -1 if they left; a stored fd may be reused.
static int sender_fd_now(const ChatJob* job) {
	ActivePlayer* sender = player_acquire(job->sender_id);
	int fd = -1;

	if(sender) {
		if(sender->account_id == job->sender_account)
			fd = sender->client_fd;
		player_release(sender);
	}

	return fd;
}

// Who a whisper is going to.
typedef struct {
	uint32_t id;
	uint32_t account;
	int fd;
} whisper_dest;

// Looks the named player up; returns 0 when nobody by that name is online.
static int resolve_dest(const char* name, whisper_dest* dest) {
	ActivePlayer* player;

	dest->id = player_find_by_name(name);
	if(dest->id == 0)
		return 0;

	player = player_acquire(dest->id);
	if(!player)
		return 0;

	dest->fd = player->client_fd;
	dest->account = player->account_id;
	player_release(player);

	return 1;
}

// One line to one player; never walks the roster.
static void deliver_whisper(const ChatJob* job) {
	ChatMessagePacket msg = job->packet;
	ChatMessagePacket echo = job->packet;
	int sender_fd = sender_fd_now(job);
	whisper_dest dest;
	char notice[MAX_CHAT_TARGET + 40];

	if(!resolve_dest(job->whisper_target, &dest)) {
		snprintf(notice, sizeof(notice), "No player named %s is online.",
				job->whisper_target);
		send_notice(sender_fd, notice);
		return;
	}

	if(dest.id == job->sender_id) {
		send_notice(sender_fd, "You cannot whisper yourself.");
		return;
	}

	// A blocked whisper is dropped but still echoed, so a block can't be seen
	if(world_friends_is_blocked(dest.account, job->sender_account))
		LOG_DEBUG("[WHISPER] account %u is blocked by %u; swallowed",
				job->sender_account, dest.account);
	else
		outbox_send(dest.fd, &msg, sizeof(msg));

	// The echo is labelled with where it went
	echo.flags = CHAT_FLAG_ECHO;
	memset(echo.sender_name, 0, sizeof(echo.sender_name));
	STR_COPY_FIELD(echo.sender_name, job->whisper_target);
	if(sender_fd >= 0)
		outbox_send(sender_fd, &echo, sizeof(echo));
}

// Whether a player at this offset and party hears a non-whisper message.
static int wants_message(const ChatJob* job, float dx, float dy, uint32_t party) {
	switch(job->channel) {
		case CHAT_CHANNEL_LOCAL:
			return (dx * dx + dy * dy) <= CHAT_LOCAL_RANGE * CHAT_LOCAL_RANGE;
		case CHAT_CHANNEL_GLOBAL:
			return 1;
		case CHAT_CHANNEL_PARTY:
			return job->sender_party != 0 && party == job->sender_party;
	}

	return 0;
}

// Recipients of the message being delivered; only the dispatch thread uses it.
static int g_recipients[MAX_PLAYERS];

// Local, global and party: the one part that is O(online players).
static void deliver_broadcast(const ChatJob* job) {
	ChatMessagePacket msg = job->packet;
	const int* slots;
	int slot_count = 0;
	int recipient_count = 0;

	player_registry_rdlock();
	slots = player_active_list_locked(&slot_count);

	for(int s = 0; s < slot_count && recipient_count < MAX_PLAYERS; s++) {
		ActivePlayer* player = &active_players[slots[s]];
		int fd;
		float dx;
		float dy;
		uint32_t party;
		uint32_t account;

		if(!player->is_loaded)
			continue;

		pthread_mutex_lock(&player->lock);
		fd = player->client_fd;
		dx = player->pos_x - job->sender_x;
		dy = player->pos_y - job->sender_y;
		party = player->party_id;
		account = player->account_id;
		pthread_mutex_unlock(&player->lock);

		// Cheap test first: the block lookup is the costlier of the two
		if(!wants_message(job, dx, dy, party))
			continue;
		if(world_friends_is_blocked(account, job->sender_account))
			continue;

		g_recipients[recipient_count++] = fd;
	}

	player_registry_unlock();

	for(int r = 0; r < recipient_count; r++)
		outbox_send(g_recipients[r], &msg, sizeof(msg));
}

// Delivers one queued message to whoever should receive it.
static void deliver(const ChatJob* job) {
	if(job->channel == CHAT_CHANNEL_WHISPER)
		deliver_whisper(job);
	else
		deliver_broadcast(job);
}

/** Drain the queue until shutdown. */
static void* chat_dispatch_thread(void* arg) {
    (void)arg;

    while (atomic_load(&g_running)) {
        ChatJob job;
        int have = 0;

        pthread_mutex_lock(&g_lock);
        // chat_shutdown() broadcasts under the lock, so a plain wait is woken
        while(g_count == 0 && atomic_load(&g_running))
            pthread_cond_wait(&g_ready, &g_lock);
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

	g_queue_limit = env_size("MMO_CHAT_QUEUE_LIMIT", CHAT_QUEUE_LIMIT_DEFAULT,
			16, 1000000);

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

// Seconds until global chat is free for this slot again; 0 means go ahead.
static double global_cooldown_remaining(int slot, double now) {
	double remaining = 0.0;

	if(g_global_cooldown <= 0.0 || slot < 0 || slot >= MAX_PLAYERS)
		return 0.0;

	pthread_mutex_lock(&g_cooldown_lock);
	if(now - g_last_global[slot] >= g_global_cooldown)
		g_last_global[slot] = now;
	else
		remaining = g_global_cooldown - (now - g_last_global[slot]);
	pthread_mutex_unlock(&g_cooldown_lock);

	return remaining;
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

// Copies and cleans the typed text; returns 0 when nothing is left to say.
static int read_text(const ChatSendPacket* chat, char* text, size_t size) {
	snprintf(text, size, "%.*s", MAX_CHAT_MESSAGE - 1, chat->message);
	clean_text(text);

	return text[0] != '\0';
}

// Fills the whisper target; says why and returns 0 when there is none.
static int read_target(int client_fd, const ChatSendPacket* chat, ChatJob* job) {
	snprintf(job->whisper_target, sizeof(job->whisper_target), "%.*s",
			MAX_CHAT_TARGET - 1, chat->target);
	clean_text(job->whisper_target);

	if(job->whisper_target[0] == '\0') {
		send_notice(client_fd, "Whisper to whom?");
		return 0;
	}

	return 1;
}

/**
 * Validate one CHAT_SEND and hand it to the dispatcher.
 */
void chat_handle_send(int client_fd, uint32_t character_id,
		uint8_t* buffer, ssize_t bytes) {
	const ChatSendPacket* chat = (const ChatSendPacket*)buffer;
	ChatJob job;
	ActivePlayer* sender;
	char sender_name[32];
	char text[MAX_CHAT_MESSAGE];
	char notice[64];
	double wait;
	int slot;

	if(bytes < (ssize_t)sizeof(ChatSendPacket)) {
		LOG_WARN_RL(5, 60, "Invalid chat packet size");
		return;
	}

	// Only the speakable channels; the system channel is the server's alone
	if(chat->channel >= CHAT_SPEAKABLE_CHANNELS) {
		LOG_WARN_RL(5, 60, "[CHAT] character %u sent on channel %u",
				character_id, chat->channel);
		return;
	}

	if(!read_text(chat, text, sizeof(text)))
		return;

	memset(&job, 0, sizeof(job));
	job.channel = chat->channel;
	job.sender_id = character_id;
	if(job.channel == CHAT_CHANNEL_WHISPER && !read_target(client_fd, chat, &job))
		return;

	// Take what is needed from the sender and let go at once
	slot = player_slot_of(character_id);
	sender = player_acquire(character_id);
	if(!sender)
		return;
	snprintf(sender_name, sizeof(sender_name), "%s", sender->username);
	job.sender_x = sender->pos_x;
	job.sender_y = sender->pos_y;
	job.sender_party = sender->party_id;
	// Carried on the job: blocks are per account, and the slot may be reused
	job.sender_account = sender->account_id;
	player_release(sender);

	if(job.channel == CHAT_CHANNEL_GLOBAL) {
		wait = global_cooldown_remaining(slot, now_seconds());
		if(wait > 0.0) {
			snprintf(notice, sizeof(notice),
					"Global chat is on cooldown for %d more second(s).",
					(int)wait + 1);
			send_notice(client_fd, notice);
			return;
		}
	}

	init_message(&job.packet, job.channel, 0, character_id, sender_name, text);

	// Whisper text stays out of the log
	if(job.channel == CHAT_CHANNEL_WHISPER)
		LOG_DEBUG("[WHISPER] %s -> %s", sender_name, job.whisper_target);
	else
		LOG_DEBUG("[CHAT] %s (ch=%u): %s", sender_name, job.channel, text);

	enqueue(&job);
}
