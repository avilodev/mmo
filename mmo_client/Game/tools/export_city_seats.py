"""Bake the city's SIT_ markers into a seats file.

Run headless (from mmo_client/); the name picks <name>.blend's output file:

    blender -b Game/assets/models/all_assets/test_town.blend \
            --python Game/tools/export_city_seats.py -- test_town

Writes Game/assets/baked/<name>_seats.bin (layout below) and prints each seat.

A seat is an EMPTY named SIT_<something> (a single arrow, usually parented to
the bench or chair it belongs to). The OBJ export carries meshes only, so
markers never reach the game through it; this file is how they do.

    position  where the empty sits: the point a seated body is placed at
    facing    the way its arrow points (local +Z), flattened to the ground

File layout, little-endian:

    "CSIT", i32 version (1), i32 count,
    then per seat, 48 bytes, sorted by name:
        f32 x, f32 y      tiles from the scene origin, like city_spans.bin
        f32 z             height in world units
        f32 heading       radians; facing is (sin, cos) in world x, y, the
                          same heading the client's locomotion uses
        char name[32]     the empty's name, NUL padded

x and y are tiles with a fraction (a seat is not on a tile corner); y points
south, so Blender's +Y is -y here, exactly as in export_city_spans.py.
"""

import math
import os
import struct
import sys

import bpy
from mathutils import Vector

BAKED_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets", "baked")

# Must match CHARACTER_HEIGHT / 1.83 (city_renderer.h) and WORLD_TILE_PX.
CHARACTER_HEIGHT = 40.0
CHARACTER_METRES = 1.83
TILE_PX = 16.0
UNITS_PER_METRE = CHARACTER_HEIGHT / CHARACTER_METRES
TILES_PER_METRE = UNITS_PER_METRE / TILE_PX

PREFIX = "SIT_"
NAME_BYTES = 32
FILE_MAGIC = b"CSIT"
FILE_VERSION = 1
MIN_FLAT_FACING = 1e-3


def seat_from(obj):
    """(name, x, y, z, heading) for one marker, in game units."""
    world = obj.matrix_world
    pos = world.translation
    arrow = world.to_3x3() @ Vector((0.0, 0.0, 1.0))
    fx, fy = arrow.x, -arrow.y
    if math.hypot(fx, fy) < MIN_FLAT_FACING:
        sys.exit("%s points straight up or down; it has no facing" % obj.name)
    name = obj.name.encode("ascii")
    if len(name) >= NAME_BYTES:
        sys.exit("%s is too long (max %d characters)" % (obj.name, NAME_BYTES - 1))
    return (name, pos.x * TILES_PER_METRE, -pos.y * TILES_PER_METRE,
            pos.z * UNITS_PER_METRE, math.atan2(fx, fy))


def write_file(path, seats):
    out = bytearray()
    out += FILE_MAGIC
    out += struct.pack("<ii", FILE_VERSION, len(seats))
    for name, x, y, z, heading in seats:
        out += struct.pack("<ffff%ds" % NAME_BYTES, x, y, z, heading, name)
    with open(path, "wb") as f:
        f.write(out)
    return len(out)


def main():
    name = "test_town"
    if "--" in sys.argv:
        rest = sys.argv[sys.argv.index("--") + 1:]
        if rest:
            name = rest[0]
    out_path = os.path.normpath(os.path.join(BAKED_DIR, name + "_seats.bin"))

    bpy.context.view_layer.update()
    markers = sorted((o for o in bpy.data.objects
                      if o.type == "EMPTY" and o.name.startswith(PREFIX)),
                     key=lambda o: o.name)
    seats = [seat_from(o) for o in markers]
    for seat_name, x, y, z, heading in seats:
        print("[SEATS] %-16s tile (%.2f, %.2f) z %.1f heading %.1f deg"
              % (seat_name.decode(), x, y, z, math.degrees(heading)))
    if not seats:
        print("[SEATS] no %s markers in %s" % (PREFIX, bpy.data.filepath))
    size = write_file(out_path, seats)
    print("[SEATS] %d seats, %d bytes -> %s" % (len(seats), size, out_path))


if __name__ == "__main__":
    main()
