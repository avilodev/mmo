#ifndef COVER_FIELD_H
#define COVER_FIELD_H

// The height of the highest surface over every spot of the city: a roof, an
// awning, or just the ground. Rain and snow stop at it, so anything standing
// lower is under cover. A coarse copy of the top-down map the renderer draws.
// No GL. Positions are world (x, y); heights are up.

// Keep a grid of heights, row by row from (x0, y0), each cell cell_w by
// cell_h. A copy is made. A height far below the ground means nothing is there.
void cover_field_set(const float* heights, int width, int height, float x0, float y0,
                     float cell_w, float cell_h);

// Forget the grid, as when the city is unloaded
void cover_field_clear(void);

// The highest surface over (x, y). Returns 0 when the grid does not reach it.
int cover_field_height(float x, float y, float* out);

// Whether something stands more than clearance above a point at height h. A
// point the grid does not reach is in the open.
int cover_field_covered(float x, float y, float h, float clearance);

#endif /* COVER_FIELD_H */
