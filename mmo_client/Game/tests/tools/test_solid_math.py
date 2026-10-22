import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))

import math
import unittest

from footing_math import CELLS_PER_TILE, OPEN, STEEP
from solid_math import (MIN_SOLID_CELLS, _free, block_cells, drop_empty, face_occupancy, finish_mask,
                        rasterise, solidify, thicken)

C = CELLS_PER_TILE
N = C * C
TALL = 60.0


def flat_tiles(span_x, span_y, floor=0.0):
    """Tiles (0..span_x-1, 0..span_y-1), one level at `floor` everywhere."""
    return {(tx, ty): ([floor], [[(floor, OPEN)] * N])
            for ty in range(span_y) for tx in range(span_x)}


def entry(tiles, gx, gy, k=0):
    floors, levels = tiles[(gx // C, gy // C)]
    return levels[k][(gy % C) * C + gx % C]


def blocked_cells(tiles, k=0):
    out = set()
    for (tx, ty), (_, levels) in tiles.items():
        if k >= len(levels):
            continue
        for at, cell in enumerate(levels[k]):
            if cell is None:
                out.add((tx * C + at % C, ty * C + at // C))
    return out


def wall(x0, y0, x1, y1, top=TALL, bottom=0.0):
    """Two triangles of a vertical wall from (x0, y0) to (x1, y1), in cells,
    with its normal on the left of the direction of travel."""
    a = (x0 / C, y0 / C, bottom)
    b = (x1 / C, y1 / C, bottom)
    c = (x1 / C, y1 / C, top)
    d = (x0 / C, y0 / C, top)
    dx, dy = x1 - x0, y1 - y0
    length = math.hypot(dx, dy)
    nx, ny = -dy / length, dx / length
    return [(a, b, c, nx, ny), (a, c, d, nx, ny)]


def box(x0, y0, x1, y1, top=TALL):
    """The four walls of a closed box, normals outward."""
    return (wall(x0, y1, x0, y0, top) + wall(x0, y0, x1, y0, top) +
            wall(x1, y0, x1, y1, top) + wall(x1, y1, x0, y1, top))


def footprint(faces):
    cells = set()
    for a, b, c, _, _ in faces:
        cells |= set(rasterise(a, b, c))
    return cells


class RasteriseTest(unittest.TestCase):
    def test_a_wall_inside_a_column_of_cells_touches_only_that_column(self):
        cells = rasterise((3.5 / C, 0.5 / C, 0.0), (3.5 / C, 3.5 / C, 0.0), (3.5 / C, 3.5 / C, 30.0))
        self.assertEqual({gx for gx, _ in cells}, {3})
        self.assertEqual({gy for _, gy in cells}, {0, 1, 2, 3})

    def test_a_wall_on_a_cell_edge_touches_both_sides(self):
        cells = rasterise((4.0 / C, 0.5 / C, 0.0), (4.0 / C, 2.5 / C, 0.0), (4.0 / C, 2.5 / C, 30.0))
        self.assertEqual({gx for gx, _ in cells}, {3, 4})

    def test_the_height_range_is_the_part_inside_the_cell(self):
        # a slope rising 10 units per cell along x
        a, b, c = (0.0, 0.0, 0.0), (4.0 / C, 0.0, 40.0), (4.0 / C, 1.0 / C, 40.0)
        lo, hi = rasterise(a, b, c)[(2, 0)]
        self.assertAlmostEqual(lo, 20.0, delta=0.1)
        self.assertAlmostEqual(hi, 30.0, delta=0.1)

    def test_negative_coordinates_number_cells_downward(self):
        cells = rasterise((-0.5 / C, -0.5 / C, 0.0), (-0.5 / C, -0.2 / C, 0.0),
                          (-0.5 / C, -0.2 / C, 9.0))
        self.assertEqual(set(cells), {(-1, -1)})

    def test_a_triangle_across_a_tile_corner_reaches_all_four_tiles(self):
        cells = rasterise((0.9, 0.9, 0.0), (1.1, 0.9, 0.0), (1.0, 1.1, 20.0))
        tiles = {(gx // C, gy // C) for gx, gy in cells}
        self.assertEqual(tiles, {(0, 0), (1, 0), (0, 1), (1, 1)})


class ShapeMatrixTest(unittest.TestCase):
    """Whatever the shape, the cells it touches are its footprint within one."""

    def check(self, faces, expected):
        got = footprint(faces)
        self.assertTrue(expected <= got, "missed %s" % sorted(expected - got)[:5])
        grown = {(x + dx, y + dy) for x, y in expected for dx in (-1, 0, 1) for dy in (-1, 0, 1)}
        self.assertTrue(got <= grown, "extra %s" % sorted(got - grown)[:5])

    def test_box(self):
        expected = {(x, y) for x in range(10, 20) for y in range(10, 14)
                    if x in (10, 19) or y in (10, 13)}
        self.check(box(10.5, 10.5, 19.5, 13.5), expected)

    def test_thin_plane(self):
        self.check(wall(2.5, 5.5, 30.5, 5.5), {(x, 5) for x in range(2, 31)})

    def test_sloped_plank(self):
        a, b, c = (2.5 / C, 2.5 / C, 0.0), (12.5 / C, 2.5 / C, 30.0), (12.5 / C, 4.5 / C, 30.0)
        d = (2.5 / C, 4.5 / C, 0.0)
        faces = [(a, b, c, 0.0, 0.0), (a, c, d, 0.0, 0.0)]
        self.check(faces, {(x, y) for x in range(2, 13) for y in range(2, 5)})

    def test_cylinder(self):
        sides, r, cx, cy = 16, 3.0, 20.0, 20.0
        faces = []
        for i in range(sides):
            t0, t1 = 2 * math.pi * i / sides, 2 * math.pi * (i + 1) / sides
            faces += wall(cx + r * math.cos(t0), cy + r * math.sin(t0),
                          cx + r * math.cos(t1), cy + r * math.sin(t1))
        ring = {(math.floor(cx + r * math.cos(t / 64 * 2 * math.pi)),
                 math.floor(cy + r * math.sin(t / 64 * 2 * math.pi))) for t in range(64)}
        self.check(faces, ring)

    def test_a_mirrored_copy_has_the_mirrored_footprint(self):
        faces = wall(2.5, 5.5, 9.5, 8.5)
        mirrored = [((-a[0], a[1], a[2]), (-c[0], c[1], c[2]), (-b[0], b[1], b[2]), -nx, ny)
                    for a, b, c, nx, ny in faces]
        expected = {(-1 - x, y) for x, y in footprint(faces)}
        self.assertEqual(footprint(mirrored), expected)


class BlockCellsTest(unittest.TestCase):
    def test_a_lone_plane_blocks_the_floor_it_stands_on(self):
        tiles = flat_tiles(3, 3)
        found, _ = block_cells(tiles, face_occupancy(wall(20.5, 18.5, 20.5, 25.5)))
        self.assertEqual({(gx, gy) for gx, gy, _ in found},
                         {(20, y) for y in range(18, 26)})

    def test_a_wall_too_low_to_matter_is_stepped_over(self):
        tiles = flat_tiles(3, 3)
        found, _ = block_cells(tiles, face_occupancy(wall(20.5, 18.0, 20.5, 26.0, top=10.0)))
        self.assertEqual(found, [])

    def test_something_overhead_does_not_block(self):
        tiles = flat_tiles(3, 3)
        found, _ = block_cells(tiles, face_occupancy(wall(20.5, 18.0, 20.5, 26.0,
                                                          bottom=45.0, top=80.0)))
        self.assertEqual(found, [])

    def test_the_face_of_a_solid_the_rays_saw_is_left_alone(self):
        # a platform: the rays found no ground under cells x < 20, its face at
        # 20.3, so cell 20 keeps its ground (its centre is outside the face)
        tiles = flat_tiles(3, 3)
        for gy in range(C * 3):
            for gx in range(20):
                tile = tiles[(gx // C, gy // C)]
                tile[1][0][(gy % C) * C + gx % C] = None
        faces = wall(20.3, 47.5, 20.3, 0.5)
        self.assertIn((20, 10), face_occupancy(faces))
        self.assertEqual(block_cells(tiles, face_occupancy(faces)), ([], []))

    def test_a_rail_at_a_drop_blocks_the_floor_it_stands_on(self):
        # a balcony at 100 for x < 24, open air over ground at 0 beyond it;
        # the rail's face points out over the drop
        tiles = {}
        for tx in range(3):
            ground = [None] * N
            deck = [None] * N
            for at in range(N):
                if tx * C + at % C < 24:
                    deck[at] = (100.0, OPEN)
                else:
                    ground[at] = (0.0, OPEN)
            tiles[(tx, 0)] = ([0.0, 100.0], [ground, deck])
        faces = wall(23.5, 0.5, 23.5, 15.5, bottom=100.0, top=125.0)
        found, _ = block_cells(tiles, face_occupancy(faces))
        self.assertEqual({(gx, gy) for gx, gy, _ in found}, {(23, y) for y in range(16)})

    def test_a_rim_never_reopens_a_cell_a_face_blocked(self):
        # a rail standing on a deck's edge: the deck's own fascia face rises to
        # the deck and would lay it over the rail's cell
        tiles = flat_tiles(3, 1, floor=100.0)
        for gy in range(C):
            tiles[(1, 0)][1][0][gy * C + 4] = None   # the rays saw the rail
        faces = wall(20.5, 15.5, 20.5, 0.5, bottom=100.0, top=125.0)
        faces += wall(20.6, 0.5, 20.6, 15.5, bottom=60.0, top=100.0)
        out = solidify(tiles, *block_cells(tiles, face_occupancy(faces)))
        self.assertIsNone(entry(out, 20, 8))

    def test_the_same_triangles_block_the_same_cells_whatever_they_came_from(self):
        tiles = flat_tiles(3, 3)
        faces = wall(20.5, 18.0, 20.5, 26.0)
        once, _ = block_cells(tiles, face_occupancy(faces))
        again, _ = block_cells(tiles, face_occupancy(list(reversed(faces))))
        self.assertEqual(sorted(once), sorted(again))

    def test_a_floor_well_above_the_wall_is_not_blocked(self):
        tiles = {(1, 1): ([0.0, 100.0], [[(0.0, 80.0)] * N, [(100.0, OPEN)] * N])}
        found, _ = block_cells(tiles, face_occupancy(wall(20.5, 18.0, 20.5, 26.0)))
        self.assertTrue(found)
        self.assertTrue(all(abs(f) < 1.0 for _, _, f in found))


class ThickenTest(unittest.TestCase):
    def test_a_thin_line_becomes_min_cells_thick(self):
        tiles = flat_tiles(3, 3)
        line = [(24, gy, 0.0) for gy in range(2, 46)]
        out, _ = thicken(solidify(tiles, line), min_cells=13)
        for gy in (2, 24, 45):
            row = [gx for gx in range(C * 3) if entry(out, gx, gy) is None]
            self.assertEqual(row, list(range(18, 31)), gy)

    def test_a_post_becomes_a_square_a_box_cannot_straddle(self):
        tiles = flat_tiles(3, 3)
        out, _ = thicken(solidify(tiles, [(24, 24, 0.0), (25, 24, 0.0)]), min_cells=13)
        cells = blocked_cells(out)
        xs = {x for x, _ in cells}
        ys = {y for _, y in cells}
        self.assertEqual(len(xs), 13)
        self.assertEqual(len(ys), 13)
        self.assertEqual(len(cells), 13 * 13)

    def test_a_platform_rim_is_not_thickened(self):
        tiles = flat_tiles(3, 1)
        for gy in range(C):
            for gx in range(20):
                tiles[(gx // C, 0)][1][0][gy * C + gx % C] = None
        out, widened = thicken(tiles, min_cells=13)
        self.assertEqual(widened, 0)
        self.assertEqual(out, tiles)

    def test_a_narrow_gap_over_lower_ground_is_not_a_solid(self):
        # a gutter: floor 30 on both sides, ground 0 in the 4 cells between
        floors = [0.0, 30.0]
        lower = [None] * N
        upper = [(30.0, OPEN)] * N
        for gy in range(C):
            for gx in range(6, 10):
                lower[gy * C + gx] = (0.0, OPEN)
                upper[gy * C + gx] = None
        tiles = {(0, 0): (floors, [lower, upper])}
        out, widened = thicken(tiles, min_cells=13)
        self.assertEqual(widened, 0)

    def test_a_thin_solid_the_rays_saw_is_thickened_too(self):
        tiles = flat_tiles(3, 3)
        for gy in range(2, 46):
            for gx in (23, 24, 25, 26):
                tiles[(gx // C, gy // C)][1][0][(gy % C) * C + gx % C] = None
        out, widened = thicken(tiles, min_cells=13)
        row = [gx for gx in range(C * 3) if entry(out, gx, 24) is None]
        self.assertEqual(len(row), 13)
        self.assertGreater(widened, 0)

    def test_a_thin_solid_at_a_drop_grows_into_the_floor_side(self):
        # deck at 100 for x < 22, a rail in 22..23, open air over ground beyond
        tiles = {}
        for tx in range(3):
            ground = [None] * N
            deck = [None] * N
            for at in range(N):
                gx = tx * C + at % C
                if gx < 22:
                    deck[at] = (100.0, OPEN)
                elif gx > 23:
                    ground[at] = (0.0, OPEN)
            tiles[(tx, 0)] = ([0.0, 100.0], [ground, deck])
        out, _ = thicken(tiles, min_cells=13)
        row = [gx for gx in range(22) if entry(out, gx, 8, 1) is None]
        self.assertEqual(row, list(range(11, 22)))
        self.assertEqual(entry(out, 30, 8, 0), (0.0, OPEN))

    def test_widening_leaves_no_sliver_a_step_could_cross(self):
        # an L: a 2-wide stub with a 1-wide tail; growing it along columns
        # makes cells the row pass never saw
        tiles = flat_tiles(4, 4)
        cells = [(30, y, 0.0) for y in range(20, 31)] + [(31, y, 0.0) for y in range(20, 27)]
        out, _ = thicken(solidify(tiles, cells), min_cells=13)
        for dx, dy in ((1, 0), (0, 1)):
            for a in range(C * 4):
                line = [_free(out, a * dy + i * dx, a * dx + i * dy, 0.0, C, 12.0, 40.0)
                        for i in range(C * 4)]
                runs = "".join("." if f else "#" for f in line).strip("#").split(".")
                self.assertTrue(all(len(r) == 0 or len(r) >= 4 for r in runs), (dx, a))

    def test_widening_leaves_no_room_at_that_height(self):
        # a low slope beside a post: blocking only the ground would leave room
        tiles = {}
        for tx in range(3):
            slope = [(4.0, STEEP)] * N
            tiles[(tx, 0)] = ([0.0, 4.0], [[(0.0, OPEN)] * N, slope])
        for gy in range(C):
            tiles[(1, 0)][1][0][gy * C + 8] = None
            tiles[(1, 0)][1][1][gy * C + 8] = None
        out, _ = thicken(tiles, min_cells=5)
        for gx in range(C + 6, C + 11):
            self.assertFalse(_free(out, gx, 8, 0.0, C, 12.0, 40.0), gx)

    def test_the_input_is_left_alone(self):
        tiles = flat_tiles(3, 3)
        marked = solidify(tiles, [(24, 24, 0.0)])
        thicken(marked, min_cells=13)
        self.assertEqual(len(blocked_cells(marked)), 1)
        self.assertEqual(blocked_cells(tiles), set())


class DropEmptyTest(unittest.TestCase):
    def test_a_span_with_no_floor_anywhere_is_dropped_with_its_level(self):
        grid = {(0, 0): [(0.0, 50.0), (60.0, OPEN)]}
        tiles = {(0, 0): ([0.0, 60.0], [[None] * N, [(60.0, OPEN)] * N])}
        grid2, tiles2, dropped = drop_empty(grid, tiles)
        self.assertEqual(dropped, 1)
        self.assertEqual(grid2[(0, 0)], [(60.0, OPEN)])
        self.assertEqual(tiles2[(0, 0)], ([60.0], [[(60.0, OPEN)] * N]))

    def test_a_tile_left_with_nothing_has_no_spans(self):
        grid = {(0, 0): [(0.0, OPEN)]}
        tiles = {(0, 0): ([0.0], [[None] * N])}
        grid2, tiles2, _ = drop_empty(grid, tiles)
        self.assertEqual(grid2[(0, 0)], [])
        self.assertNotIn((0, 0), tiles2)

    def test_the_input_is_left_alone(self):
        grid = {(0, 0): [(0.0, OPEN)]}
        tiles = {(0, 0): ([0.0], [[None] * N])}
        drop_empty(grid, tiles)
        self.assertEqual(grid[(0, 0)], [(0.0, OPEN)])


class LedgeFaceTest(unittest.TestCase):
    """A hollow platform: the rays find ground under its top as well, so only
    its face keeps a body at the ground out, and the top must still reach the
    face so that a body can step off it."""

    def hollow(self):
        tiles = {}
        for tx in range(3):
            ground = [(0.0, 44.0)] * N
            top = [None] * N
            for at in range(N):
                gx = tx * C + at % C
                if gx < 20:
                    top[at] = (44.0, OPEN)
                elif gx > 25:
                    ground[at] = (0.0, OPEN)
            tiles[(tx, 0)] = ([0.0, 44.0], [ground, top])
        # the face slopes from the rim at 20 down to its foot at 26
        a, b = (20.0 / C, 0.0, 44.0), (20.0 / C, 1.0, 44.0)
        c, d = (26.0 / C, 1.0, 0.0), (26.0 / C, 0.0, 0.0)
        nx = 44.0 / math.hypot(44.0, 6.0)
        return tiles, [(a, b, c, nx, 0.0), (a, c, d, nx, 0.0)]

    def test_the_face_keeps_the_ground_out(self):
        tiles, faces = self.hollow()
        out = solidify(tiles, *block_cells(tiles, face_occupancy(faces)))
        for gx in range(21, 25):
            self.assertIsNone(entry(out, gx, 8, 0))

    def test_the_top_reaches_over_the_face(self):
        tiles, faces = self.hollow()
        out = solidify(tiles, *block_cells(tiles, face_occupancy(faces)))
        for gx in range(20, 26):
            self.assertIsNotNone(entry(out, gx, 8, 1), gx)
            self.assertAlmostEqual(entry(out, gx, 8, 1)[0], 44.0)

    def test_the_rim_is_not_thickened_into_the_ground_beyond(self):
        tiles, faces = self.hollow()
        out = solidify(tiles, *block_cells(tiles, face_occupancy(faces)))
        out, _ = thicken(out, min_cells=13)
        for gx in range(27, 48):
            self.assertEqual(entry(out, gx, 8, 0), (0.0, OPEN), gx)


    def test_a_hollow_shells_wall_grows_inward(self):
        # a vertical face at 20: ground blocked in cell 20, the top laid there
        tiles, _ = self.hollow()
        for (tx, _), (_, levels) in tiles.items():
            for at in range(N):
                gx = tx * C + at % C
                if gx == 20:
                    levels[0][at] = None
                    levels[1][at] = (44.0, OPEN)
                elif gx > 20:
                    levels[0][at] = (0.0, OPEN)
        out, widened = thicken(tiles, min_cells=13)
        self.assertEqual(widened, 12 * C)
        for gx in range(8, 21):
            self.assertIsNone(entry(out, gx, 8, 0), gx)
        for gx in range(21, 48):
            self.assertEqual(entry(out, gx, 8, 0), (0.0, OPEN), gx)


class FinishMaskTest(unittest.TestCase):
    AREA = (-1, -1, 5, 5)

    def grid_of(self, tiles):
        return {tile: [(floors[0], OPEN)] for tile, (floors, _) in tiles.items()}

    def test_closing_does_not_heal_a_thin_obstacle(self):
        # a 4-cell solid the rays saw, narrow enough for close_gaps to fill
        tiles = flat_tiles(3, 3)
        for gy in range(19, 29):
            for gx in range(22, 26):
                tiles[(1, 1)][1][0][(gy % C) * C + gx % C] = None
        faces = wall(22.0, 19.0, 22.0, 29.0) + wall(26.0, 29.0, 26.0, 19.0)
        _, out, report = finish_mask(self.grid_of(tiles), tiles, face_occupancy(faces), self.AREA)
        self.assertGreater(report["seams"] + report["holes"], 0)
        row = [gx for gx in range(C * 3) if entry(out, gx, 24) is None]
        self.assertGreaterEqual(len(row), MIN_SOLID_CELLS)
        self.assertTrue(set(range(22, 26)) <= set(row))

    def test_a_platform_rim_keeps_the_ground_at_its_foot(self):
        # ground at 0 except under the platform (x < 20); the top at 39 there
        tiles = flat_tiles(3, 3)
        for (tx, ty), (floors, levels) in list(tiles.items()):
            top = [None] * N
            for at in range(N):
                if tx * C + at % C < 20:
                    levels[0][at] = None
                    top[at] = (39.0, OPEN)
            tiles[(tx, ty)] = ([0.0, 39.0], [levels[0], top])
        faces = wall(20.0, 48.0, 20.0, 0.0, top=39.0)
        grid = {tile: [(0.0, 39.0), (39.0, OPEN)] for tile in tiles}
        _, out, report = finish_mask(grid, tiles, face_occupancy(faces), self.AREA)
        self.assertEqual(report["blocked"], 0)
        self.assertEqual(report["widened"], 0)
        self.assertEqual(entry(out, 20, 24), (0.0, OPEN))
        self.assertEqual(entry(out, 19, 24, 1), (39.0, OPEN))

    def test_a_span_blocked_everywhere_is_dropped(self):
        tiles = {(1, 1): ([0.0], [[None] * N])}
        grid, out, report = finish_mask({(1, 1): [(0.0, OPEN)]}, tiles, {}, self.AREA)
        self.assertEqual(report["dropped"], 1)
        self.assertEqual(grid[(1, 1)], [])
        self.assertNotIn((1, 1), out)


if __name__ == "__main__":
    unittest.main()
