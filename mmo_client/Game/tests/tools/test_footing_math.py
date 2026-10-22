import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))

import struct
import unittest

from footing_math import (CELLS_PER_TILE, NONE, OPEN, STEEP, add_levels, assign_level, bake_tile,
                          encode_tile, steep_levels,
                          clear_border, close_gaps, close_seams, encode_level,
                          is_flat, pack_file)

N = CELLS_PER_TILE * CELLS_PER_TILE
BIG = 1.0e9


def hits_of(fn):
    """Per cell, the list of (floor, ceil) hits fn(i, j) gives."""
    return [fn(i, j) for j in range(CELLS_PER_TILE) for i in range(CELLS_PER_TILE)]


def level_of(floor, holes=()):
    """A level at one floor with open sky, absent at the `holes` cells."""
    level = [(floor, OPEN)] * N
    for ci, cj in holes:
        level[cj * CELLS_PER_TILE + ci] = None
    return level


def tile_of(floors, holes=()):
    """One tile's (floors, levels), every level absent at `holes`."""
    return (list(floors), [level_of(floor, holes) for floor in floors])


def cell(tiles, tile, ci, cj, k=0):
    return tiles[tile][1][k][cj * CELLS_PER_TILE + ci]


class FootingMathTest(unittest.TestCase):
    def test_a_flat_tile_stores_nothing(self):
        self.assertEqual(bake_tile(hits_of(lambda i, j: [(10.0, BIG)]), [10.0]), [])

    def test_a_staircase_stores_every_tread(self):
        hits = hits_of(lambda i, j: [(float(i // 4 * 4), BIG)])
        ((base, deltas, clears),) = bake_tile(hits, [6.0])
        self.assertEqual(base, 0.0)
        self.assertEqual(len(deltas), N)
        self.assertEqual(deltas[0], 0)
        self.assertEqual(deltas[4], 16)    # 4 units, in quarter units
        self.assertEqual(deltas[15], 48)

    def test_a_ramp_stores_its_slope(self):
        hits = hits_of(lambda i, j: [(i * 0.5, BIG)])
        ((base, deltas, clears),) = bake_tile(hits, [4.0])
        self.assertEqual(base, 0.0)
        self.assertEqual(deltas[10], 20)

    def test_the_hit_nearest_the_span_is_the_cells_floor(self):
        self.assertEqual(assign_level([[(0.0, BIG), (10.0, BIG)]], 9.0), [(10.0, OPEN)])

    def test_a_hit_further_than_a_tile_of_rise_is_not_this_levels(self):
        self.assertEqual(assign_level([[(0.0, BIG), (74.0, BIG)]], 0.0), [(0.0, OPEN)])
        self.assertEqual(assign_level([[(74.0, BIG)]], 0.0), [None])

    def test_the_clearance_above_each_floor_is_kept(self):
        # a deck 60 above the floor: 60 units of room, in 2-unit steps
        hits = hits_of(lambda i, j: [(0.0, 60.0)] if i < 8 else [(0.0, BIG)])
        ((base, deltas, clears),) = bake_tile(hits, [0.0])
        self.assertEqual(clears[0], 30)
        self.assertEqual(clears[15], 255)

    def test_a_tile_under_a_deck_keeps_its_clearance_even_when_flat(self):
        self.assertNotEqual(bake_tile(hits_of(lambda i, j: [(0.0, 60.0)]), [0.0]), [])

    def test_the_stored_ceiling_is_never_below_the_true_one(self):
        # a passage with 40.5 of room: rounding the clearance down to 40 would
        # leave the span unusable (40 or more is needed) once the floor is a hair low
        for clear in (40.0, 40.5, 41.9, 43.0):
            _, _, clears = encode_level([(0.4371, clear)])
            self.assertGreaterEqual(clears[0] * 2.0, clear)
            self.assertLess(clears[0] * 2.0, clear + 2.0)

    def test_a_cell_with_no_floor_is_none(self):
        hits = hits_of(lambda i, j: [] if i == 3 else [(i * 0.5, BIG)])
        ((base, deltas, clears),) = bake_tile(hits, [4.0])
        self.assertEqual(deltas[3], NONE)
        self.assertNotEqual(deltas[4], NONE)

    def test_a_floor_too_far_above_the_base_cannot_be_encoded(self):
        with self.assertRaises(ValueError):
            encode_level([(0.0, OPEN), (100.0, OPEN)])

    def test_a_ramp_under_a_bridge_stores_two_levels(self):
        hits = hits_of(lambda i, j: [(i * 0.5, BIG), (74.0, BIG)])
        levels = bake_tile(hits, [4.0, 74.0])
        self.assertEqual(len(levels), 2)
        self.assertEqual(levels[0][0], 0.0)
        self.assertEqual(levels[1][0], 74.0)

    def test_levels_stay_aligned_with_the_tiles_spans(self):
        # nothing near the second span (74): its level is kept, wholly absent
        hits = hits_of(lambda i, j: [(i * 0.5, BIG)])
        levels = bake_tile(hits, [4.0, 74.0])
        self.assertEqual(len(levels), 2)
        self.assertEqual(levels[1], (0.0, bytes([NONE]) * N, bytes([255]) * N))

    def test_a_flat_tile_with_a_cell_missing_gets_a_record(self):
        hits = hits_of(lambda i, j: [] if (i, j) == (3, 5) else [(10.0, BIG)])
        ((base, deltas, clears),) = bake_tile(hits, [10.0])
        self.assertEqual(deltas[5 * CELLS_PER_TILE + 3], NONE)
        self.assertEqual(deltas[0], 0)

    def test_a_tile_whose_every_cell_is_missing_is_a_record_of_none(self):
        ((base, deltas, clears),) = bake_tile(hits_of(lambda i, j: []), [10.0])
        self.assertEqual(deltas, bytes([NONE]) * N)

    def test_is_flat(self):
        self.assertTrue(is_flat([(1.0, OPEN), (1.2, OPEN), None]))
        self.assertFalse(is_flat([(1.0, OPEN), (2.0, OPEN)]))
        self.assertTrue(is_flat([None, None]))

    def test_the_file_layout(self):
        levels = [(0.0, bytes(N), bytes([20]) * N)]
        data = pack_file(-2, -3, 2, 1, {(-1, -3): levels})
        self.assertEqual(data[:4], b"CFTG")
        version, min_x, min_y, width, height, cells, count = struct.unpack("<IiiiiiI", data[4:32])
        self.assertEqual((version, min_x, min_y, width, height, cells, count),
                         (4, -2, -3, 2, 1, CELLS_PER_TILE, 1))
        body = data[32:]
        self.assertEqual(body[0], 0)                      # tile (-2, -3): flat
        self.assertEqual(body[1], 1)                      # tile (-1, -3): one level
        self.assertEqual(struct.unpack("<f", body[2:6])[0], 0.0)
        self.assertEqual(body[6 + N], 20)                 # the first clearance byte
        self.assertEqual(len(body), 1 + 1 + 4 + 2 * N)


class CloseGapsTest(unittest.TestCase):
    def test_a_one_cell_hole_between_floors_is_filled(self):
        out, filled = close_gaps({(0, 0): tile_of([10.0], [(5, 5)])})
        self.assertEqual(filled, 1)
        self.assertEqual(cell(out, (0, 0), 5, 5), (10.0, OPEN))

    def test_the_fill_goes_into_the_span_the_floor_beside_it_belongs_to(self):
        # a rim sliver between a plateau at 100 and the ground at 0, in a tile
        # with both spans: the lower flank is the ground
        plateau = [None if ci >= 7 else (100.0, OPEN) for cj in range(16) for ci in range(16)]
        ground = [None if ci <= 7 else (0.0, OPEN) for cj in range(16) for ci in range(16)]
        out, filled = close_gaps({(0, 0): ([0.0, 100.0], [ground, plateau])})
        self.assertEqual(filled, CELLS_PER_TILE)
        self.assertEqual(cell(out, (0, 0), 7, 3, k=0), (0.0, OPEN))
        self.assertIsNone(cell(out, (0, 0), 7, 3, k=1))

    def test_a_plateau_only_tile_keeps_its_floor_beside_ground(self):
        # the tile has only the plateau span; the ground flank belongs to the
        # next tile. The sliver is filled at the plateau's height, never the
        # ground's, so the plateau cells stay present and encodable.
        plateau = [None if ci == 15 else (406.8, OPEN) for cj in range(16) for ci in range(16)]
        tiles = {(0, 0): ([406.8], [plateau]), (1, 0): tile_of([0.4])}
        out, filled = close_gaps(tiles)
        for ci in range(15):
            self.assertEqual(cell(out, (0, 0), ci, 4), (406.8, OPEN))
        self.assertEqual(cell(out, (0, 0), 15, 4), (406.8, OPEN))
        encode_level(out[(0, 0)][1][0])       # must not raise

    def test_a_hole_with_no_matching_span_is_left(self):
        # the floors beside the hole are at 10 but the tile's only span is at 200
        level = level_of(10.0, [(5, 5)])
        out, filled = close_gaps({(0, 0): ([200.0], [level])})
        self.assertEqual(filled, 0)
        self.assertIsNone(cell(out, (0, 0), 5, 5))

    def test_a_hole_wider_than_the_limit_in_both_directions_is_left(self):
        block4 = [(ci, cj) for ci in range(4, 8) for cj in range(4, 8)]
        out, filled = close_gaps({(0, 0): tile_of([10.0], block4)}, max_gap=3)
        self.assertEqual(filled, 0)
        block3 = [(ci, cj) for ci in range(4, 7) for cj in range(4, 7)]
        out, filled = close_gaps({(0, 0): tile_of([10.0], block3)}, max_gap=3)
        self.assertEqual(filled, 9)

    def test_a_thin_crack_is_closed_across_its_width(self):
        crack = [(ci, 5) for ci in range(2, 12)]      # ten cells long, one wide
        out, filled = close_gaps({(0, 0): tile_of([10.0], crack)})
        self.assertEqual(filled, 10)

    def test_a_hole_across_a_tile_boundary_is_bridged(self):
        tiles = {(0, 0): tile_of([10.0], [(15, 4)]), (1, 0): tile_of([10.0], [(0, 4)])}
        out, filled = close_gaps(tiles)
        self.assertEqual(filled, 2)

    def test_a_hole_at_the_edge_of_the_floor_is_left(self):
        out, filled = close_gaps({(0, 0): tile_of([10.0], [(0, 0)])})
        self.assertEqual(filled, 0)

    def test_the_gap_beside_a_tile_with_no_spans_is_not_bridged(self):
        out, filled = close_gaps({(0, 0): tile_of([10.0], [(15, 0)])})
        self.assertEqual(filled, 0)

    def test_the_input_is_left_alone(self):
        tiles = {(0, 0): tile_of([10.0], [(5, 5)])}
        close_gaps(tiles)
        self.assertIsNone(cell(tiles, (0, 0), 5, 5))


class CloseSeamsTest(unittest.TestCase):
    def two_storeys(self, upper_holes):
        # a ground storey wholly present under an upper floor with holes
        return {(0, 0): ([0.0, 100.0], [level_of(0.0), level_of(100.0, upper_holes)])}

    def test_a_seam_in_an_upper_floor_over_a_present_lower_one_is_closed(self):
        out, filled = close_seams(self.two_storeys([(5, 5)]))
        self.assertEqual(filled, 1)
        self.assertEqual(cell(out, (0, 0), 5, 5, k=1), (100.0, OPEN))

    def test_close_gaps_alone_leaves_that_seam(self):
        out, filled = close_gaps(self.two_storeys([(5, 5)]))
        self.assertEqual(filled, 0)

    def test_a_wide_gap_in_an_upper_floor_is_left(self):
        holes = [(ci, cj) for ci in range(4, 9) for cj in range(4, 9)]
        out, filled = close_seams(self.two_storeys(holes))
        self.assertEqual(filled, 0)

    def test_a_seam_across_a_tile_boundary_is_closed_in_the_matching_level(self):
        tiles = {(0, 0): ([0.0, 100.0], [level_of(0.0), level_of(100.0, [(15, 4)])]),
                 (1, 0): ([0.0, 104.0], [level_of(0.0), level_of(104.0, [(0, 4)])])}
        out, filled = close_seams(tiles)
        self.assertEqual(filled, 2)
        # both cells are filled from the lower of the two floors beside the run
        self.assertEqual(cell(out, (0, 0), 15, 4, k=1)[0], 100.0)
        self.assertEqual(cell(out, (1, 0), 0, 4, k=1)[0], 100.0)

    def test_the_edge_of_a_level_is_not_a_seam(self):
        out, filled = close_seams(self.two_storeys([(0, 0)]))
        self.assertEqual(filled, 0)

    def test_the_input_is_left_alone(self):
        tiles = self.two_storeys([(5, 5)])
        close_seams(tiles)
        self.assertIsNone(cell(tiles, (0, 0), 5, 5, k=1))


class ClearBorderTest(unittest.TestCase):
    def test_the_outer_ring_of_tiles_is_made_whole(self):
        tiles = {(x, y): tile_of([10.0], [(2, 2)]) for x in range(4) for y in range(4)}
        out = clear_border(tiles, 0, 0, 4, 4, ring=1)
        self.assertTrue(all(c == (10.0, OPEN) for c in out[(0, 2)][1][0]))
        self.assertIsNone(cell(out, (1, 1), 2, 2))

    def test_a_wider_ring_reaches_further_in(self):
        tiles = {(x, y): tile_of([10.0], [(2, 2)]) for x in range(5) for y in range(5)}
        out = clear_border(tiles, 0, 0, 5, 5, ring=2)
        self.assertTrue(all(c == (10.0, OPEN) for c in out[(1, 1)][1][0]))
        self.assertIsNone(cell(out, (2, 2), 2, 2))

    def test_every_level_of_a_border_tile_is_made_whole(self):
        tile = ([0.0, 100.0], [[None] * N, [None] * N])
        out = clear_border({(0, 0): tile}, 0, 0, 3, 3, ring=1)
        self.assertEqual(out[(0, 0)][1], [[(0.0, OPEN)] * N, [(100.0, OPEN)] * N])

    def test_the_input_is_left_alone(self):
        tiles = {(0, 0): tile_of([10.0], [(2, 2)])}
        clear_border(tiles, 0, 0, 3, 3, ring=1)
        self.assertIsNone(cell(tiles, (0, 0), 2, 2))



class SteepTest(unittest.TestCase):
    """Cells of a slope too steep to walk: their height, and open air above."""

    def test_a_steep_cell_encodes_as_254(self):
        level = [(0.0, OPEN)] * (N - 1) + [(10.0, STEEP)]
        base, deltas, clears = encode_level(level)
        self.assertEqual((deltas[-1], clears[-1]), (40, 254))
        self.assertEqual(clears[0], 255)

    def test_a_tall_room_never_reads_as_steep(self):
        _, _, clears = encode_level([(0.0, 507.0)] * N)
        self.assertEqual(clears[0], 255)
        _, _, clears = encode_level([(0.0, 505.0)] * N)
        self.assertEqual(clears[0], 253)

    def test_a_steep_level_is_never_plain(self):
        self.assertTrue(encode_tile([[(0.0, STEEP)] * N]))

    def test_the_border_leaves_a_slope_alone(self):
        slope = [(float(i % CELLS_PER_TILE), STEEP) for i in range(N)]
        out = clear_border({(0, 0): ([0.0], [slope])}, 0, 0, 3, 3, ring=1)
        self.assertEqual(out[(0, 0)][1][0], slope)

    def test_a_seam_is_not_filled_from_a_slope(self):
        level = [(0.0, OPEN)] * N
        for j in range(CELLS_PER_TILE):
            level[j * CELLS_PER_TILE + 5] = None
            level[j * CELLS_PER_TILE + 4] = (2.0, STEEP)
        out, filled = close_seams({(0, 0): ([0.0], [level])})
        self.assertEqual(filled, 0)


class SteepLevelsTest(unittest.TestCase):
    def slope(self, rise=1.5, cols=range(CELLS_PER_TILE)):
        """A slope rising along x: per cell, its height when the column is in cols."""
        return [[(i % CELLS_PER_TILE) * rise] if i % CELLS_PER_TILE in cols else []
                for i in range(N)]

    def test_one_slope_is_one_level_at_its_own_heights(self):
        levels = steep_levels(self.slope())
        self.assertEqual(len(levels), 1)
        floor, level = levels[0]
        self.assertEqual(floor, 0.0)
        self.assertEqual(level[5], (7.5, STEEP))

    def test_a_few_steep_cells_are_not_a_slope(self):
        self.assertEqual(steep_levels(self.slope(cols=[3])[:CELLS_PER_TILE * 4]
                                      + [[]] * (N - CELLS_PER_TILE * 4)), [])

    def test_two_slopes_far_apart_are_two_levels(self):
        cells = [a + [z + 100.0 for z in a] for a in self.slope()]
        self.assertEqual([f for f, _ in steep_levels(cells)], [0.0, 100.0])

    def test_a_slope_joins_the_spans_in_order(self):
        ground = [(0.0, OPEN)] * N
        spans, floors, levels = add_levels([(0.0, 1.0e9), (40.0, 1.0e9)], [ground, ground],
                                           steep_levels(self.slope(rise=2.0)), 8)
        self.assertEqual(floors, [0.0, 0.02, 40.0])
        self.assertEqual(spans, [(0.0, 0.02), (0.02, 40.0), (40.0, 1.0e9)])
        self.assertEqual(levels[1][3], (6.0, STEEP))

    def test_a_full_tile_keeps_its_spans_before_a_slope(self):
        spans = [(100.0 * k, 100.0 * k + 50.0) for k in range(8)]
        out, _, _ = add_levels(spans, [[None] * N] * 8, steep_levels(self.slope()), 8)
        self.assertEqual(out, spans)


if __name__ == "__main__":
    unittest.main()
