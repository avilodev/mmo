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

/** Give every fully transparent pixel the colour of the opaque pixels around it.
 *
 * Smoothing a small image with transparent black beside its edges averages that
 * black into them, which darkens and fades thin parts such as sun rays. Giving
 * the transparent pixels their neighbours' colour first (alpha is not changed,
 * so nothing new shows) keeps the edge colours true when the image is mipmapped.
 *
 * @param rgba    Tightly packed 8-bit RGBA pixels, changed in place.
 * @param passes  How many pixels out from the opaque ones to reach.
 */
void texture_bleed_color(unsigned char* rgba, int w, int h, int passes);

#endif /* TEXTURE_ATLAS_H */
