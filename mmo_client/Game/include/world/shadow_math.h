#ifndef WORLD_SHADOW_MATH_H
#define WORLD_SHADOW_MATH_H

// The matrices for one shadow map: an orthographic box around a point, looking
// along the light. No GL. All matrices are column-major, over GL world space
// (+X east, +Y up, +Z south).

typedef struct {
	float view[16];      // world to the light's eye
	float proj[16];      // eye to clip
	float viewproj[16];  // proj * view
	float tex[16];       // world to shadow texture: u, v and depth all in 0..1
	float radius;        // half the width of the box, world units
	float texel;         // world units across one texel of the map
	float depth_range;   // world units between the near and the far plane
} shadow_cascade;

// Fit a box of half-width `radius` round `center`, for a map `map_size` texels
// across, lit from the unit direction `light_dir` (toward the light). The box
// slides in whole texels, so a shadow's edge does not shimmer as the centre moves.
void shadow_cascade_fit(shadow_cascade* out, const float light_dir[3], const float center[3],
                        float radius, int map_size);

// Where a world point lands on the map: u and v in 0..1 inside the box, and its
// depth in 0..1. Returns nonzero when the point is inside the box.
int shadow_cascade_project(const shadow_cascade* c, const float point[3], float out_uvz[3]);

// Whether a map `frames_old` old (negative: never drawn) must be drawn again
int shadow_cascade_redraw_due(int frames_old, float moved, float radius, int interval);

#endif /* WORLD_SHADOW_MATH_H */
