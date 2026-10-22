"""Bake the city's COLLISION collection into walkable spans.

Run headless (from mmo_client/); the name picks <name>.blend's output file:

    blender -b Game/assets/models/all_assets/test_town.blend \
            --python Game/tools/export_city_spans.py -- test_town

Writes Game/assets/baked/<name>_spans.bin (layout below), the camera's
Game/assets/baked/<name>_heights.bin and the floor mask Game/assets/baked/<name>_footing.bin
(layout in footing_math.py; the game reads it for the feet and for collision),
and prints a summary.

Every tile is sampled by 4x4 rays straight down through the COLLISION meshes.
Each upward-facing hit (a face tilted at most 45 degrees) is a floor, and the
next hit above it of any face is its ceiling. A floor with under a body of
headroom (the ground under a slab, the inner treads of a stack of step boxes)
is dropped first, then the floors most of a tile has become its spans. No
solid removes a span: what stands on part of a tile is in the floor mask, cell
by cell. The mask's 16x16 rays see a solid wherever it covers a cell's centre;
the steep triangles of every object, whatever it is, are then rasterised to
block what the rays cannot see (solid_math.py). A span left with no floor in
any cell is dropped, so a tile that is solid all over has none. Surfaces
steeper than 45 degrees and up to 62 are slopes: they get levels of their own,
marked steep, which a body slides down and never climbs.

File layout, little-endian, one record per tile of the grid, row-major:

    "CSPN", i32 version (1), i32 min_x, i32 min_y, i32 width, i32 height,
    then per tile: u8 count, then count x (f32 floor, f32 ceil) in world units.

Tiles are relative to the scene origin, like city_collision.bin. The C side is
Game/tools/worldgen/worldgen_city_spans.c.
"""

import math
import os
import struct
import sys

import bpy
from mathutils import Vector
from mathutils.bvhtree import BVHTree

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
BAKED_DIR = os.path.normpath(os.path.join(TOOLS_DIR, "..", "assets", "baked"))
sys.path.insert(0, TOOLS_DIR)

from span_math import (cluster_cells, merge_close, recover_spans,  # noqa: E402
                       resolve_overlaps, usable)
from footing_math import (CELLS_PER_TILE, add_levels, encode_level,  # noqa: E402
                          encode_tile, pack_file, steep_levels, tile_levels)
from solid_math import face_occupancy, finish_mask  # noqa: E402

COLLECTION = "COLLISION"

# Must match CHARACTER_HEIGHT / 1.83 (city_renderer.h) and WORLD_TILE_PX.
CHARACTER_HEIGHT = 40.0
CHARACTER_METRES = 1.83
TILE_PX = 16.0
UNITS_PER_METRE = CHARACTER_HEIGHT / CHARACTER_METRES
TILES_PER_METRE = UNITS_PER_METRE / TILE_PX

BODY = 40.0
STEP = 12.0
OPEN_CEIL = 1.0e9
MAX_SPANS_PER_TILE = 8

RAYS = 4
MIN_FLOOR_CELLS = 8
# Hits this close are one tread or one slope; floors within a step of each
# other are then merged (a doorway tile is half interior slab, half outside
# ground).
FLOOR_GAP = 6.0
# 45 degrees is still walkable; a float normal of a 45-degree face falls a hair
# short of cos(45)
WALK_MIN_NORMAL_Z = math.cos(math.radians(45.0)) - 1e-4
# Steeper than walkable but no steeper than this is a slope to slide down;
# beyond it a body's uphill corner would be more than a step into it: a wall
STEEP_MIN_NORMAL_Z = math.cos(math.radians(62.0))

FILE_MAGIC = b"CSPN"
FILE_VERSION = 1


def to_tile(point):
    """Blender metres to tile coordinates plus height in world units."""
    return Vector((point.x * TILES_PER_METRE, -point.y * TILES_PER_METRE,
                   point.z * UNITS_PER_METRE))


def from_tile(tx, ty):
    return tx / TILES_PER_METRE, -ty / TILES_PER_METRE


def collect(collection):
    """Return (verts, tris): world-space verts in metres and vertex index
    triples."""
    deps = bpy.context.evaluated_depsgraph_get()
    verts, tris = [], []
    for obj in collection.objects:
        if obj.type != "MESH":
            continue
        evaluated = obj.evaluated_get(deps)
        mesh = evaluated.to_mesh()
        mesh.calc_loop_triangles()
        base = len(verts)
        world = [obj.matrix_world @ v.co for v in mesh.vertices]
        if not world:
            evaluated.to_mesh_clear()
            continue
        verts.extend(world)
        # A mirrored object (negative scale) flips its winding, which would turn
        # its floors into ceilings; swap two corners to put it right.
        flip = obj.matrix_world.determinant() < 0
        for tri in mesh.loop_triangles:
            a, b, c = tri.vertices
            if flip:
                b, c = c, b
            tris.append((base + a, base + b, base + c))
        evaluated.to_mesh_clear()
    return verts, tris


def steep_faces(verts, tris):
    """Every triangle too steep to walk on, as solid_math's faces: corners in
    tile coordinates and the horizontal part of its normal, in tile axes. A
    triangle with no area is skipped."""
    faces = []
    for tri in tris:
        normal = (verts[tri[1]] - verts[tri[0]]).cross(verts[tri[2]] - verts[tri[0]])
        if normal.length < 1e-12:
            continue
        normal.normalize()
        if abs(normal.z) >= WALK_MIN_NORMAL_Z:
            continue
        # tile y runs against Blender's y (to_tile)
        nx, ny = normal.x, -normal.y
        flat = math.hypot(nx, ny)
        a, b, c = (tuple(to_tile(verts[i])) for i in tri)
        faces.append((a, b, c, nx / flat, ny / flat))
    return faces


def sample_hits(bvh, tx, ty, z_top):
    """Floor and slope hits of one sub-sample ray: ([(floor, ceil)],
    [(slope, ceil)]) in world units, a slope being a surface too steep to walk
    and not so steep it is a wall."""
    bx, by = from_tile(tx, ty)
    origin = Vector((bx, by, z_top))
    down = Vector((0.0, 0.0, -1.0))
    hits = []
    for _ in range(64):
        loc, normal, index, _dist = bvh.ray_cast(origin, down)
        if loc is None:
            break
        z = loc.z * UNITS_PER_METRE
        if hits and abs(hits[-1][0] - z) < 0.05 and (normal.z >= 0.0) == (hits[-1][1] >= 0.0):
            pass
        else:
            hits.append((z, normal.z))
        origin = Vector((bx, by, loc.z - 1e-4))
    hits.sort()

    floors = []
    slopes = []
    for i, (z, nz) in enumerate(hits):
        if nz < STEEP_MIN_NORMAL_Z:
            continue
        ceil = OPEN_CEIL
        for z2, _ in hits[i + 1:]:
            if z2 - z > 0.05:
                ceil = z2
                break
        (floors if nz >= WALK_MIN_NORMAL_Z else slopes).append((z, ceil))
    return floors, slopes


def sample_cell(bvh, tx, ty, z_top):
    """Floor hits of one sub-sample ray: [(floor, ceil)] in world units."""
    return sample_hits(bvh, tx, ty, z_top)[0]


def bake_tile(bvh, tx, ty, z_top):
    cells = []
    for j in range(RAYS):
        for i in range(RAYS):
            cells.append(sample_cell(bvh, tx + (i + 0.5) / RAYS, ty + (j + 0.5) / RAYS, z_top))
    # A floor with no room for a body above it (the ground under a slab or a
    # stair) must not drag a nearby real floor's ceiling down when they cluster.
    cells = [usable(cell, BODY) for cell in cells]
    spans = merge_close(cluster_cells(cells, FLOOR_GAP), STEP, MIN_FLOOR_CELLS)
    # a floor less than a body above another (a low platform's foot) cuts its
    # ceiling; the mask gives each cell its own, so both stay
    return resolve_overlaps(usable(spans, BODY))[:MAX_SPANS_PER_TILE]


def fine_hits(bvh, tx, ty, z_top, count):
    """(floors, slopes) under a count x count grid of rays across one tile, one
    list per cell, row-major: the usable floors (floor, ceil), and the heights
    of slopes with room for a body above. Anything with no room for a body is
    left out, as in the span bake."""
    cells = []
    slopes = []
    for j in range(count):
        for i in range(count):
            floors, steep = sample_hits(bvh, tx + (i + 0.5) / count, ty + (j + 0.5) / count,
                                        z_top)
            cells.append(usable(floors, BODY))
            slopes.append([z for z, _ in usable(steep, BODY)])
    return cells, slopes


def fine_clusters(cells):
    """Every floor of a tile found by the fine rays: [(floor, ceil)]."""
    return [(floor, ceil) for floor, ceil, _ in cluster_cells(cells, FLOOR_GAP)]


def summarize(grid, min_x, min_y, width, height):
    counts = {}
    tallest = (0.0, None)
    for (tx, ty), spans in grid.items():
        counts[min(len(spans), 2)] = counts.get(min(len(spans), 2), 0) + 1
        for floor, _ in spans:
            if floor > tallest[0]:
                tallest = (floor, (tx, ty))
    print("[SPANS] tiles: %d with no span, %d with one, %d with two or more"
          % (counts.get(0, 0), counts.get(1, 0), counts.get(2, 0)))
    print("[SPANS] tallest floor %.1f units at tile %s" % tallest)

    jumps = []
    for (tx, ty), spans in grid.items():
        for nx, ny in ((tx + 1, ty), (tx, ty + 1)):
            other = grid.get((nx, ny))
            if not other:
                continue
            for floor, _ in spans:
                near = min(abs(floor - f2) for f2, _ in other)
                if near > STEP and near < 40.0:
                    jumps.append((near, (tx, ty), (nx, ny)))
    jumps.sort(reverse=True)
    print("[SPANS] %d neighbouring floors differ by more than a step (%.0f)" % (len(jumps), STEP))
    for near, a, b in jumps[:20]:
        print("[SPANS]   %.1f between tiles %s and %s" % (near, a, b))


HEIGHT_UNIT_M = 0.25


def solid_tops(occupancy):
    """Per tile, how tall the things that stop a camera are, in metres: the
    top of the steep faces in it. Walkable surfaces (stair treads, terrace
    tops, ramps) are not here, so the camera is not pulled in by the floor the
    player walks on."""
    tops = {}
    for (gx, gy), faces in occupancy.items():
        tile = (gx // CELLS_PER_TILE, gy // CELLS_PER_TILE)
        top = max(face[1] for face in faces) / UNITS_PER_METRE
        tops[tile] = max(tops.get(tile, 0.0), top)
    return tops


def write_heights(path, tops, min_x, min_y, width, height):
    """city_heights.bin layout: "CHGT", u32 version, i32 min_x, min_y, width,
    height, then a byte per tile in HEIGHT_UNIT_M steps (0 where nothing)."""
    grid = bytearray(width * height)
    for (tx, ty), top in tops.items():
        if min_x <= tx < min_x + width and min_y <= ty < min_y + height and top > 0.0:
            units = int(math.ceil(max(top, HEIGHT_UNIT_M) / HEIGHT_UNIT_M))
            grid[(ty - min_y) * width + (tx - min_x)] = min(units, 255)
    with open(path, "wb") as f:
        f.write(b"CHGT")
        f.write(struct.pack("<Iiiii", 1, min_x, min_y, width, height))
        f.write(grid)
    return len(grid) + 24


def write_file(path, grid, min_x, min_y, width, height):
    out = bytearray()
    out += FILE_MAGIC
    out += struct.pack("<iiiii", FILE_VERSION, min_x, min_y, width, height)
    for ty in range(min_y, min_y + height):
        for tx in range(min_x, min_x + width):
            spans = grid[(tx, ty)]
            out += struct.pack("<B", len(spans))
            for floor, ceil in spans:
                out += struct.pack("<ff", floor, ceil)
    with open(path, "wb") as f:
        f.write(out)
    return len(out)


def main():
    name = "test_town"
    if "--" in sys.argv:
        rest = sys.argv[sys.argv.index("--") + 1:]
        if rest:
            name = rest[0]
    out_path = os.path.join(BAKED_DIR, name + "_spans.bin")

    collection = bpy.data.collections.get(COLLECTION)
    if collection is None:
        sys.exit("no %s collection in %s" % (COLLECTION, bpy.data.filepath))
    verts, tris = collect(collection)
    if not tris:
        sys.exit("%s has no triangles" % COLLECTION)
    print("[SPANS] %d triangles from %s" % (len(tris), COLLECTION))

    bvh = BVHTree.FromPolygons(verts, tris, epsilon=0.0)
    faces = steep_faces(verts, tris)
    occupancy = face_occupancy(faces, CELLS_PER_TILE)
    print("[SPANS] %d steep triangles touch %d cells" % (len(faces), len(occupancy)))

    xs = [to_tile(v).x for v in verts]
    ys = [to_tile(v).y for v in verts]
    min_x, max_x = math.floor(min(xs)), math.floor(max(xs))
    min_y, max_y = math.floor(min(ys)), math.floor(max(ys))
    width, height = max_x - min_x + 1, max_y - min_y + 1
    z_top = max(v.z for v in verts) + 1.0
    print("[SPANS] grid %dx%d tiles from (%d, %d)" % (width, height, min_x, min_y))

    grid = {}
    fine = {}
    slopes = {}
    for ty in range(min_y, max_y + 1):
        for tx in range(min_x, max_x + 1):
            grid[(tx, ty)] = bake_tile(bvh, tx, ty, z_top)
            fine[(tx, ty)], slopes[(tx, ty)] = fine_hits(bvh, tx, ty, z_top, CELLS_PER_TILE)

    # Floors the coarse rays dropped for covering too little of a tile: the
    # ground below a platform's edge, a narrow rim above a drop.
    found = {tile: fine_clusters(cells) for tile, cells in fine.items()}
    before = sum(len(spans) for spans in grid.values())
    grid = recover_spans(grid, found, set(), STEP, BODY, MAX_SPANS_PER_TILE)
    print("[SPANS] recovered %d spans the coarse bake dropped"
          % (sum(len(spans) for spans in grid.values()) - before))

    # The mask: which cells of each span have floor (footing_math.py), then
    # what solids block (solid_math.py). It can drop spans, so the spans file
    # is written after it.
    tiles = {}
    for tile, spans in grid.items():
        if spans:
            floors = [floor for floor, _ in spans]
            tiles[tile] = (floors, tile_levels(fine[tile], floors))
    # Slopes too steep to walk get levels of their own: open air above them,
    # never climbed, slid down (common/world_spans.c)
    added = 0
    for tile, cells in slopes.items():
        found_slopes = steep_levels(cells)
        if not found_slopes:
            continue
        levels = tiles[tile][1] if tile in tiles else []
        spans, floors, levels = add_levels(grid[tile], levels, found_slopes,
                                           MAX_SPANS_PER_TILE)
        added += len(spans) - len(grid[tile])
        grid[tile] = spans
        tiles[tile] = (floors, levels)
    print("[SPANS] %d slopes too steep to walk" % added)
    grid, tiles, report = finish_mask(grid, tiles, occupancy, (min_x, min_y, width, height))
    print("[SPANS] closed %d narrow seams and %d narrow holes in the mask"
          % (report["seams"], report["holes"]))
    print("[SPANS] faces blocked %d floor cells and carried %d rims over their faces"
          % (report["blocked"], report["rims"]))
    print("[SPANS] thin solids widened by %d cells; %d spans left with no floor dropped"
          % (report["widened"], report["dropped"]))

    summarize(grid, min_x, min_y, width, height)
    size = write_file(out_path, grid, min_x, min_y, width, height)
    print("[SPANS] %d bytes -> %s" % (size, out_path))

    heights_path = os.path.join(BAKED_DIR, name + "_heights.bin")
    size = write_heights(heights_path, solid_tops(occupancy), min_x, min_y, width, height)
    print("[SPANS] camera heights, %d bytes -> %s" % (size, heights_path))

    footing = {}
    bare = 0
    for tile, (_, levels) in tiles.items():
        encoded = encode_tile(levels)
        # a span with under a body of room at tile level is only usable through
        # the ceilings the mask gives its cells
        if not encoded and any(ceil - floor < BODY for floor, ceil in grid[tile]):
            encoded = [encode_level(level) for level in levels]
        if encoded:
            footing[tile] = encoded
        elif any(cell is None for level in levels for cell in level):
            bare += 1
    if bare:
        sys.exit("%d tiles with a blocked cell have no mask" % bare)
    footing_path = os.path.join(BAKED_DIR, name + "_footing.bin")
    data = pack_file(min_x, min_y, width, height, footing)
    with open(footing_path, "wb") as f:
        f.write(data)
    levels = sum(len(encoded) for encoded in footing.values())
    print("[SPANS] mask: %d tiles with a mask, %d levels, %d bytes -> %s"
          % (len(footing), levels, len(data), footing_path))


if __name__ == "__main__":
    main()
