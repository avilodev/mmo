// The matrices for a shadow map that follows a point.

#include "world/shadow_math.h"

#include <math.h>
#include <string.h>

// How far toward the light the box reaches, to catch what casts into it
#define SHADOW_REACH_TOWARD_LIGHT 900.0f
#define SHADOW_NEAR 0.5f
// A kept map is redrawn once its centre strays this many radii
#define SHADOW_REDRAW_MOVED 0.2f

static void cross3(const float* a, const float* b, float* out) {
	out[0] = a[1] * b[2] - a[2] * b[1];
	out[1] = a[2] * b[0] - a[0] * b[2];
	out[2] = a[0] * b[1] - a[1] * b[0];
}

static float dot3(const float* a, const float* b) {
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void normalize3(float* v) {
	float len = sqrtf(dot3(v, v));

	if(len < 1.0e-6f)
		return;
	for(int i = 0; i < 3; i++)
		v[i] /= len;
}

static void multiply(const float* a, const float* b, float* out) {
	float r[16];

	for(int col = 0; col < 4; col++)
		for(int row = 0; row < 4; row++) {
			float sum = 0.0f;

			for(int k = 0; k < 4; k++)
				sum += a[k * 4 + row] * b[col * 4 + k];
			r[col * 4 + row] = sum;
		}
	memcpy(out, r, sizeof(r));
}

static float snap(float v, float step) {
	return floorf(v / step + 0.5f) * step;
}

void shadow_cascade_fit(shadow_cascade* out, const float light_dir[3], const float center[3],
                        float radius, int map_size) {
	float z[3] = { light_dir[0], light_dir[1], light_dir[2] };
	float up[3] = { 0.0f, 1.0f, 0.0f };
	float x[3], y[3];
	float texel = 2.0f * radius / (float)map_size;
	float far_plane, cx, cy, cz;
	float ortho[16] = { 0 };
	float bias[16] = { 0 };

	normalize3(z);
	// Looking straight down or up, "up" must be something else
	if(fabsf(z[1]) > 0.99f) {
		up[1] = 0.0f;
		up[2] = -1.0f;
	}
	cross3(up, z, x);
	normalize3(x);
	cross3(z, x, y);

	cx = snap(dot3(x, center), texel);
	cy = snap(dot3(y, center), texel);
	cz = dot3(z, center);
	far_plane = SHADOW_REACH_TOWARD_LIGHT + radius * 1.2f + 50.0f;

	memset(out, 0, sizeof(*out));
	for(int i = 0; i < 3; i++) {
		out->view[i * 4 + 0] = x[i];
		out->view[i * 4 + 1] = y[i];
		out->view[i * 4 + 2] = z[i];
	}
	out->view[12] = -cx;
	out->view[13] = -cy;
	out->view[14] = -(cz + SHADOW_REACH_TOWARD_LIGHT);
	out->view[15] = 1.0f;

	ortho[0] = 1.0f / radius;
	ortho[5] = 1.0f / radius;
	ortho[10] = -2.0f / (far_plane - SHADOW_NEAR);
	ortho[14] = -(far_plane + SHADOW_NEAR) / (far_plane - SHADOW_NEAR);
	ortho[15] = 1.0f;
	memcpy(out->proj, ortho, sizeof(ortho));
	multiply(out->proj, out->view, out->viewproj);

	bias[0] = bias[5] = bias[10] = 0.5f;
	bias[12] = bias[13] = bias[14] = 0.5f;
	bias[15] = 1.0f;
	multiply(bias, out->viewproj, out->tex);

	out->radius = radius;
	out->texel = texel;
	out->depth_range = far_plane - SHADOW_NEAR;
}

int shadow_cascade_project(const shadow_cascade* c, const float point[3], float out_uvz[3]) {
	for(int row = 0; row < 3; row++)
		out_uvz[row] = c->tex[row] * point[0] + c->tex[4 + row] * point[1] +
		               c->tex[8 + row] * point[2] + c->tex[12 + row];

	return out_uvz[0] >= 0.0f && out_uvz[0] <= 1.0f && out_uvz[1] >= 0.0f &&
	       out_uvz[1] <= 1.0f && out_uvz[2] >= 0.0f && out_uvz[2] <= 1.0f;
}

int shadow_cascade_redraw_due(int frames_old, float moved, float radius, int interval) {
	if(frames_old < 0)
		return 1;

	return frames_old >= interval || moved > radius * SHADOW_REDRAW_MOVED;
}
