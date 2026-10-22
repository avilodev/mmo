#ifndef INTEREST_H
#define INTEREST_H

/** @file Resolve which connected players are near enough to be told about an event.
 *
 * Event broadcasts — a zone appearing, a zone expiring, a projectile despawning —
 * all ask the same question: who is close enough to care? Three call sites had
 * grown their own answer, and one of them had quietly stopped asking: the zone
 * removal broadcast walked every slot in the table and sent to all of them,
 * distance unread.
 *
 * That failure is invisible in a diff and invisible in testing, because with two
 * players online a full-world broadcast and an interest broadcast produce the
 * same result. It only appears at population, as a packet every player receives
 * and all but a handful discard.
 *
 * This is deliberately a linear pass over the online list rather than a spatial
 * query. Events are rare and unscheduled — they happen on a cast or a despawn,
 * not on every tick — and they occur on threads that hold no tick snapshot, so
 * there is no grid to query. Per-tick work, which is the part that has to scale,
 * uses tick_snapshot_query() instead; see ability_tick().
 */

/** Collect the descriptors of online players within a radius of a world point.
 *
 * Takes the player registry read lock and each candidate's slot lock for the
 * duration of one sample; the caller must not already hold either.
 *
 * @param x  Event center x-coordinate in world units.
 * @param y  Event center y-coordinate in world units.
 * @param radius  Non-negative interest radius in world units.
 * @param out_fds  Buffer receiving client descriptors.
 * @param max_out  Capacity of out_fds; collection stops when it is reached.
 * @return  The number of descriptors written.
 */
int interest_collect_fds(float x, float y, float radius, int* out_fds, int max_out);

/** Interest radius for world events players should see happen around them.
 *
 * Matched to PLAYER_VIEW_RADIUS in main.c: an event on someone you cannot see is
 * an event you cannot see either.
 */
#define INTEREST_EVENT_RADIUS 800.0f

#endif // INTEREST_H
