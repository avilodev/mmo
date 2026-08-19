#ifndef WORLD_OVERVIEW_H
#define WORLD_OVERVIEW_H

/**
 * Downscaled whole-world image used by the full map screen.
 *
 * The streamed world file is ~1 GB, so it cannot back a continent-scale view.
 * The generator writes a companion overview next to it (one palette index per
 * NxN tile block) which is small enough to load once and upload as a texture.
 */

/**
 * Load the overview that accompanies a world file.
 *
 * Looks for "<world_file_path>.overview". A missing or malformed file is not an
 * error: the map simply falls back to its grid-only rendering.
 *
 * @param world_file_path  Path passed to world_init.
 * @return                 1 if an overview is now available, otherwise 0.
 */
int world_overview_load(const char* world_file_path);

/** Release the overview texture and buffers. Safe to call when none loaded. */
void world_overview_unload(void);

/** @return Nonzero once an overview texture is ready to draw. */
int world_overview_ready(void);

/** @return The overview's OpenGL texture, or 0 when none is loaded. */
unsigned int world_overview_texture(void);

/**
 * Report overview geometry.
 *
 * @param out_width   Receives width in cells, may be NULL.
 * @param out_height  Receives height in cells, may be NULL.
 * @param out_scale   Receives tiles per cell, may be NULL.
 */
void world_overview_dims(int* out_width, int* out_height, int* out_scale);

#endif // WORLD_OVERVIEW_H
