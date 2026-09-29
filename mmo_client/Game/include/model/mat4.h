#ifndef MODEL_MAT4_H
#define MODEL_MAT4_H

#include <math.h>

/* 4x4 matrices and quaternions, for skinning. Header-only for the same reason
   math_utils.h is: these are a dozen multiplies each and want to inline.

   Storage is column-major, the order OpenGL and glTF both use, so a matrix
   read out of a .glb file needs no transposing and one built here could be
   handed to glMultMatrixf unchanged. Element (row r, column c) is m[c*4+r].

   Quaternions are (x, y, z, w) with w last, again matching glTF. */

typedef struct { float m[16]; } Mat4;

static inline Mat4 mat4_identity(void) {
    Mat4 out = {{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}};
    return out;
}

/* out = a * b, applying b first: the parent-times-child order a joint
   hierarchy walks in. */
static inline Mat4 mat4_multiply(Mat4 a, Mat4 b) {
    Mat4 out;
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            out.m[c * 4 + r] = a.m[0 * 4 + r] * b.m[c * 4 + 0] +
                               a.m[1 * 4 + r] * b.m[c * 4 + 1] +
                               a.m[2 * 4 + r] * b.m[c * 4 + 2] +
                               a.m[3 * 4 + r] * b.m[c * 4 + 3];
    return out;
}

/* The transform a glTF node describes: scale, then rotate, then translate. */
static inline Mat4 mat4_from_trs(const float t[3], const float r[4], const float s[3]) {
    float x = r[0], y = r[1], z = r[2], w = r[3];
    Mat4 out;
    out.m[0]  = (1 - 2 * (y * y + z * z)) * s[0];
    out.m[1]  = (2 * (x * y + w * z)) * s[0];
    out.m[2]  = (2 * (x * z - w * y)) * s[0];
    out.m[3]  = 0;
    out.m[4]  = (2 * (x * y - w * z)) * s[1];
    out.m[5]  = (1 - 2 * (x * x + z * z)) * s[1];
    out.m[6]  = (2 * (y * z + w * x)) * s[1];
    out.m[7]  = 0;
    out.m[8]  = (2 * (x * z + w * y)) * s[2];
    out.m[9]  = (2 * (y * z - w * x)) * s[2];
    out.m[10] = (1 - 2 * (x * x + y * y)) * s[2];
    out.m[11] = 0;
    out.m[12] = t[0];
    out.m[13] = t[1];
    out.m[14] = t[2];
    out.m[15] = 1;
    return out;
}

static inline void mat4_transform_point(const Mat4* m, const float p[3], float out[3]) {
    for (int r = 0; r < 3; ++r)
        out[r] = m->m[0 * 4 + r] * p[0] + m->m[1 * 4 + r] * p[1] +
                 m->m[2 * 4 + r] * p[2] + m->m[3 * 4 + r];
}

/* Ignores the translation column. Skinning matrices carry rotation and near
   uniform scale, so the 3x3 stands in for the inverse transpose a general
   matrix would need; a bone that squashed non-uniformly would want the real
   thing, and none of this rig's do. */
static inline void mat4_transform_direction(const Mat4* m, const float d[3], float out[3]) {
    for (int r = 0; r < 3; ++r)
        out[r] = m->m[0 * 4 + r] * d[0] + m->m[1 * 4 + r] * d[1] + m->m[2 * 4 + r] * d[2];
}

/* a * b: the rotation b followed by the rotation a, in the same order
   mat4_multiply composes. */
static inline void quat_multiply(const float a[4], const float b[4], float out[4]) {
    float x = a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1];
    float y = a[3]*b[1] - a[0]*b[2] + a[1]*b[3] + a[2]*b[0];
    float z = a[3]*b[2] + a[0]*b[1] - a[1]*b[0] + a[2]*b[3];
    out[3]  = a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2];
    out[0] = x; out[1] = y; out[2] = z;
}

/* A rotation of `radians` about `axis`, right-handed. `axis` need not be unit:
   it is normalized here, since the axes this gets handed are columns pulled out
   of a matrix rather than authored constants. */
static inline void quat_from_axis_angle(const float axis[3], float radians,
                                        float out[4]) {
    float length = sqrtf(axis[0]*axis[0] + axis[1]*axis[1] + axis[2]*axis[2]);
    if (length < 1e-8f) { out[0] = out[1] = out[2] = 0; out[3] = 1; return; }
    float half = radians * 0.5f;
    float scale = sinf(half) / length;
    out[0] = axis[0] * scale;
    out[1] = axis[1] * scale;
    out[2] = axis[2] * scale;
    out[3] = cosf(half);
}

/* The three transforms a fixed-function matrix stack is built out of, so a
   transform chain can be composed as arithmetic instead of as a sequence of GL
   calls. Each is mat4_from_trs with the other two components neutral, which
   keeps one construction path rather than three hand-written ones.

   Composed left to right with mat4_multiply, these reproduce a glTranslatef /
   glRotatef / glScalef chain exactly: GL post-multiplies each onto the current
   matrix, so `glTranslatef(a); glRotatef(b);` is T(a) * R(b), which is
   mat4_multiply(T, R). */
static inline Mat4 mat4_translation(float x, float y, float z) {
    const float t[3] = {x, y, z};
    const float r[4] = {0, 0, 0, 1};
    const float s[3] = {1, 1, 1};
    return mat4_from_trs(t, r, s);
}

static inline Mat4 mat4_scaling(float x, float y, float z) {
    const float t[3] = {0, 0, 0};
    const float r[4] = {0, 0, 0, 1};
    const float s[3] = {x, y, z};
    return mat4_from_trs(t, r, s);
}

/* Degrees about an arbitrary axis, right-handed, matching glRotatef. */
static inline Mat4 mat4_rotation_degrees(float degrees, float ax, float ay, float az) {
    const float axis[3] = {ax, ay, az};
    const float t[3] = {0, 0, 0};
    const float s[3] = {1, 1, 1};
    float r[4];
    quat_from_axis_angle(axis, degrees * 3.14159265358979323846f / 180.0f, r);
    return mat4_from_trs(t, r, s);
}

/* The shortest rotation taking unit vector `from` onto unit vector `to`.

   The antiparallel case has no shortest arc - every half turn about an axis
   square to the pair does it - so one such axis is picked rather than letting
   the general form divide by zero. */
static inline void quat_between(const float from[3], const float to[3], float out[4]) {
    float dot = from[0]*to[0] + from[1]*to[1] + from[2]*to[2];
    if (dot > 0.999999f) { out[0] = out[1] = out[2] = 0; out[3] = 1; return; }
    float axis[3];
    if (dot < -0.999999f) {
        /* Any axis square to `from`. Crossing with x, unless `from` is x. */
        float seed_x = fabsf(from[0]) < 0.9f ? 1.0f : 0.0f;
        float seed_z = fabsf(from[0]) < 0.9f ? 0.0f : 1.0f;
        axis[0] = from[1]*seed_z - from[2]*0.0f;
        axis[1] = from[2]*seed_x - from[0]*seed_z;
        axis[2] = from[0]*0.0f   - from[1]*seed_x;
        float length = sqrtf(axis[0]*axis[0] + axis[1]*axis[1] + axis[2]*axis[2]);
        if (length < 1e-8f) { out[0] = out[1] = out[2] = 0; out[3] = 1; return; }
        out[0] = axis[0]/length; out[1] = axis[1]/length; out[2] = axis[2]/length;
        out[3] = 0.0f;
        return;
    }
    axis[0] = from[1]*to[2] - from[2]*to[1];
    axis[1] = from[2]*to[0] - from[0]*to[2];
    axis[2] = from[0]*to[1] - from[1]*to[0];
    float q[4] = {axis[0], axis[1], axis[2], 1.0f + dot};
    float length = sqrtf(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
    if (length < 1e-8f) { out[0] = out[1] = out[2] = 0; out[3] = 1; return; }
    for (int i = 0; i < 4; ++i) out[i] = q[i] / length;
}

/* Normalized lerp rather than true slerp: it is a few instructions instead of
   a trigonometric pair, and between two keyframes a fraction of a second apart
   the difference in angular rate is not visible on a limb. The sign flip keeps
   the blend on the short arc, without which a quaternion pair that crossed
   zero would spin the joint the long way round. */
static inline void quat_nlerp(const float a[4], const float b[4], float t, float out[4]) {
    float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    float sign = dot < 0 ? -1.0f : 1.0f;
    float length = 0;
    for (int i = 0; i < 4; ++i) {
        out[i] = a[i] + (b[i] * sign - a[i]) * t;
        length += out[i] * out[i];
    }
    length = sqrtf(length);
    if (length > 1e-8f)
        for (int i = 0; i < 4; ++i) out[i] /= length;
    else
        out[0] = out[1] = out[2] = 0, out[3] = 1;
}

#endif
