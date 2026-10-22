// The views a lamp's shadow is drawn from.

#include "world/lamp_shadow_math.h"

#include <math.h>
#include <string.h>

static const float FACE_DIR[LAMP_SHADOW_FACES][3] = {
	{ 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 },
};
static const float FACE_UP[LAMP_SHADOW_FACES][3] = {
	{ 0, 1, 0 }, { 0, 1, 0 }, { 0, 0, -1 }, { 0, 0, 1 }, { 0, 1, 0 }, { 0, 1, 0 },
};

const float* lamp_shadow_face_dir(int face) {
	return FACE_DIR[face];
}

const float* lamp_shadow_face_up(int face) {
	return FACE_UP[face];
}

static void cross3(const float* a, const float* b, float* out) {
	out[0] = a[1] * b[2] - a[2] * b[1];
	out[1] = a[2] * b[0] - a[0] * b[2];
	out[2] = a[0] * b[1] - a[1] * b[0];
}

static float dot3(const float* a, const float* b) {
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
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

void lamp_shadow_fit(const float pos[3], float near_plane, float far_plane,
                     float viewproj[LAMP_SHADOW_FACES][16]) {
	float tan_half = 1.0f + LAMP_SHADOW_MARGIN;

	for(int face = 0; face < LAMP_SHADOW_FACES; face++) {
		const float* d = FACE_DIR[face];
		float right[3], view[16] = { 0 }, proj[16] = { 0 };

		cross3(d, FACE_UP[face], right);
		for(int i = 0; i < 3; i++) {
			view[i * 4 + 0] = right[i];
			view[i * 4 + 1] = FACE_UP[face][i];
			view[i * 4 + 2] = -d[i];
		}
		view[12] = -dot3(right, pos);
		view[13] = -dot3(FACE_UP[face], pos);
		view[14] = dot3(d, pos);
		view[15] = 1.0f;

		proj[0] = 1.0f / tan_half;
		proj[5] = 1.0f / tan_half;
		proj[10] = -(far_plane + near_plane) / (far_plane - near_plane);
		proj[11] = -1.0f;
		proj[14] = -2.0f * far_plane * near_plane / (far_plane - near_plane);
		multiply(proj, view, viewproj[face]);
	}
}

int lamp_shadow_locate(const float v[3], float* ndc_x, float* ndc_y, float* dist) {
	float a[3] = { fabsf(v[0]), fabsf(v[1]), fabsf(v[2]) };
	float right[3];
	int face;

	if(a[0] >= a[1] && a[0] >= a[2])
		face = v[0] > 0.0f ? 0 : 1;
	else if(a[1] >= a[2])
		face = v[1] > 0.0f ? 2 : 3;
	else
		face = v[2] > 0.0f ? 4 : 5;

	cross3(FACE_DIR[face], FACE_UP[face], right);
	*dist = dot3(v, FACE_DIR[face]);
	*ndc_x = dot3(v, right) / *dist / (1.0f + LAMP_SHADOW_MARGIN);
	*ndc_y = dot3(v, FACE_UP[face]) / *dist / (1.0f + LAMP_SHADOW_MARGIN);

	return face;
}

float lamp_shadow_depth(float dist, float near_plane, float far_plane) {
	float ndc = (far_plane + near_plane) / (far_plane - near_plane) -
	            2.0f * far_plane * near_plane / ((far_plane - near_plane) * dist);

	return ndc * 0.5f + 0.5f;
}
