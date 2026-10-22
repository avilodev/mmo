import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))

import unittest
from span_math import cluster_cells, cluster_floors, clip_spans, merge_close, recover_spans, resolve_overlaps, usable

OPEN = 1.0e9


class SpanMathTest(unittest.TestCase):
    def test_flat_floor_is_one_span(self):
        cells = [[(0.0, OPEN)] for _ in range(16)]
        self.assertEqual(cluster_floors(cells), [(0.0, OPEN)])

    def test_a_thin_wall_top_is_not_a_floor(self):
        # 3 of 16 sub-samples hit a wall top at 60; the rest hit the ground
        cells = [[(0.0, OPEN)] for _ in range(13)] + [[(60.0, OPEN)] for _ in range(3)]
        self.assertEqual(cluster_floors(cells), [(0.0, OPEN)])

    def test_a_ramp_tile_is_one_span_at_its_median(self):
        cells = [[(float(i), OPEN)] for i in range(16)]  # rises 1 unit per cell
        spans = cluster_floors(cells)
        self.assertEqual(len(spans), 1)
        self.assertAlmostEqual(spans[0][0], 7.5, delta=1.0)

    def test_two_decks_are_two_spans(self):
        ground = [[(0.0, 70.0), (74.0, OPEN)] for _ in range(16)]
        self.assertEqual(cluster_floors(ground), [(0.0, 70.0), (74.0, OPEN)])

    def test_blocker_removes_the_floor_it_overlaps(self):
        # a prop from 0 to 50 stands on the ground span
        self.assertEqual(clip_spans([(0.0, OPEN)], [(0.0, 50.0)]), [])

    def test_a_ramps_own_surface_does_not_block_itself(self):
        # the ramp mesh spans 0..24 and its top is the span at 24
        self.assertEqual(clip_spans([(24.0, OPEN)], [(0.0, 24.0)]), [(24.0, OPEN)])

    def test_the_ground_under_a_solid_ramp_is_removed(self):
        self.assertEqual(clip_spans([(0.0, OPEN), (24.0, OPEN)], [(0.0, 24.0)]),
                         [(24.0, OPEN)])

    def test_low_curbs_do_not_block(self):
        self.assertEqual(clip_spans([(0.0, OPEN)], [(0.0, 8.0)]), [(0.0, OPEN)])

    def test_low_ceiling_is_dropped(self):
        self.assertEqual(usable([(0.0, 30.0), (0.0, 70.0)]), [(0.0, 70.0)])

    def test_a_tile_with_only_rejected_hits_has_no_floor(self):
        # 7 distinct sub-samples (< min_cells 8) hit; the other 9 hit nothing.
        # Also: many hits from one sub-sample must not count as many cells.
        cells = [[(0.0, OPEN), (1.0, OPEN)] for _ in range(7)] + [[] for _ in range(9)]
        self.assertEqual(cluster_floors(cells), [])

    def test_no_blockers_keeps_every_span(self):
        spans = [(0.0, 70.0), (74.0, OPEN)]
        self.assertEqual(clip_spans(spans, []), spans)


class ResolveOverlapsTest(unittest.TestCase):
    def test_a_ceiling_is_cut_at_the_next_floor(self):
        self.assertEqual(resolve_overlaps([(0.0, 59.0), (55.0, OPEN)]),
                         [(0.0, 55.0), (55.0, OPEN)])

    def test_unsorted_input_is_sorted(self):
        self.assertEqual(resolve_overlaps([(74.0, OPEN), (0.0, 70.0)]),
                         [(0.0, 70.0), (74.0, OPEN)])

    def test_a_span_with_no_height_left_is_dropped(self):
        self.assertEqual(resolve_overlaps([(10.0, 30.0), (10.0, OPEN)]), [(10.0, OPEN)])

    def test_spans_that_already_fit_are_unchanged(self):
        spans = [(0.0, 70.0), (74.0, OPEN)]
        self.assertEqual(resolve_overlaps(spans), spans)


class MergeTest(unittest.TestCase):
    def test_a_doorway_tile_is_one_span(self):
        # half the tile is a slab at 8, half is ground at 0: neither has 8 cells
        cells = [[(8.0, OPEN)] for _ in range(7)] + [[(0.0, OPEN)] for _ in range(9)]
        self.assertEqual(cluster_floors(cells), [(0.0, OPEN)])  # 9 ground cells alone
        cells = [[(8.0, OPEN)] for _ in range(6)] + [[(0.0, OPEN)] for _ in range(6)] + [[] for _ in range(4)]
        self.assertEqual(cluster_floors(cells), [])
        spans = merge_close(cluster_cells(cells), 12.0, 8)
        self.assertEqual(len(spans), 1)
        self.assertAlmostEqual(spans[0][0], 4.0, delta=4.1)

    def test_a_blocked_floor_is_not_merged_into_the_bench_top(self):
        cells = [[(0.0, OPEN), (21.0, OPEN)] for _ in range(10)] + [[(0.0, OPEN)] for _ in range(6)]
        clusters = cluster_cells(cells)
        kept = [c for c in clusters if clip_spans([(c[0], c[1])], [(0.0, 21.0)])]
        self.assertEqual(merge_close(kept, 12.0, 8), [(21.0, OPEN)])

    def test_distant_floors_stay_apart(self):
        cells = [[(0.0, 70.0), (74.0, OPEN)] for _ in range(16)]
        self.assertEqual(merge_close(cluster_cells(cells), 12.0, 8), [(0.0, 70.0), (74.0, OPEN)])


class RecoverSpansTest(unittest.TestCase):
    def test_lower_ground_beside_a_plateau_edge_is_recovered(self):
        # tile (0,0) kept only its plateau; the plain below it is thin there
        grid = {(0, 0): [(100.0, OPEN)], (1, 0): [(0.0, OPEN)]}
        found = {(0, 0): [(100.0, OPEN), (0.0, OPEN)]}
        out = recover_spans(grid, found, set())
        self.assertEqual(out[(0, 0)], [(0.0, 100.0), (100.0, OPEN)])

    def test_a_wall_top_far_above_the_ground_is_not_a_floor(self):
        grid = {(0, 0): [(0.0, OPEN)]}
        found = {(1, 0): [(300.0, OPEN)]}
        self.assertEqual(recover_spans(grid, found, set()).get((1, 0)), None)

    def test_a_partial_floor_with_no_span_gets_one_when_it_touches_ground(self):
        grid = {(0, 0): [(0.0, OPEN)]}
        found = {(1, 0): [(4.0, OPEN)]}
        self.assertEqual(recover_spans(grid, found, set())[(1, 0)], [(4.0, OPEN)])

    def test_recovery_chains_across_tiles(self):
        grid = {(0, 0): [(0.0, OPEN)]}
        found = {(1, 0): [(0.0, OPEN)], (2, 0): [(0.0, OPEN)], (3, 0): [(0.0, OPEN)]}
        out = recover_spans(grid, found, set())
        self.assertEqual([t for t in sorted(out) if out[t]], [(0, 0), (1, 0), (2, 0), (3, 0)])

    def test_a_floor_the_tile_already_has_is_not_added_again(self):
        grid = {(0, 0): [(0.0, OPEN)]}
        found = {(0, 0): [(5.0, OPEN)]}
        self.assertEqual(recover_spans(grid, found, set())[(0, 0)], [(0.0, OPEN)])

    def test_blocked_tiles_gain_nothing(self):
        grid = {(0, 0): [(0.0, OPEN)]}
        found = {(1, 0): [(0.0, OPEN)]}
        self.assertEqual(recover_spans(grid, found, {(1, 0)}).get((1, 0)), None)

    def test_a_floor_without_room_for_a_body_is_ignored(self):
        grid = {(0, 0): [(0.0, OPEN)]}
        found = {(1, 0): [(0.0, 20.0)]}
        self.assertEqual(recover_spans(grid, found, set()).get((1, 0)), None)

    def test_a_recovered_floor_never_costs_the_tile_its_ground(self):
        # a platform 30 up cuts the ground's ceiling to 30; the ground stays,
        # the floor mask gives its open cells their own ceiling
        grid = {(0, 0): [(0.0, OPEN)], (1, 0): [(30.0, OPEN)]}
        out = recover_spans(grid, {(0, 0): [(30.0, OPEN)]}, set())
        self.assertEqual(out[(0, 0)], [(0.0, 30.0), (30.0, OPEN)])

    def test_a_full_tile_gains_no_span_that_would_push_one_out(self):
        # seven spans, room for one more, and two usable lower floors to recover:
        # the cap is eight and every span the tile had must survive it
        have = [(1000.0 * n, 1000.0 * n + 900.0) for n in range(1, 8)]
        grid = {(0, 0): list(have), (1, 0): [(0.0, OPEN), (100.0, OPEN)]}
        found = {(0, 0): [(0.0, OPEN), (100.0, OPEN)]}
        out = recover_spans(grid, found, set())
        self.assertEqual(len(out[(0, 0)]), 8)
        for span in have:
            self.assertIn(span[0], [f for f, _ in out[(0, 0)]])

    def test_the_input_grid_is_left_alone(self):
        grid = {(0, 0): [(0.0, OPEN)]}
        recover_spans(grid, {(1, 0): [(0.0, OPEN)]}, set())
        self.assertEqual(grid, {(0, 0): [(0.0, OPEN)]})


if __name__ == "__main__":
    unittest.main()
