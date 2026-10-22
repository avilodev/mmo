#ifndef LETTERBOX_H
#define LETTERBOX_H

/**
 * @file
 * Fit the game's fixed-shape view inside any window without stretching it.
 *
 * The game draws into a 1920x1080 view. Whatever the window or framebuffer
 * size, the view is scaled evenly and centred, and the leftover is bars.
 * No GL here, so it can be tested on its own.
 */

typedef struct {
	int x;
	int y;
	int w;
	int h;
} letterbox_rect;

// The largest box of the view's shape that fits a target, centred in it
letterbox_rect letterbox_fit(int target_w, int target_h, int view_w, int view_h);

// A point in the target (a cursor, in the same pixels the box was fitted in) as a point in the view
void letterbox_to_view(const letterbox_rect* box, double px, double py,
                       int view_w, int view_h, float* x, float* y);

#endif /* LETTERBOX_H */
