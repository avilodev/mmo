#ifndef TEXTURE_ATLAS_H
#define TEXTURE_ATLAS_H

/**
 * @file
 * Pad a tileset atlas with per-tile gutters so it can be mipmapped.
 *
 * Pure pixel arithmetic, no OpenGL: texture.c uploads the result.
 */

/** Copy every tile into a cell `pad` pixels larger on each side, filling the
 *  gutter by repeating the tile's own edge pixels.
 *
 * @param src       Tightly packed pixels, `channels` bytes each.
 * @param cols,rows Tiles across and down; must divide w and h exactly.
 * @return A malloc'd image of *out_w x *out_h, or NULL when the atlas does not
 *         divide into whole tiles or allocation fails.
 */
unsigned char* texture_atlas_pad(const unsigned char* src, int w, int h, int channels,
                                 int cols, int rows, int pad, int* out_w, int* out_h);

/** Normalized UVs of tile `index` (row-major) in an atlas padded by `pad`. */
void texture_atlas_uv(int cols, int rows, int tile_w, int tile_h, int pad, int index,
                      float* u0, float* v0, float* u1, float* v1);

/** Mip levels past the base for which a `pad`-pixel gutter is still at least
 *  one pixel wide, so no level samples a neighbouring tile. */
int texture_atlas_mip_levels(int pad);

#endif /* TEXTURE_ATLAS_H */
