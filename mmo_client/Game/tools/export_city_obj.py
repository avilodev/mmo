"""Export the visible city of ennara_city.blend to ennara_city.obj for the game.

Run headless (from mmo_client/):

    blender -b Game/assets/models/all_assets/ennara_city.blend \
            --python Game/tools/export_city_obj.py

Everything visible is written except the reference character. The hidden
COLLISION collection and the excluded LIB collection are not visible, so they
stay out. The OBJ keeps Blender's metres, Y up, and the scene origin, which
render/city_renderer.h puts on Ennara's courtyard centre.
"""

import os
import sys

import bpy

OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       "..", "assets", "models", "all_assets")
# `-- <name>` after the script picks the output (default "ennara_city").
NAME = "ennara_city"
if "--" in sys.argv and sys.argv[sys.argv.index("--") + 1:]:
    NAME = sys.argv[sys.argv.index("--") + 1]
OUT_PATH = os.path.normpath(os.path.join(OUT_DIR, NAME + ".obj"))
SKIP_COLLECTIONS = {"REF_character", "COLLISION", "LIB"}


def exportable(obj):
    if obj.type != "MESH" or not obj.visible_get():
        return False
    return not any(c.name in SKIP_COLLECTIONS for c in obj.users_collection)


def flatten_texture_paths(mtl_path):
    """Blender writes a relative path that climbs out of the drive when the
    textures sit beside the OBJ; keep only the file name."""
    with open(mtl_path) as f:
        lines = f.read().splitlines()
    fixed = ["map_Kd " + os.path.basename(l[7:].replace("\\", "/"))
             if l.startswith("map_Kd ") else l for l in lines]
    with open(mtl_path, "w") as f:
        f.write("\n".join(fixed) + "\n")


def main():
    objects = [o for o in bpy.context.view_layer.objects if exportable(o)]
    bpy.ops.object.select_all(action="DESELECT")
    for obj in objects:
        obj.select_set(True)
    print("[CITY_OBJ] exporting %d objects to %s" % (len(objects), OUT_PATH))
    bpy.ops.wm.obj_export(
        filepath=OUT_PATH,
        export_selected_objects=True,
        apply_modifiers=True,
        export_uv=True,
        export_normals=True,
        export_materials=True,
        path_mode="RELATIVE",
        forward_axis="NEGATIVE_Z",
        up_axis="Y",
        global_scale=1.0,
    )
    flatten_texture_paths(os.path.splitext(OUT_PATH)[0] + ".mtl")
    print("[CITY_OBJ] done")


main()
