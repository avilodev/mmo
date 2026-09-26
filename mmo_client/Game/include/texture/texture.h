#ifndef TEXTURE_H
#define TEXTURE_H

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

// Load a texture from file and return OpenGL texture ID
// Returns 0 if loading fails
unsigned int texture_load(const char* filepath);

/** Where a tileset's tiles sit in the texture texture_load_tileset() built. */
typedef struct {
    int tile_w, tile_h;   /**< Tile size in texels. */
    int pad;              /**< Gutter texels around each tile (texture_atlas.h). */
} TilesetLayout;

/** Load a tileset for the ground: gutter-padded, mipmapped, anisotropic.
 *
 * Viewed at an angle the ground is minified, which shimmers without mipmaps and
 * bleeds neighbouring tiles with them unless each tile carries a gutter of its
 * own edge pixels. Magnification stays nearest, so pixel art stays crisp up
 * close (Next_steps/3d_refactor.md, D16).
 *
 * @param cols,rows  Tiles across and down, from the world header.
 * @param layout     Filled with where the tiles ended up.
 * @return The texture, or 0 when the image cannot be loaded. An image that does
 *         not divide into cols x rows tiles loads unpadded and unmipmapped.
 */
unsigned int texture_load_tileset(const char* filepath, int cols, int rows,
                                  TilesetLayout* layout);

// Unload a texture and free GPU memory
void texture_unload(unsigned int texture_id);

// Get texture dimensions (optional, for checking size)
void texture_get_size(unsigned int texture_id, int* width, int* height);

#endif // TEXTURE_H