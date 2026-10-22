#ifndef WORLD_SPANS_H
#define WORLD_SPANS_H

// What can be stood on at a world point, and what a body can reach.
// Pure data and arithmetic: no logging, no globals beyond the loaded file.
// Every function fails closed outside the world and on non-finite input.

typedef struct {
	float floor;
	float ceil;
} world_span;

// Set the world extent and drop any loaded spans (flat ground everywhere).
void world_spans_init(int width_tiles, int height_tiles, int tile_size);

// Load the sidecar. It must match the extent given to world_spans_init.
// Returns 1 on success. On failure returns 0 and keeps the old tables.
int world_spans_load(const char* path);

int world_spans_loaded(void);

// The fine floor mask (read from beside the spans) trims spans to its cells
int world_spans_load_mask(const char* path);
int world_spans_mask_loaded(void);

// The tile the mask file's coordinates are counted from (the city origin)
void world_spans_set_mask_origin(int tile_x, int tile_y);
void world_spans_shutdown(void);

// Copy up to max spans of the tile holding (x, y) with floor at (x, y); the count
int world_spans_at(float x, float y, world_span* out, int max);

// Returns 1 (success) when a floor within one step of from_z has headroom.
int world_spans_reach(float x, float y, float from_z, float* out_z);

// As reach, for the body box: every point tested around its edge (each
// edge at 4 points, corners included) must hold the same floor.
int world_spans_reach_box(float x, float y, float half, float from_z,
                          float* out_z);

// Walk the box from (x0, y0, z0) to (x1, y1) in half-tile samples.
// Returns 1 when every sample is reachable; out_z is the final floor.
int world_spans_walk(float x0, float y0, float z0, float x1, float y1,
                     float half, float* out_z);

// Nearest usable floor to hint_z, with no step limit. Returns 0 if none.
int world_spans_snap(float x, float y, float hint_z, float* out_z);

// Every edge point of the box holds a body standing at z.
int world_spans_fits_box(float x, float y, float half, float z);

// Every edge point of the box leaves room for a body at z, floor possibly far below.
int world_spans_free_box(float x, float y, float half, float z);

// As free, but no floor may stand above z: the box overlaps no ledge's face.
int world_spans_clear_box(float x, float y, float half, float z);

// Highest floor in [z_lo, z_hi] of the centre column the box fits on.
int world_spans_floor_between(float x, float y, float half, float z_lo,
                              float z_hi, float* out_z);

// Highest floor in [z_lo, z_hi] under any edge point the box can stand at free
int world_spans_support(float x, float y, float half, float z_lo, float z_hi,
                        float* out_z);

// The centre column holds a steep floor (a slope too steep to walk) above the
// feet at z and within a step: the slope ahead cannot be climbed.
int world_spans_steep_above(float x, float y, float z);
// A body standing at z on a steep floor at (x, y) slides: 1, the downhill
// direction (unit length) and, if grade is not NULL, the rise per unit run.
// Else 0.
int world_spans_slide(float x, float y, float z, float* dir_x, float* dir_y, float* grade);
// z is inside some span's clear interval (a point, not a body).
int world_spans_free_point(float x, float y, float z);

#endif /* WORLD_SPANS_H */
