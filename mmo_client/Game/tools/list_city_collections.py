"""List every collection and mesh of a city .blend, for planning the span bake.

Run headless (from mmo_client/):

    blender -b Game/assets/models/all_assets/test_town.blend \
            --python Game/tools/list_city_collections.py

Prints name, vertex count, bounding box (m) and the median face tilt (degrees
from horizontal) per object, and flags stairs, ramps and slopes.
"""

import math

import bpy

FLAGS = ("stair", "ramp", "slope", "hill", "terrace", "bridge", "deck")


def tilt_degrees(obj):
    normal_matrix = obj.matrix_world.to_3x3()
    tilts = sorted(math.degrees(math.acos(min(1.0, abs((normal_matrix @ p.normal).normalized().z))))
                   for p in obj.data.polygons)
    return tilts[len(tilts) // 2] if tilts else 0.0


def main():
    for coll in bpy.data.collections:
        print("== %s (%d objects, hidden=%s)" % (coll.name, len(coll.objects), coll.hide_viewport))
        for obj in coll.objects:
            if obj.type != "MESH":
                continue
            verts = [obj.matrix_world @ v.co for v in obj.data.vertices]
            if not verts:
                continue
            lo = [min(v[i] for v in verts) for i in range(3)]
            hi = [max(v[i] for v in verts) for i in range(3)]
            flag = " <-- WALKABLE?" if any(f in obj.name.lower() for f in FLAGS) else ""
            print("  %-28s v=%-6d x[%.1f,%.1f] y[%.1f,%.1f] z[%.2f,%.2f] tilt=%.0f%s"
                  % (obj.name, len(verts), lo[0], hi[0], lo[1], hi[1], lo[2], hi[2],
                     tilt_degrees(obj), flag))


main()
