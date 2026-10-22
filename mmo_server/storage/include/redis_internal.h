#ifndef REDIS_INTERNAL_H
#define REDIS_INTERNAL_H

/** @file Share the Redis connection pool among the storage modules only.
 *
 * Nothing outside database/ includes this header, so hiredis types never reach
 * the network or game code. Those layers use the session, presence and ticket
 * functions instead.
 */

#include <hiredis/hiredis.h>

/** Lease one pooled Redis connection to the calling thread.
 *
 * There used to be one connection behind one process-wide mutex, so every
 * session validate, ticket mint and ticket consume in the service queued on
 * the same lock -- the busiest moment the system has, everybody logging in at
 * once, was the moment it serialized itself hardest.
 *
 * Re-entrant, so a helper may issue commands inside a caller's span. Every
 * acquire must be paired with redis_pool_release(), including a failed one.
 *
 * @return 1 when a connection is held, or 0 when none could be leased.
 */
int redis_pool_acquire(void);

/** Return this thread's leased Redis connection to the pool. */
void redis_pool_release(void);

/** Issue one Redis command with the sticky-context heal and one retry.
 *
 * The caller must already hold a lease from redis_pool_acquire().
 *
 * Exposed because realm_world_auth.c used redisCommand() directly and so
 * inherited none of this: after a single Redis blip its context stayed broken,
 * and every realm-to-world authentication failed permanently -- silently taking
 * the whole world list offline -- until the process was restarted.
 *
 * @return The reply, which the caller frees, or NULL when Redis is unreachable.
 */
redisReply* redis_command_locked(const char* format, ...);

#endif // REDIS_INTERNAL_H
