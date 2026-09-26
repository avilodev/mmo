/**
 * @file
 * Point-in-shape tests for the telegraph geometries the wire carries.
 *
 * Extracted from npc_ai.c: these are pure functions with no NPC state in them,
 * and the tick is not their only caller for long -- V9's frontal block asks the
 * same question about a facing arc.
 */

#include "npc_geometry.h"
#include "npc_registry.h"   /* NPCTeleShape */

#include <math.h>
#include <stdint.h>

/** Plane distance between two points. */
static float dist2d(float ax, float ay, float bx, float by) {
    float dx = bx - ax;
    float dy = by - ay;
    return sqrtf(dx * dx + dy * dy);
}

int point_in_circle(float px, float py, float cx, float cy, float radius) {
    return dist2d(px, py, cx, cy) <= radius;
}

/**
 * Test whether a point lies within an oriented cone.
 *
 * @return 1 when inside, or 0 otherwise.
 */
int point_in_cone(float px, float py, float cx, float cy,
                          float dir_x, float dir_y, float radius, float angle_deg) {
    float dx = px - cx;
    float dy = py - cy;
    float dist = sqrtf(dx * dx + dy * dy);
    if (dist > radius) return 0;
    if (dist < 0.001f) return 1; // At center

    float dot = (dx * dir_x + dy * dir_y) / dist;
    float half_angle_rad = (angle_deg / 2.0f) * (float)M_PI / 180.0f;
    return dot >= cosf(half_angle_rad);
}

/**
 * Test whether a point lies within an oriented forward rectangle.
 *
 * @return 1 when inside, or 0 otherwise.
 */
int point_in_rectangle(float px, float py, float cx, float cy,
                                float dir_x, float dir_y, float width, float length) {
    // Rectangle extends from center in dir direction for length, width perpendicular
    float dx = px - cx;
    float dy = py - cy;

    // Project onto direction axis (length)
    float along = dx * dir_x + dy * dir_y;
    if (along < 0.0f || along > length) return 0;

    // Project onto perpendicular axis (width)
    float perp = dx * (-dir_y) + dy * dir_x;
    float half_w = width / 2.0f;
    return (perp >= -half_w && perp <= half_w);
}

/**
 * Dispatch a point-containment test for a telegraph shape.
 *
 * @return 1 when inside, or 0 for an exterior point or unknown shape.
 */
int point_in_telegraph(float px, float py, int shape,
                                float cx, float cy,
                                float dir_x, float dir_y,
                                float radius, float angle,
                                float width, float length) {
    switch (shape) {
        case NPC_TELE_CIRCLE:
            return point_in_circle(px, py, cx, cy, radius);
        case NPC_TELE_CONE:
            return point_in_cone(px, py, cx, cy, dir_x, dir_y, radius, angle);
        case NPC_TELE_RECTANGLE:
            return point_in_rectangle(px, py, cx, cy, dir_x, dir_y, width, length);
        case NPC_TELE_LINE:
            return point_in_rectangle(px, py, cx, cy, dir_x, dir_y, width, length);
        default:
            return 0;
    }
}
