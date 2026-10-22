#ifndef WORLD_SPANS_FORMAT_H
#define WORLD_SPANS_FORMAT_H

// The sidecar beside world.dat: what can be stood on, and where.
// Native-endian, like world.dat. Layout:
//   header, 32 bytes: magic[8], u32 version, i32 width, i32 height,
//     i32 tile_size, u32 tile_count, u32 span_count
//   tile records, 12 bytes each: u32 tile_index, u32 first_span,
//     u32 span_count. Sorted by tile_index, no repeats, and first_span
//     runs on from the previous record.
//   spans, 8 bytes each: f32 floor, f32 ceil. Ascending, not overlapping.
// A tile with no record is flat open ground: floor 0, no ceiling.

#include <stdint.h>

#define WORLD_SPANS_MAGIC          "MMOSPANS"
#define WORLD_SPANS_MAGIC_LEN      8
#define WORLD_SPANS_VERSION        1
#define WORLD_SPANS_HEADER_BYTES   32
#define WORLD_SPANS_TILE_BYTES     12
#define WORLD_SPANS_SPAN_BYTES     8
#define WORLD_SPANS_MAX_TILES      16000000u
#define WORLD_SPANS_MAX_SPANS      64000000u
#define WORLD_SPANS_MAX_PER_TILE   8

#define WORLD_BODY_HEIGHT          40.0f
#define WORLD_STEP_HEIGHT          12.0f
#define WORLD_Z_TOLERANCE          1.0f
#define WORLD_CLEAR_EPS            0.01f
#define WORLD_SPAN_OPEN_CEIL       1.0e9f
// A steep floor (the mask's slope too steep to walk) higher than this above the
// feet is a slope to slide down, never one to climb
#define WORLD_STEEP_RISE           1.0f

#endif /* WORLD_SPANS_FORMAT_H */
