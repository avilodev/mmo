#ifndef WORLD_LAMP_SHADOW_MATH_H
#define WORLD_LAMP_SHADOW_MATH_H

// The six views a lamp's shadow is drawn from: one square view along each axis,
// like the faces of a cube round the lamp. No GL. The shaders pick a face for
// a surface the same way lamp_shadow_locate() does, from the same face table.
// Directions are GL world space: +X east, +Y up, +Z south. Matrices are
// column-major.

#define LAMP_SHADOW_FACES 6
// Each face sees a little past its 90 degrees, so soft-edge samples near the
// edge of a face still land inside it. A face covers x/d and y/d in
// -(1 + margin) .. (1 + margin).
#define LAMP_SHADOW_MARGIN 0.01f

// The direction and up of a face, 0..5 as +X, -X, +Y, -Y, +Z, -Z
const float* lamp_shadow_face_dir(int face);
const float* lamp_shadow_face_up(int face);

// Fit the six views round a lamp at `pos`: they see from `near_plane` to
// `far_plane` away, along each face's own axis.
void lamp_shadow_fit(const float pos[3], float near_plane, float far_plane,
                     float viewproj[LAMP_SHADOW_FACES][16]);

// Which face sees the offset `v` from the lamp, and where in it. Returns the
// face 0..5, with the position in the face's clip space (-1..1) and the
// distance along its axis.
int lamp_shadow_locate(const float v[3], float* ndc_x, float* ndc_y, float* dist);

// The depth (0..1) a face stores for a surface `dist` away along its axis
float lamp_shadow_depth(float dist, float near_plane, float far_plane);

#endif /* WORLD_LAMP_SHADOW_MATH_H */
