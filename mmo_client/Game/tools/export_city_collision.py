"""Bake the city's COLLISION collection into Game/assets/baked/city_collision.bin.

Run headless (from mmo_client/):

    blender -b Game/assets/models/all_assets/ennara_city.blend \
            --python Game/tools/export_city_collision.py

Every mesh in the COLLISION collection that stands on the ground becomes solid
tiles. A small mesh (a prop, a tree, a bollard) is solid wherever any of its
triangles touches a tile, seen from above. A large mesh (a building) is a
hollow shell with floors, ceilings and interior walls, so only its walls count,
and only those at body height: filling the footprint would make every building
solid, and a wall on an upper storey would block the ground floor. Meshes that
start well above the ground (upper floors, a lighthouse lamp room) are skipped,
because nothing walkable stacks over anything else in the open world
(3d_refactor.md D4).

Tiles are relative to the scene origin, which the game puts on Ennara's
courtyard centre, so the file does not depend on the world's scale. The C side
is Game/tools/worldgen/worldgen_city_collision.c, which documents the file layout.
"""

import math
import os
import struct
import sys

import bpy

COLLECTION = "COLLISION"
# `-- <name>` after the script picks the output files: <name>_collision.bin and
# <name>_heights.bin (default "city", the Ennara files).
NAME = "city"
if "--" in sys.argv and sys.argv[sys.argv.index("--") + 1:]:
    NAME = sys.argv[sys.argv.index("--") + 1]

BAKED_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets", "baked")
OUT_PATH = os.path.normpath(os.path.join(BAKED_DIR, NAME + "_collision.bin"))

# Must match CHARACTER_HEIGHT / 1.83 (city_renderer.h) and WORLD_TILE_PX.
CHARACTER_HEIGHT = 40.0
CHARACTER_METRES = 1.83
TILE_PX = 16.0
TILES_PER_METRE = CHARACTER_HEIGHT / CHARACTER_METRES / TILE_PX

# A mesh counts as standing on the ground when its lowest point is under this,
# and as having a body when its highest point is over MIN_TOP.
MAX_BOTTOM_M = 1.5
MIN_TOP_M = 0.3

# A mesh whose footprint is at least this wide is a structure (walls only).
STRUCTURE_MIN_M = 3.0
# A wall blocks when it overlaps this band of height above the ground. Doorways
# are cut from the sill (under 0.5 m, which a character steps over) up to a
# lintel above 1.7 m, so a wider band seals every door.
WALL_BAND_M = (0.5, 1.7)
# Faces steeper than this from the vertical are floors, ceilings and roofs.
WALL_MAX_NORMAL_Z = 0.7

# A structure's walls are 0.2 m thick and a doorway about 1.1 m wide, against
# tiles of 0.73 m. Marking every tile a wall touches closes a doorway; instead
# each tile is cut into SUBCELLS x SUBCELLS and is solid only when at least
# MIN_WALL_SUBCELLS wall outline cells land in it (a wall running through a tile
# lands about 16, one split between two tiles about 8 in each, the end of a wall
# at a jamb 2 or 4). Pairs with the smaller player box in player.h.
SUBCELLS = 8
MIN_WALL_SUBCELLS = 6

FILE_MAGIC = b"CCOL"
FILE_VERSION = 1

# Alongside the collision, how tall each solid tile is, so the camera can tell a
# wall it must stay in front of from a fence it can see over. One byte per
# tile in HEIGHT_UNIT_M steps; 0 where nothing is solid.
HEIGHTS_PATH = os.path.normpath(os.path.join(BAKED_DIR, NAME + "_heights.bin"))
HEIGHTS_MAGIC = b"CHGT"
HEIGHTS_VERSION = 1
HEIGHT_UNIT_M = 0.25


def to_tile(bx, by):
    """Blender metres to tile coordinates; +X is east, -Y is south."""
    return bx * TILES_PER_METRE, -by * TILES_PER_METRE


def raise_tile(solid, tile, top):
    """Mark `tile` solid, keeping the tallest top seen for it (metres)."""
    if top > solid.get(tile, 0.0):
        solid[tile] = top


def mark_segment(solid, a, b, top):
    """Mark the tiles along a segment, for triangles seen edge-on."""
    length = math.hypot(b[0] - a[0], b[1] - a[1])
    steps = max(1, int(length * 4))
    for i in range(steps + 1):
        t = i / steps
        raise_tile(solid, (math.floor(a[0] + (b[0] - a[0]) * t),
                           math.floor(a[1] + (b[1] - a[1]) * t)), top)


def mark_triangle(solid, a, b, c, top):
    """Mark every tile whose square touches the triangle."""
    area2 = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])
    if abs(area2) < 1e-9:
        for p, q in ((a, b), (b, c), (c, a)):
            mark_segment(solid, p, q, top)
        return
    if area2 < 0:
        b, c = c, b

    edges = ((a, b), (b, c), (c, a))
    x0 = math.floor(min(a[0], b[0], c[0]))
    x1 = math.floor(max(a[0], b[0], c[0]))
    y0 = math.floor(min(a[1], b[1], c[1]))
    y1 = math.floor(max(a[1], b[1], c[1]))

    for ty in range(y0, y1 + 1):
        for tx in range(x0, x1 + 1):
            outside = False
            for p, q in edges:
                ex, ey = q[0] - p[0], q[1] - p[1]
                # The tile corner furthest to the inside of this edge.
                cx = tx + (1 if ey > 0 else 0)
                cy = ty + (1 if ex < 0 else 0)
                if ex * (cy - p[1]) - ey * (cx - p[0]) < 0:
                    outside = True
                    break
            if not outside:
                raise_tile(solid, (tx, ty), top)


def wall_cells(a, b, cells, top):
    """Record the sub-tile cells along the segment a-b in `cells`, with the
    tallest top (metres) that touches each."""
    length = math.hypot(b[0] - a[0], b[1] - a[1])
    steps = max(1, int(length * SUBCELLS * 2))
    for i in range(steps + 1):
        t = i / steps
        cell = (math.floor((a[0] + (b[0] - a[0]) * t) * SUBCELLS),
                math.floor((a[1] + (b[1] - a[1]) * t) * SUBCELLS))
        if top > cells.get(cell, 0.0):
            cells[cell] = top


def collect_solid_tiles(collection):
    """Return ({solid (tx, ty): top in metres}, kept object count, skipped count)."""
    solid = {}
    wall_counts = {}
    wall_tops = {}
    kept = skipped = 0
    for obj in collection.objects:
        if obj.type != "MESH":
            continue
        matrix = obj.matrix_world
        verts = [matrix @ v.co for v in obj.data.vertices]
        if not verts:
            continue
        bottom = min(v.z for v in verts)
        top = max(v.z for v in verts)
        if bottom > MAX_BOTTOM_M or top < MIN_TOP_M:
            skipped += 1
            continue
        kept += 1
        tiles = [to_tile(v.x, v.y) for v in verts]
        width = max(v.x for v in verts) - min(v.x for v in verts)
        depth = max(v.y for v in verts) - min(v.y for v in verts)
        structure = max(width, depth) >= STRUCTURE_MIN_M
        normal_matrix = matrix.to_3x3()
        cells = {}
        for poly in obj.data.polygons:
            idx = poly.vertices
            if structure:
                if abs((normal_matrix @ poly.normal).normalized().z) >= WALL_MAX_NORMAL_Z:
                    continue
                low = min(verts[i].z for i in idx)
                high = max(verts[i].z for i in idx)
                if high < WALL_BAND_M[0] or low > WALL_BAND_M[1]:
                    continue
                for k in range(len(idx)):
                    wall_cells(tiles[idx[k]], tiles[idx[(k + 1) % len(idx)]], cells, top)
                continue
            for k in range(1, len(idx) - 1):
                mark_triangle(solid, tiles[idx[0]], tiles[idx[k]], tiles[idx[k + 1]], top)
        for (cx, cy), cell_top in cells.items():
            key = (cx // SUBCELLS, cy // SUBCELLS)
            wall_counts[key] = wall_counts.get(key, 0) + 1
            wall_tops[key] = max(wall_tops.get(key, 0.0), cell_top)
    for tile, n in wall_counts.items():
        if n >= MIN_WALL_SUBCELLS:
            raise_tile(solid, tile, wall_tops[tile])
    return solid, kept, skipped


def write_file(path, solid):
    """Write the solid tiles as per-row runs of (start, length)."""
    min_x = min(x for x, _ in solid)
    max_x = max(x for x, _ in solid)
    min_y = min(y for _, y in solid)
    max_y = max(y for _, y in solid)
    width = max_x - min_x + 1
    height = max_y - min_y + 1

    rows = {}
    for x, y in solid:
        rows.setdefault(y, []).append(x)

    out = bytearray()
    out += FILE_MAGIC
    out += struct.pack("<Iiiii", FILE_VERSION, min_x, min_y, width, height)
    for y in range(min_y, max_y + 1):
        runs = []
        for x in sorted(rows.get(y, ())):
            if runs and runs[-1][0] + runs[-1][1] == x:
                runs[-1][1] += 1
            else:
                runs.append([x, 1])
        out += struct.pack("<H", len(runs))
        for start, length in runs:
            out += struct.pack("<HH", start - min_x, length)
    with open(path, "wb") as f:
        f.write(out)
    return width, height, len(out)


def write_heights(path, solid):
    """Write the tallest top of every solid tile, over the collision file's grid."""
    min_x = min(x for x, _ in solid)
    min_y = min(y for _, y in solid)
    width = max(x for x, _ in solid) - min_x + 1
    height = max(y for _, y in solid) - min_y + 1
    grid = bytearray(width * height)
    for (x, y), top in solid.items():
        units = int(math.ceil(max(top, HEIGHT_UNIT_M) / HEIGHT_UNIT_M))
        grid[(y - min_y) * width + (x - min_x)] = min(units, 255)
    with open(path, "wb") as f:
        f.write(HEIGHTS_MAGIC)
        f.write(struct.pack("<Iiiii", HEIGHTS_VERSION, min_x, min_y, width, height))
        f.write(grid)
    return len(grid) + 24


def main():
    collection = bpy.data.collections.get(COLLECTION)
    if collection is None:
        sys.exit("no %s collection in %s" % (COLLECTION, bpy.data.filepath))
    solid, kept, skipped = collect_solid_tiles(collection)
    if not solid:
        sys.exit("%s produced no solid tiles" % COLLECTION)
    width, height, size = write_file(OUT_PATH, solid)
    print("[COLLISION] %d meshes kept, %d skipped (above ground)" % (kept, skipped))
    print("[COLLISION] %d solid tiles in a %dx%d grid, %d bytes -> %s"
          % (len(solid), width, height, size, OUT_PATH))
    print("[COLLISION] heights, %d bytes -> %s" % (write_heights(HEIGHTS_PATH, solid), HEIGHTS_PATH))


main()
