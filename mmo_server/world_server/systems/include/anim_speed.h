#ifndef ANIM_SPEED_H
#define ANIM_SPEED_H

/** @file The ground speed other players animate a body by.
 *
 * Clients used to work a remote body's speed out from how fast its drawn
 * position moved, smoothed over several frames, so a player who stopped kept
 * walking on everyone else's screen for half a second. The speed the owner is
 * actually moving at travels in the position broadcast instead.
 *
 * It is cosmetic and only ever shown, but it is still capped by the speed the
 * server allows the player, so a client cannot make its body look like it is
 * sprinting at the speed of sound.
 */

/** Seconds without an accepted move after which a body counts as standing. */
#define ANIM_SPEED_STALE_SECONDS 0.3

/** The speed a move packet reports, in world units per second.
 *
 * Zero for a velocity that is not finite; never above what the server allows
 * a player moving at `move_speed`.
 */
float anim_speed_reported(float vel_x, float vel_y, float move_speed);

/** The speed to broadcast: `speed` while it is fresh, otherwise zero. */
float anim_speed_current(float speed, double set_at, double now);

#endif // ANIM_SPEED_H
