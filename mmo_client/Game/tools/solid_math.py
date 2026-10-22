"""Cells that solid geometry blocks, at the floor mask's resolution. Pure: no bpy.

The bake's rays already see a solid wherever it covers a cell's centre: the
floor under it has no room for a body, so the mask has no floor there. What the
rays cannot see is geometry with no thickness at their spacing (a wall that is
a single plane, a rail, a pole), and close_gaps then heals the narrow holes
that thin solids do leave. So, after the closing passes:

  block_cells  rasterises the steep triangles and blocks a floor cell where one
               rises through the body above that floor (from a step up to a
               body's height) with room, not solid, behind it. The face of a
               solid the rays saw (a platform's rim) is backed by cells with no
               floor and blocks nothing, so the rim stays where it is. A face
               that rises to a floor of its tile, found a short way behind it,
               is that floor's rim: the floor is laid over every cell the face
               touches, so a body on top reaches the face's foot and steps off
               there (a hollow shell's face and a sloped rim alike).
  thicken      widens every solid narrower than min_cells with floor at the
               same height beside it. The game tests a body's box at points 4
               units apart around its edge, a step of 3.3 units at a time, so
               a thinner solid could pass between two points or two steps.

Nothing here asks what an object is: the same triangles block the same cells.
Tiles are (floors, levels) as in footing_math; a cell is numbered across the
whole area, gx = tx * cells + ci. A cell is judged as the game judges it
(world_spans_at): a level present there stands a body at the cell's own floor,
under its own ceiling; a steep floor has open air above it.
"""

import math

from footing_math import (BORDER_RING, CELLS_PER_TILE, MAX_HOLE, MAX_SEAM, OPEN,
                          clear_border, close_gaps, close_seams, is_steep)

STEP = 12.0
BODY = 40.0
EDGE_EPS = 1.0e-3     # cells: a triangle on a cell edge touches both sides
BACK_REACH = 1.0      # cells behind a face to look for what backs it
RIM_REACH = 8         # cells behind a face to look for the floor it rises to
MIN_SOLID_CELLS = 5   # one more than the 4 units between the box's edge points
SLIVER_CELLS = 4      # a widened solid thinner than this, a step could cross


def _clip(poly, axis, bound, keep_greater):
    """Clip a polygon of (x, y, z) tuples against one axis-aligned plane."""
    def inside(p):
        return p[axis] >= bound if keep_greater else p[axis] <= bound

    def cross(p, q):
        span = q[axis] - p[axis]
        t = 0.0 if span == 0.0 else (bound - p[axis]) / span
        return tuple(p[i] + (q[i] - p[i]) * t for i in range(3))

    out = []
    for i, cur in enumerate(poly):
        prev = poly[i - 1]
        if inside(cur):
            if not inside(prev):
                out.append(cross(prev, cur))
            out.append(cur)
        elif inside(prev):
            out.append(cross(prev, cur))
    return out


def rasterise(a, b, c, cells=CELLS_PER_TILE, eps=EDGE_EPS):
    """{(gx, gy): (low, high)}: the height range of triangle abc inside each
    cell it touches. a, b and c are (x, y, z), x and y in tiles and z in world
    units. Each cell is grown by eps cells, so nothing slips between two."""
    poly = [(p[0] * cells, p[1] * cells, p[2]) for p in (a, b, c)]
    y0 = math.floor(min(p[1] for p in poly) - eps)
    y1 = math.floor(max(p[1] for p in poly) + eps)
    out = {}
    for gy in range(y0, y1 + 1):
        strip = _clip(_clip(poly, 1, gy - eps, True), 1, gy + 1 + eps, False)
        if not strip:
            continue
        x0 = math.floor(min(p[0] for p in strip) - eps)
        x1 = math.floor(max(p[0] for p in strip) + eps)
        for gx in range(x0, x1 + 1):
            piece = _clip(_clip(strip, 0, gx - eps, True), 0, gx + 1 + eps, False)
            if piece:
                zs = [p[2] for p in piece]
                out[(gx, gy)] = (min(zs), max(zs))
    return out


def face_occupancy(faces, cells=CELLS_PER_TILE):
    """{(gx, gy): [(low, high, nx, ny, top)]} for faces given as
    (a, b, c, nx, ny), where (nx, ny) is the horizontal part of the outward
    normal, unit length or zero, and top is the face's highest point."""
    occupancy = {}
    for a, b, c, nx, ny in faces:
        top = max(a[2], b[2], c[2])
        for cell, (lo, hi) in rasterise(a, b, c, cells).items():
            occupancy.setdefault(cell, []).append((lo, hi, nx, ny, top))
    return occupancy


def _at(gx, gy, cells):
    return (gx // cells, gy // cells), (gy % cells) * cells + gx % cells


def _present(tiles, gx, gy, cells):
    """(k, floor, ceiling) of each level with floor in cell (gx, gy)."""
    tile, at = _at(gx, gy, cells)
    entry = tiles.get(tile)
    if entry is None:
        return []
    out = []
    for k, level in enumerate(entry[1]):
        cell = level[at]
        if cell is None:
            continue
        open_above = cell[1] == OPEN or is_steep(cell)
        out.append((k, cell[0], OPEN if open_above else cell[0] + cell[1]))
    return out


def _holding(tiles, gx, gy, floor, cells, step):
    """Index of the level of cell (gx, gy) whose floor there is nearest
    `floor`, within a step of it, or None."""
    near = [(abs(f - floor), k) for k, f, _ in _present(tiles, gx, gy, cells)
            if abs(f - floor) <= step]
    return min(near)[1] if near else None


def _free(tiles, gx, gy, floor, cells, step, body):
    """Is there room for a body at `floor` in cell (gx, gy), as the game's
    column test asks: a floor no higher than a step above it, with a body's
    height of room above `floor`?"""
    return any(f <= floor + step and ceil - floor >= body
               for _, f, ceil in _present(tiles, gx, gy, cells))


def _behind(gx, gy, nx, ny, n):
    """The cell n cells behind (gx, gy), against the normal (nx, ny)."""
    return math.floor(gx + 0.5 - nx * n), math.floor(gy + 0.5 - ny * n)


def _rim_of(tiles, gx, gy, face, faces, cells, step, body, reach):
    """(gx, gy, floor, entry) laying the floor the face rises to over its
    cell, or None: a level of the cell's tile, whose span floor is `floor`,
    within a step of the face's top and absent in the cell, with floor within
    `reach` cells behind it; entry is that floor's cell, copied. No floor is
    laid where any of the cell's faces stands in a body's height above it (a
    rail on the rim)."""
    _, _, nx, ny, top = face
    tile, at = _at(gx, gy, cells)
    floors, levels = tiles[tile]
    if any(is_steep(level[at]) for level in levels):
        return None  # a slope's own cells already have air above them
    for k, floor in enumerate(floors):
        if abs(floor - top) > step or levels[k][at] is not None:
            continue
        if any(hi > floor + step and lo < floor + body for lo, hi, _, _, _ in faces):
            continue
        for n in range(1, reach + 1):
            bx, by = _behind(gx, gy, nx, ny, n)
            k2 = _holding(tiles, bx, by, floor, cells, step)
            if k2 is not None:
                btile, bat = _at(bx, by, cells)
                return (gx, gy, floor, tiles[btile][1][k2][bat])
    return None


def block_cells(tiles, occupancy, cells=CELLS_PER_TILE, step=STEP, body=BODY,
                reach=BACK_REACH, rim_reach=RIM_REACH):
    """(blocked, rims): the floor cells faces block, as [(gx, gy, floor)], and
    the floors laid over a rim's face, as [(gx, gy, floor, entry)].

    A face blocks a floor in its cell when it rises past a step above that
    floor and starts below a body's height, and the cell `reach` behind it,
    against its normal, has room for a body at that height (floor, or a drop).
    Behind the face of a solid the rays saw there is no room, and that face
    blocks nothing.
    """
    found = []
    rims = []
    for (gx, gy), faces in sorted(occupancy.items()):
        if _at(gx, gy, cells)[0] not in tiles:
            continue
        for face in faces:
            rim = _rim_of(tiles, gx, gy, face, faces, cells, step, body, rim_reach)
            if rim is not None:
                rims.append(rim)
                break
        for _, floor, _ in _present(tiles, gx, gy, cells):
            for lo, hi, nx, ny, _ in faces:
                if hi <= floor + step or lo >= floor + body:
                    continue
                bx, by = _behind(gx, gy, nx, ny, reach)
                if (bx, by) == (gx, gy) or _free(tiles, bx, by, floor, cells, step, body):
                    found.append((gx, gy, floor))
                    break
    return found, rims


def _copy(tiles):
    return {tile: (list(floors), [list(level) for level in levels])
            for tile, (floors, levels) in tiles.items()}


def solidify(tiles, blocked, rims=(), cells=CELLS_PER_TILE, step=STEP):
    """A copy of tiles with no floor at each (gx, gy, floor) of `blocked`,
    then the entry of each (gx, gy, floor, entry) of `rims` laid into the
    level whose span floor is nearest `floor`, where the cell has none."""
    out = _copy(tiles)
    for gx, gy, floor in blocked:
        k = _holding(out, gx, gy, floor, cells, step)
        if k is not None:
            tile, at = _at(gx, gy, cells)
            out[tile][1][k][at] = None
    for gx, gy, floor, entry in rims:
        tile, at = _at(gx, gy, cells)
        floors, levels = out[tile]
        k = min(range(len(floors)), key=lambda i: abs(floors[i] - floor))
        if levels[k][at] is None:
            levels[k][at] = entry
    return out


def _covered(tiles, gx, gy, floor, cells, step):
    """Does cell (gx, gy) hold a floor more than a step above `floor`, so it
    keeps one when the floor at `floor` is blocked?"""
    return any(f > floor + step for _, f, _ in _present(tiles, gx, gy, cells))


def _covered_run(tiles, gx, gy, dx, dy, count, floor, cells, step):
    return all(_covered(tiles, gx + i * dx, gy + i * dy, floor, cells, step)
               for i in range(count))


def _thin_runs(tiles, dx, dy, min_cells, cells, step, body):
    """(gx, gy, floor) cells to block so that every solid run along (dx, dy)
    narrower than min_cells, with floor at the same height on at least one
    side, grows to min_cells."""
    targets = []
    for (tx, ty), (floors, levels) in tiles.items():
        for floor, level in zip(floors, levels):
            for at, cell in enumerate(level):
                if cell is None:
                    continue
                gx = tx * cells + at % cells
                gy = ty * cells + at // cells
                for sx, sy in ((dx, dy), (-dx, -dy)):
                    targets += _grow_run(tiles, gx, gy, sx, sy, floor, min_cells, cells,
                                         step, body)
    return targets


def _grow_run(tiles, gx, gy, dx, dy, floor, min_cells, cells, step, body):
    """The cells _thin_runs blocks for the run that starts next to floor cell
    (gx, gy) along (dx, dy), if there is one. With floor on both sides it
    grows about its middle, or wholly into one side when that side's cells
    are under a higher floor (a hollow shell's inside), so a rim's open foot
    stays open; with a drop on the far side it grows into the floor side."""
    if _free(tiles, gx + dx, gy + dy, floor, cells, step, body):
        return []
    n = 2
    while n <= min_cells and not _free(tiles, gx + n * dx, gy + n * dy, floor,
                                       cells, step, body):
        n += 1
    if n > min_cells:
        return []
    extra = min_cells - (n - 1)
    if _holding(tiles, gx + n * dx, gy + n * dy, floor, cells, step) is None:
        before = extra
    elif dx < 0 or dy < 0:
        return []  # the same run, found from its other end
    elif _covered_run(tiles, gx, gy, -dx, -dy, extra, floor, cells, step):
        before = extra
    elif _covered_run(tiles, gx + n * dx, gy + n * dy, dx, dy, extra, floor,
                      cells, step):
        before = 0
    else:
        before = extra // 2
    out = [(gx - i * dx, gy - i * dy, floor) for i in range(before)]
    out += [(gx + (n + i) * dx, gy + (n + i) * dy, floor) for i in range(extra - before)]
    return out


def thicken(tiles, min_cells=MIN_SOLID_CELLS, cells=CELLS_PER_TILE, step=STEP, body=BODY):
    """Widen the solids narrower than min_cells: along rows, then along the
    columns of the result, so a post becomes a square. Growing columns can
    leave a sliver at the ragged end of a widened solid that a single step
    crosses (and the server, sampling finer, refuses); those cells are given
    their floor back. Returns (new tiles, cells blocked); the input is not
    changed."""
    out = tiles
    for dx, dy in ((1, 0), (0, 1)):
        out = _fill(out, _thin_runs(out, dx, dy, min_cells, cells, step, body), cells,
                    step, body)
    out = _trim_slivers(out, tiles, cells, step, body)
    return out, _count_floors(tiles) - _count_floors(out)


def _run_across(tiles, gx, gy, dx, dy, floor, cells, step, body):
    """(length, open at both ends) of the solid run at `floor` through cell
    (gx, gy) along (dx, dy), looking no further than SLIVER_CELLS."""
    length = 1
    ends = 0
    for sign in (1, -1):
        n = 1
        while n <= SLIVER_CELLS and not _free(tiles, gx + sign * n * dx, gy + sign * n * dy,
                                              floor, cells, step, body):
            n += 1
        length += n - 1
        ends += n <= SLIVER_CELLS
    return length, ends == 2


def _trim_slivers(tiles, original, cells, step, body):
    """Give back the floor of each widened cell (blocked in `tiles`, floor in
    `original`) in a solid run under SLIVER_CELLS across with room at both
    ends, until there are none. Only widening is undone."""
    out = _copy(tiles)
    while True:
        trimmed = []
        for tile, (floors, levels) in out.items():
            for k, level in enumerate(levels):
                was = original[tile][1][k]
                for at, cell in enumerate(level):
                    if cell is not None or was[at] is None:
                        continue
                    gx = tile[0] * cells + at % cells
                    gy = tile[1] * cells + at // cells
                    for dx, dy in ((1, 0), (0, 1)):
                        length, open_ends = _run_across(out, gx, gy, dx, dy, floors[k],
                                                        cells, step, body)
                        if open_ends and length < SLIVER_CELLS:
                            trimmed.append((tile, k, at, was[at]))
                            break
        if not trimmed:
            return out
        for tile, k, at, entry in trimmed:
            out[tile][1][k][at] = entry


def _fill(tiles, targets, cells, step, body):
    """A copy of tiles in which each (gx, gy, floor) of targets has no room for
    a body at floor: every entry of the cell that gave room there is gone."""
    out = _copy(tiles)
    for gx, gy, floor in targets:
        tile, at = _at(gx, gy, cells)
        entry = out.get(tile)
        if entry is None:
            continue
        for k, f, ceil in _present(out, gx, gy, cells):
            if f <= floor + step and ceil - floor >= body:
                entry[1][k][at] = None
    return out


def _count_floors(tiles):
    return sum(cell is not None for _, levels in tiles.values()
               for level in levels for cell in level)


def drop_empty(grid, tiles):
    """Drop each span whose level has no floor in any cell, with its level, so
    a tile left with none is solid. Returns (new grid, new tiles, spans
    dropped); the inputs are not changed."""
    grid_out = {tile: list(spans) for tile, spans in grid.items()}
    tiles_out = {}
    dropped = 0
    for tile, (floors, levels) in tiles.items():
        keep = [k for k, level in enumerate(levels) if any(c is not None for c in level)]
        dropped += len(levels) - len(keep)
        spans = grid_out.get(tile, [])
        grid_out[tile] = [spans[k] for k in keep]
        if keep:
            tiles_out[tile] = ([floors[k] for k in keep], [list(levels[k]) for k in keep])
    return grid_out, tiles_out, dropped


def finish_mask(grid, tiles, occupancy, area, cells=CELLS_PER_TILE):
    """The whole-area passes over the mask, in the order that matters.

    grid maps a tile to its spans and tiles a tile with spans to (floors,
    levels); area is (min_x, min_y, width, height). The border ring is made
    whole and narrow holes are closed first; the faces block after that, so a
    closing pass cannot heal a thin obstacle. Returns (grid, tiles, report),
    the report a dict of counts; the inputs are not changed.
    """
    tiles = clear_border(tiles, *area, BORDER_RING)
    tiles, seams = close_seams(tiles, cells, MAX_SEAM)
    tiles, holes = close_gaps(tiles, cells, MAX_HOLE)
    found, rims = block_cells(tiles, occupancy, cells)
    tiles = solidify(tiles, found, rims, cells)
    tiles, widened = thicken(tiles, cells=cells)
    grid, tiles, dropped = drop_empty(grid, tiles)
    report = {"seams": seams, "holes": holes, "face_cells": len(occupancy),
              "blocked": len(found), "rims": len(rims), "widened": widened,
              "dropped": dropped}
    return grid, tiles, report
