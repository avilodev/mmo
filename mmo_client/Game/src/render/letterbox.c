/**
 * @file
 * Fit the game's fixed-shape view inside any window without stretching it.
 */
#include "render/letterbox.h"

letterbox_rect letterbox_fit(int target_w, int target_h, int view_w, int view_h) {
	letterbox_rect box = { 0, 0, target_w, target_h };

	if(target_w <= 0 || target_h <= 0 || view_w <= 0 || view_h <= 0)
		return box;

	// Compare target_w / target_h with view_w / view_h without dividing
	if((long long)target_w * view_h <= (long long)target_h * view_w) {
		box.h = (int)((long long)target_w * view_h / view_w);
	} else {
		box.w = (int)((long long)target_h * view_w / view_h);
	}

	box.x = (target_w - box.w) / 2;
	box.y = (target_h - box.h) / 2;

	return box;
}

void letterbox_to_view(const letterbox_rect* box, double px, double py,
                       int view_w, int view_h, float* x, float* y) {
	if(box->w <= 0 || box->h <= 0) {
		*x = (float)px;
		*y = (float)py;

		return;
	}

	*x = (float)((px - box->x) * view_w / box->w);
	*y = (float)((py - box->y) * view_h / box->h);
}
