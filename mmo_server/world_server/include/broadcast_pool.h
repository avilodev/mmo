#ifndef BROADCAST_POOL_H
#define BROADCAST_POOL_H

/** @file Fan one broadcast pass out across worker threads, sharded by descriptor.
 *
 * The broadcast thread used to build and send every player's packets by itself:
 * at 1000 players, one pass meant 1000 interest queries and 1000 send() calls,
 * strictly serialised, three times over for the projectile, player, and NPC
 * streams. The TickScheduler's overload shedding kept that from falling over,
 * but shedding is a safety valve, not headroom.
 *
 * Connections are already pinned to an epoll loop by `fd % net_loop_count()` for
 * their whole lifetime. This pool shards on the same expression, so shard N only
 * ever writes to sockets loop N owns. Nothing about that is required for
 * correctness — connection_io_send() locks per descriptor, so any thread may
 * send to any socket — but it keeps a connection's outbound traffic on one
 * thread instead of scattering it across all of them.
 *
 * Sharding is only safe because a pass sends from an already-built snapshot: by
 * the time work is handed out, the snapshot is immutable and no shard writes
 * anything another shard reads.
 */

/** Send one shard of a broadcast pass.
 *
 * Invoked concurrently, once per shard. Handle exactly the entries where
 * `fd % shard_count == shard` and touch nothing another shard might write.
 *
 * @param ctx  Pass context supplied to broadcast_pool_run().
 * @param shard  This worker's shard index, in [0, shard_count).
 * @param shard_count  Total shards in this pass.
 */
typedef void (*BroadcastShardFn)(void* ctx, int shard, int shard_count);

/** Start the pool.
 *
 * @param shard_count  Desired shards; clamped to [1, BROADCAST_POOL_MAX_SHARDS].
 *                     Pass net_loop_count() to align shards with loop ownership.
 * @return 1 on success, or 0 when threads could not be created.
 */
int broadcast_pool_start(int shard_count);

/** Stop the pool and join its workers. Safe to call when never started. */
void broadcast_pool_stop(void);

/** Report the active shard count, or 1 when the pool is not running. */
int broadcast_pool_shards(void);

/** Run one function across every shard and return once all shards have finished.
 *
 * The calling thread runs shard 0 itself rather than idling, so a single-shard
 * pool costs nothing beyond a direct call.
 */
void broadcast_pool_run(BroadcastShardFn fn, void* ctx);

/** Bound the pool, matching NET_LOOP_MAX_LOOPS so shards can align with loops. */
#define BROADCAST_POOL_MAX_SHARDS 8

#endif // BROADCAST_POOL_H
