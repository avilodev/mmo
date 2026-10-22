#ifndef WORLD_FOOTING_H
#define WORLD_FOOTING_H

// Fine floor heights under the feet; visual only, flat tiles use the spans

#define FOOTING_PATH "Game/assets/baked/test_town_footing.bin"

// Cells further apart in height than this are a tread edge, not a slope
#define FOOTING_EDGE_UNITS 2.0f

// A rise over `reach` units beyond 45 degrees plus this is a step, not slope
#define FOOTING_SLOPE_SLACK 0.5f

// Load the map; 1 on success, else 0 (logged) and queries use the spans
int footing_load(const char* path);
void footing_free(void);
int footing_loaded(void);

// Floor nearest hint_z, none more than a step above it; 0 leaves out_z alone
int footing_height_at(float x, float y, float hint_z, float* out_z);

// Floor rise per unit along the unit heading, and along (-dir_y, dir_x)
void footing_slope_at(float x, float y, float hint_z, float dir_x, float dir_y,
                      float reach, float* along, float* across);

#endif /* WORLD_FOOTING_H */
