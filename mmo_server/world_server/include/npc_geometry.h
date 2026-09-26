#ifndef NPC_GEOMETRY_H
#define NPC_GEOMETRY_H

/** @file Test whether a point lies inside a telegraph's shape.
 *
 * The four shapes the wire carries, as pure functions over positions. They live
 * apart from the AI tick because the tick is not their only caller for long:
 * mitigation (frontal block) asks the same question about a facing arc, and any
 * future shape the client learns to draw is tested here rather than inline.
 *
 * Every angle is in degrees and every distance in world units, matching what
 * NPCAbilityDef carries after npc_ai_profile.c converts it from tiles.
 */

/** Test a point against a circle. */
int point_in_circle(float px, float py, float cx, float cy, float radius);

/** Test a point against a cone, given its axis direction and full angle. */
int point_in_cone(float px, float py, float cx, float cy,
                  float dir_x, float dir_y, float radius, float angle_deg);

/** Test a point against a rectangle extending forward along a direction. */
int point_in_rectangle(float px, float py, float cx, float cy,
                       float dir_x, float dir_y, float width, float length);

/** Dispatch to the predicate for one NPCTelegraphShape. */
int point_in_telegraph(float px, float py, int shape,
                       float cx, float cy, float dir_x, float dir_y,
                       float radius, float angle, float width, float length);

#endif // NPC_GEOMETRY_H
