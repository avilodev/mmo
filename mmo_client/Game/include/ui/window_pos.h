#ifndef WINDOW_POS_H
#define WINDOW_POS_H

// Where a window sits and how it is dragged by its title. No GL.

// A window that starts centred and, once dragged, stays where it was put
typedef struct {
	float x;
	float y;
	int placed;
	int dragging;
	float grab_x;
	float grab_y;
} window_pos;

// Back to centred and not held
void window_pos_reset(window_pos* pos);

// The top left corner of a window of size w by h in a view of view_w by view_h
void window_pos_origin(const window_pos* pos, float view_w, float view_h, float w, float h,
                       float* out_x, float* out_y);

// Drag by the title strip (strip_w by strip_h at the top left); true while held
int window_pos_drag(window_pos* pos, float view_w, float view_h, float w, float h,
                    float strip_w, float strip_h, float mx, float my, int clicked, int down);

// The same for a window that starts at home_x, home_y, such as one in a corner
int window_pos_drag_from(window_pos* pos, float home_x, float home_y, float view_w,
                         float view_h, float w, float h, float strip_w, float strip_h, float mx,
                         float my, int clicked, int down);

// Whether a point is on a window's outline: within band of its border
int window_pos_on_edge(float x, float y, float w, float h, float band, float mx, float my);

// Carry a window from home_x, home_y; may_pick says a press now picks it up
int window_pos_carry(window_pos* pos, float home_x, float home_y, float view_w, float view_h,
                     float w, float h, int may_pick, float mx, float my, int down);

#endif /* WINDOW_POS_H */
