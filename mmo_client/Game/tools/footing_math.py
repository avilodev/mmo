"""The fine floor mask: each tile's floors, cell by cell. Pure: no bpy.

Layout of the file pack_file() writes, little-endian, read by
Game/src/world/footing.c (the foot IK) and common/world_spans.c (collision):

    "CFTG", u32 version (4), i32 min_x, min_y, width, height, cells_per_tile,
    u32 level_count, then one record per tile, row-major: u8 level count (0 for
    a tile whose spans are all present and flat everywhere), then per level an
    f32 base height, cells_per_tile^2 u8 heights above the base in CELL_UNIT
    steps (255 = no floor there), and cells_per_tile^2 u8 clearances above each
    floor in CLEAR_UNIT steps (255 = open sky, 254 = a steep floor: a slope
    too steep to walk, open above it). A tile's levels are its spans, in order,
    one level each; a span with no floor anywhere keeps an all-255 level. The
    game drops a span at a point whose cell is 255 and gives it the cell's own
    floor and ceiling.

A cell is an entry (floor, clear) or None, clear being STEEP for a steep
floor. A level is a list of cells.
"""

import math
import struct

CELLS_PER_TILE = 16
CELL_UNIT = 0.25
CLEAR_UNIT = 2.0
NONE = 255
OPEN_CLEAR = 255
STEEP_CLEAR = 254
FLAT_EPS = 0.5
MAX_RISE = 16.0
STEP = 12.0
MAX_LEVELS = 8
BORDER_RING = 1       # tiles at the edge of the baked area ignore the mask
MAX_SEAM = 4          # a crack in one floor this narrow (cells) is closed, about 18 cm
MAX_HOLE = 8          # a rim hole across floors this narrow is closed, about 36 cm
OPEN = float("inf")
STEEP = -1.0          # the clear of a steep floor's cell: open above, never climbed
STEEP_GAP = 6.0       # steep hits this close in height are one slope
MAX_SLOPE_RISE = 60.0 # the most of a slope one level holds (a byte of steps)
MIN_SLOPE_CELLS = 8   # a steep patch in fewer cells of a tile is a chamfer
OPEN_CEIL = 1.0e8     # a ceiling at or above this is open sky
FILE_MAGIC = b"CFTG"
FILE_VERSION = 4


def assign_level(cell_hits, span_floor, max_rise=MAX_RISE):
    """One cell per cell of the tile for one span: the floor hit nearest the
    span's floor as (floor, clearance above it), or None when no hit is within
    max_rise. cell_hits is one list of (floor, ceil) per cell."""
    level = []
    for hits in cell_hits:
        near = [h for h in hits if abs(h[0] - span_floor) <= max_rise]
        if not near:
            level.append(None)
            continue
        floor, ceil = min(near, key=lambda h: abs(h[0] - span_floor))
        level.append((floor, OPEN if ceil >= OPEN_CEIL else ceil - floor))
    return level


def is_steep(cell):
    return cell is not None and cell[1] == STEEP


def is_flat(level, eps=FLAT_EPS):
    heights = [cell[0] for cell in level if cell is not None]
    return not heights or max(heights) - min(heights) <= eps


def encode_level(level):
    """(base, deltas, clears): base is the lowest floor; each delta is the
    height above it in CELL_UNIT steps and each clear the room above the floor
    in CLEAR_UNIT steps, rounded up; both are NONE for a cell with no floor. A floor too far
    above the base to encode is a bug upstream, so it raises."""
    heights = [cell[0] for cell in level if cell is not None]
    base = min(heights) if heights else 0.0
    deltas = bytearray()
    clears = bytearray()
    for cell in level:
        if cell is None:
            deltas.append(NONE)
            clears.append(OPEN_CLEAR)
            continue
        steps = int(round((cell[0] - base) / CELL_UNIT))
        if steps >= NONE:
            raise ValueError("a level spans %.1f units, more than a byte of steps"
                             % (cell[0] - base))
        deltas.append(steps)
        if is_steep(cell):
            clears.append(STEEP_CLEAR)
            continue
        # round up: the room must never read as less than there is; room past
        # what the byte holds below the steep mark is open
        room = math.ceil(cell[1] / CLEAR_UNIT) if cell[1] != OPEN else OPEN_CLEAR
        clears.append(room if room < STEEP_CLEAR else OPEN_CLEAR)
    return base, bytes(deltas), bytes(clears)


def tile_levels(cell_hits, span_floors):
    """One level per span of a tile, in span order."""
    return [assign_level(cell_hits, floor) for floor in span_floors[:MAX_LEVELS]]


def encode_tile(levels):
    """[] when every level is present, flat and open to the sky at every cell
    (nothing the span does not already say), else the encoded levels."""
    plain = all(cell is not None and cell[1] == OPEN
                for level in levels for cell in level)
    if plain and all(is_flat(level) for level in levels):
        return []
    return [encode_level(level) for level in levels]


def steep_levels(cell_steeps, gap=STEEP_GAP, max_rise=MAX_SLOPE_RISE,
                 min_cells=MIN_SLOPE_CELLS):
    """A level for each slope too steep to walk in a tile: [(floor, level)].

    cell_steeps is one list per cell of the heights where a ray met such a
    slope with room above it. Heights within `gap` of each other are one slope,
    up to max_rise from its lowest; a slope in fewer than min_cells cells (a
    chamfer) is left out. Each level holds its cells at their own heights as
    STEEP entries, the highest where a cell has two; its floor is the lowest.
    """
    hits = sorted((z, i) for i, cell in enumerate(cell_steeps) for z in cell)
    groups = []
    for z, i in hits:
        if groups and z - groups[-1][-1][0] <= gap and z - groups[-1][0][0] <= max_rise:
            groups[-1].append((z, i))
        else:
            groups.append([(z, i)])
    out = []
    for group in groups:
        if len({i for _, i in group}) < min_cells:
            continue
        level = [None] * len(cell_steeps)
        for z, i in group:
            if level[i] is None or z > level[i][0]:
                level[i] = (z, STEEP)
        out.append((group[0][0], level))
    return out


def add_levels(spans, levels, steep, max_spans=MAX_LEVELS, open_ceil=OPEN_CEIL):
    """A tile's spans and levels with its slopes added: (spans, floors, levels).

    spans are the tile's ascending (floor, ceil) and levels one per span; steep
    is what steep_levels() gave. Each slope becomes a span from its lowest
    point, nudged off any floor it would share, and every ceiling is cut at the
    next floor. When there is no room, slopes give way to the spans the tile
    had.
    """
    room = max(0, max_spans - len(spans))
    rows = [(floor, ceil, level) for (floor, ceil), level in zip(spans, levels)]
    for floor, level in steep[:room]:
        while any(abs(floor - f) < 0.01 for f, _, _ in rows):
            floor += 0.02
        rows.append((floor, open_ceil, level))
    rows.sort(key=lambda r: r[0])
    out = []
    for n, (floor, ceil, level) in enumerate(rows):
        if n + 1 < len(rows):
            ceil = min(ceil, rows[n + 1][0])
        out.append((floor, ceil))
    return out, [r[0] for r in rows], [r[2] for r in rows]


def bake_tile(cell_hits, span_floors):
    """The mask of one tile, before the passes over the whole area."""
    return encode_tile(tile_levels(cell_hits, span_floors))


def pack_file(min_x, min_y, width, height, tiles, cells=CELLS_PER_TILE):
    """tiles maps (tx, ty) to the list encode_tile returned for that tile."""
    body = bytearray()
    count = 0
    for ty in range(min_y, min_y + height):
        for tx in range(min_x, min_x + width):
            encoded = tiles.get((tx, ty), [])
            body.append(len(encoded))
            for base, deltas, clears in encoded:
                assert len(deltas) == cells * cells and len(clears) == cells * cells
                body += struct.pack("<f", base) + deltas + clears
                count += 1
    head = FILE_MAGIC + struct.pack("<IiiiiiI", FILE_VERSION, min_x, min_y,
                                    width, height, cells, count)
    return head + bytes(body)


def _copy(tiles):
    return {tile: (floors, [list(level) for level in levels])
            for tile, (floors, levels) in tiles.items()}


def _nearest_level(floors, height, reach):
    """Index of the span whose floor is nearest `height` within `reach`, or None."""
    best = None
    for n, floor in enumerate(floors):
        if abs(floor - height) <= reach and (
                best is None or abs(floor - height) < abs(floors[best] - height)):
            best = n
    return best


def close_gaps(tiles, cells=CELLS_PER_TILE, max_gap=MAX_HOLE):
    """Fill the holes in the mask too narrow to be real obstacles.

    A steep chamfer at a platform's rim or a crack in the ground has no floor of
    its own, so the mask marks it empty; at one-unit resolution a box corner
    must land on such a sliver at some step and could never cross it. A run of
    at most max_gap cells that no level of the tile has floor in, in a row or a
    column, with floor on both ends, is filled from one of the two floors beside
    it (the lower first) into the tile's span that floor belongs to: one whose
    floor is within MAX_RISE of it. A hole with no such span is left. A hole next
    to a tile that has no spans (solid) or past the edge of the baked area is
    left, as is anything wider.

    tiles maps a tile to (floors, levels): the tile's span floors and a level
    for each. Returns (new tiles, number of cells filled); the input is not
    changed.
    """
    if not tiles:
        return {}, 0
    solid, hole = None, object()
    lo_x = min(t[0] for t in tiles) * cells
    hi_x = (max(t[0] for t in tiles) + 1) * cells
    lo_y = min(t[1] for t in tiles) * cells
    hi_y = (max(t[1] for t in tiles) + 1) * cells
    width, height = hi_x - lo_x, hi_y - lo_y

    # the lowest floor entry at each cell, the hole marker, or None where no span is
    grid = [[solid] * width for _ in range(height)]
    for (tx, ty), (_, levels) in tiles.items():
        for cj in range(cells):
            row = grid[ty * cells + cj - lo_y]
            for ci in range(cells):
                found = [lvl[cj * cells + ci] for lvl in levels if lvl[cj * cells + ci] is not None]
                row[tx * cells + ci - lo_x] = min(found, key=lambda c: c[0]) if found else hole

    fills = {}

    def scan(line, key):
        run = 0
        for i, value in enumerate(line):
            if value is hole:
                run += 1
                continue
            if run and value is not solid and i - run > 0:
                before = line[i - run - 1]
                if before is not solid and before is not hole and run <= max_gap and \
                   not is_steep(before) and not is_steep(value):
                    for j in range(i - run, i):
                        fills.setdefault(key(j), []).extend((before, value))
            run = 0

    for gy in range(height):
        scan(grid[gy], lambda j, gy=gy: (j + lo_x, gy + lo_y))
    for gx in range(width):
        scan([grid[gy][gx] for gy in range(height)], lambda j, gx=gx: (gx + lo_x, j + lo_y))

    out = _copy(tiles)
    filled = 0
    for (gx, gy), candidates in fills.items():
        floors, levels = out[(gx // cells, gy // cells)]
        at = (gy % cells) * cells + gx % cells
        for entry in sorted(set(candidates)):
            k = _nearest_level(floors, entry[0], MAX_RISE)
            if k is not None:
                levels[k][at] = entry
                filled += 1
                break
    return out, filled


def _seams(row, max_gap):
    """Cells of a row (or column) in runs of None of at most max_gap that have a
    cell on both ends, with the entry to fill them with: the lower of the two
    floors beside the run, and the less room of the two."""
    found = []
    i = 0
    while i < len(row):
        if row[i] is not None:
            i += 1
            continue
        j = i
        while j < len(row) and row[j] is None:
            j += 1
        if i > 0 and j < len(row) and j - i <= max_gap and \
           not is_steep(row[i - 1]) and not is_steep(row[j]):
            a, b = row[i - 1], row[j]
            low = a if a[0] <= b[0] else b
            fill = (low[0], min(a[1], b[1]))
            found.extend((n, fill) for n in range(i, j))
        i = j
    return found


def close_seams(tiles, cells=CELLS_PER_TILE, max_gap=MAX_SEAM, step=STEP):
    """Close narrow gaps within one floor level.

    A seam in a storey's floor is no obstacle either, but a hole of close_gaps
    must be empty in every level, and an upper floor over a present lower one
    never is. Here a run of at most max_gap empty cells in one level, with that
    level's floor on both ends (its own tile's, or a neighbour's level within a
    step of it), is filled with the lower of the two floors beside it. Returns
    (new tiles, cells filled); the input is not changed.
    """
    out = _copy(tiles)
    pad = max_gap + 1
    filled = 0

    def level_of(tile, floor):
        entry = tiles.get(tile)
        if entry is None:
            return None
        k = _nearest_level(entry[0], floor, step)
        return None if k is None else entry[1][k]

    for (tx, ty), (floors, levels) in tiles.items():
        for k, level in enumerate(levels):
            west = level_of((tx - 1, ty), floors[k])
            east = level_of((tx + 1, ty), floors[k])
            north = level_of((tx, ty - 1), floors[k])
            south = level_of((tx, ty + 1), floors[k])
            todo = []
            for cj in range(cells):
                row = ([west[cj * cells + ci] for ci in range(cells - pad, cells)] if west else [None] * pad)
                row += [level[cj * cells + ci] for ci in range(cells)]
                row += ([east[cj * cells + ci] for ci in range(pad)] if east else [None] * pad)
                todo += [(cj * cells + n - pad, fill) for n, fill in _seams(row, max_gap)
                         if pad <= n < pad + cells]
            for ci in range(cells):
                col = ([north[cj * cells + ci] for cj in range(cells - pad, cells)] if north else [None] * pad)
                col += [level[cj * cells + ci] for cj in range(cells)]
                col += ([south[cj * cells + ci] for cj in range(pad)] if south else [None] * pad)
                todo += [((n - pad) * cells + ci, fill) for n, fill in _seams(col, max_gap)
                         if pad <= n < pad + cells]
            target = out[(tx, ty)][1][k]
            for at, fill in todo:
                if target[at] is None:
                    target[at] = fill
                    filled += 1
    return out, filled


def clear_border(tiles, min_x, min_y, width, height, ring=BORDER_RING):
    """Make the outermost `ring` tiles of the baked area whole.

    The scene's ground ends somewhere inside its last tile, but the world goes
    on past it, so a mask hole there would be an invisible wall around the whole
    area rather than an edge. Every level of a border tile gets its floor in
    every cell, with open sky above, except a slope too steep to walk, which
    keeps its own heights. Returns a new dict; the input is not
    changed.
    """
    out = _copy(tiles)
    for tile, (floors, levels) in tiles.items():
        tx, ty = tile
        border = (tx - min_x < ring or min_x + width - 1 - tx < ring or
                  ty - min_y < ring or min_y + height - 1 - ty < ring)
        if border:
            out[tile] = (floors, [list(level) if any(is_steep(c) for c in level)
                                  else [(floor, OPEN)] * len(level)
                                  for floor, level in zip(floors, levels)])
    return out
