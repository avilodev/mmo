/**
 * @file
 * Check that a login session slides while it is used and still has a hard end.
 *
 * Needs a reachable Redis and skips itself when there is not one, for the same
 * reason presence_test does.
 *
 * What this guards: a session used to die SESSION_EXPIRY_SECONDS after login
 * however it was used, because validation extended the key's TTL but never the
 * expires_at field it then checked. A player who had been in the world for
 * five minutes could not reconnect to the realm after a dropped connection
 * without logging in again.
 */

#include "session.h"
#include "redis_internal.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_failures = 0;

#define CHECK(cond, what)                                                   \
	do {                                                                    \
		if(cond) {                                                          \
			printf("  ok   %s\n", (what));                                  \
		} else {                                                            \
			printf("  FAIL %s  (%s:%d)\n", (what), __FILE__, __LINE__);     \
			g_failures++;                                                   \
		}                                                                   \
	} while(0)

#define ACCOUNT 987654321u

// Overwrite the two time fields of the test account's session.
static void set_times(long created_at, long expires_at) {
	redis_pool_acquire();
	redisReply* reply = redis_command_locked(
		"HMSET session:%u created_at %ld expires_at %ld", ACCOUNT, created_at, expires_at);
	if(reply)
		freeReplyObject(reply);
	redis_pool_release();
}

// Read the session's expires_at, or -1 when the session is gone.
static long read_expires_at(void) {
	long value = -1;

	redis_pool_acquire();
	redisReply* reply = redis_command_locked("HGET session:%u expires_at", ACCOUNT);
	if(reply && reply->type == REDIS_REPLY_STRING)
		value = atol(reply->str);
	if(reply)
		freeReplyObject(reply);
	redis_pool_release();
	return value;
}

static long within(long a, long b) {
	long d = a - b;

	return d < 0 ? -d : d;
}

int main(void) {
	log_init();
	log_set_level(LOG_LEVEL_ERROR);

	if(!session_init() || !session_is_ready()) {
		printf("SKIP: no reachable Redis\n");
		return 0;
	}

	char key[SESSION_KEY_LENGTH];
	long now;

	printf("a fresh session validates\n");
	CHECK(session_create(ACCOUNT, "10.0.0.1", key), "it is created");
	CHECK(session_validate_from(ACCOUNT, key, "10.0.0.1"), "it validates from its address");
	CHECK(!session_validate_from(ACCOUNT, key, "10.0.0.2"), "and not from another address");

	printf("use extends the expires_at field, not just the TTL\n");
	now = (long)time(NULL);
	set_times(now - 1000, now + 10);
	CHECK(session_validate_from(ACCOUNT, key, "10.0.0.1"), "a session near its end validates");
	CHECK(within(read_expires_at(), now + SESSION_EXPIRY_SECONDS) <= 2,
	      "and is pushed a full idle window out");

	printf("a session left idle past its window is refused\n");
	now = (long)time(NULL);
	set_times(now - 1000, now - 1);
	CHECK(!session_validate_from(ACCOUNT, key, "10.0.0.1"), "it is refused");
	CHECK(read_expires_at() == -1, "and removed");

	printf("the world server can renew without the key\n");
	CHECK(session_create(ACCOUNT, "10.0.0.1", key), "recreated");
	now = (long)time(NULL);
	set_times(now - 1000, now + 10);
	CHECK(session_touch(ACCOUNT), "touch renews a live session");
	CHECK(within(read_expires_at(), now + SESSION_EXPIRY_SECONDS) <= 2,
	      "by a full idle window");
	now = (long)time(NULL);
	set_times(now - 1000, now - 1);
	CHECK(!session_touch(ACCOUNT), "touch cannot revive an ended session");
	CHECK(!session_touch(0), "or touch no account");

	printf("the hard cap holds however it is used\n");
	CHECK(session_create(ACCOUNT, "10.0.0.1", key), "recreated");
	now = (long)time(NULL);
	set_times(now - (SESSION_MAX_LIFETIME_SECONDS - 60), now + 10);
	CHECK(session_validate_from(ACCOUNT, key, "10.0.0.1"), "a session one minute from the cap validates");
	CHECK(within(read_expires_at(), now + 60) <= 2, "but is extended only to the cap");
	now = (long)time(NULL);
	set_times(now - SESSION_MAX_LIFETIME_SECONDS - 1, now + 100);
	CHECK(!session_validate_from(ACCOUNT, key, "10.0.0.1"), "a session past the cap is refused");
	CHECK(read_expires_at() == -1, "and removed");

	redis_pool_acquire();
	redisReply* reply = redis_command_locked("DEL session:%u", ACCOUNT);
	if(reply)
		freeReplyObject(reply);
	redis_pool_release();
	session_close();

	if(g_failures) {
		printf("\n%d check(s) failed\n", g_failures);
		return 1;
	}
	printf("\nALL ASSERTIONS PASSED\n");
	return 0;
}
