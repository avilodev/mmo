#ifndef WORLD_FORMAT_H
#define WORLD_FORMAT_H

/** @file The on-disk layout of world.dat, declared once for every reader.
 *
 * The file is written by the client's world generator and read by two
 * independent programs: the client, to draw it, and the world server, to load
 * the collision layer that every movement check is validated against.
 *
 * It used to have no magic number and no version, and its layout -- three
 * int32 header fields, a tileset table, four uint16 tile layers, then the
 * collision layer -- was hand-duplicated in all three places. Nothing detected
 * a mismatch. A fifth tile layer added on the client made the server's seek
 * past the tile data land in the middle of it, and the server loaded whatever
 * it found as its collision map: not a crash, not an error, just a world where
 * walls are in the wrong places and movement validation quietly agrees.
 *
 * So the file now says what it is. The magic identifies it, the version gates
 * the layout, and the layer count is read rather than assumed -- which is the
 * part that makes adding a layer safe instead of silently catastrophic.
 *
 * This header is duplicated in both trees, byte for byte, and
 * tests/check_world_format.sh fails the build when the copies drift. That is
 * the same arrangement protocol.h has, for the same reason: the trees are
 * independent and neither can include from the other.
 *
 * ---------------------------------------------------------------------------
 * Layout, in file order. All integers are little-endian, which is what both
 * ends already assumed and now say out loud.
 *
 *   char     magic[8]        WORLD_FORMAT_MAGIC, not NUL-terminated
 *   uint32   version         WORLD_FORMAT_VERSION
 *   uint32   tile_layers     uint16 layers stored before the collision layer
 *   int32    width           tiles
 *   int32    height          tiles
 *   int32    tile_px         pixels per tile
 *   uint8    tileset_count
 *   repeated tileset_count times:
 *       uint8   path_len
 *       char    path[path_len]
 *       uint16  cols
 *       uint16  rows
 *   uint16   tiles[tile_layers][height][width]
 *   uint8    collision[height][width]
 * ---------------------------------------------------------------------------
 */

#include <stdint.h>

/** Identifies the file. Eight bytes, not NUL-terminated. */
#define WORLD_FORMAT_MAGIC     "MMOWORLD"
#define WORLD_FORMAT_MAGIC_LEN 8

/** Bump when the layout changes in a way an older reader cannot handle.
 *
 * Adding a tile layer does NOT need a bump: the count is in the header and
 * every reader seeks by it. Changing a field's width or order does.
 */
#define WORLD_FORMAT_VERSION 1

/** Tile layers the generator currently writes: base, floor, interior, above.
 *
 * A default for the writer, never an assumption for a reader. Readers take the
 * count from the file.
 */
#define WORLD_FORMAT_TILE_LAYERS 4

/** Bytes before the width field: magic + version + tile_layers. */
#define WORLD_FORMAT_PREAMBLE_BYTES (WORLD_FORMAT_MAGIC_LEN + 4 + 4)

/** Refuse absurd dimensions rather than trying to allocate them.
 *
 * A width and height are read from a file and multiplied to size an
 * allocation, so an implausible header is a request to allocate an implausible
 * amount of memory. The shipped world is 15400x7700 tiles; this is far above
 * that and far below anything that could be mistaken for a sane world.
 */
#define WORLD_FORMAT_MAX_DIMENSION 1000000

/** Refuse a tile *count* no world could have, independent of its shape.
 *
 * The per-axis limit above bounds each dimension but not their product: a
 * header declaring 1,000,000 x 1,000,000 passes it and then asks for a
 * terabyte. The allocation is sized by the product, so the product is what has
 * to be bounded. The shipped world is 15400x7700 = ~119 million tiles; a
 * billion is comfortably above any world either program would be asked to
 * load and comfortably below an allocation that would take the machine down
 * with it.
 */
#define WORLD_FORMAT_MAX_TILES 1000000000ULL

/** Refuse a layer count no generator would write. */
#define WORLD_FORMAT_MAX_TILE_LAYERS 64

#endif // WORLD_FORMAT_H
