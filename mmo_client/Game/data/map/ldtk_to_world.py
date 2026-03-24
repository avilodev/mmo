#!/usr/bin/env python3
"""
LDtk JSON -> world.dat converter
Usage: python ldtk_to_world.py [map.json] [output.dat]
"""

import json, struct, sys, os

GRID = 16  # tile size in pixels

# Which LDtk layer names go where
BASE_LAYERS       = {'Ground', 'Path'}
OVERLAY_FLOOR     = {'Floor'}                  # always visible, behind player
OVERLAY_INTERIOR  = {'Interior', 'Decoration'} # only visible when player is inside, behind player
OVERLAY_ABOVE     = {'Wall', 'Roof'}           # in front of player when inside
COLLISION_LAYERS  = {'Wall'}


def convert(ldtk_path, out_path):
    map_dir = os.path.dirname(os.path.abspath(ldtk_path))

    with open(ldtk_path, encoding='utf-8') as f:
        data = json.load(f)

    level = data['levels'][0]
    w = level['pxWid'] // GRID
    h = level['pxHei'] // GRID
    print(f'Map size: {w}x{h} tiles')

    # Build tileset lookup: uid -> {pxWid, pxHei, gridSize, relPath}
    uid_to_def = {ts['uid']: ts for ts in data['defs']['tilesets']}

    # Collect which tileset UIDs are actually painted in the map
    used_uids = []
    for layer in level['layerInstances']:
        uid = layer.get('__tilesetDefUid')
        tiles = layer.get('gridTiles', []) + layer.get('autoLayerTiles', [])
        if uid and tiles and uid not in used_uids:
            used_uids.append(uid)

    # Assign tileset IDs (1-based; 0 = empty)
    uid_to_id   = {}
    tileset_list = []  # (stored_path, cols, rows)
    for uid in used_uids:
        ts   = uid_to_def[uid]
        grid = ts['tileGridSize']
        cols = ts['pxWid'] // grid if ts['pxWid'] > 0 else 1
        rows = ts['pxHei'] // grid if ts['pxHei'] > 0 else 1
        rel  = ts.get('relPath') or ''
        # Make path relative to game CWD (project root)
        # relPath is relative to the .ldtk file, map_dir is Game/data/map
        if rel:
            abs_path = os.path.normpath(os.path.join(map_dir, rel))
            stored   = abs_path.replace('\\', '/') \
                               .replace(os.path.abspath('.').replace('\\','/') + '/', '')
        else:
            stored = ''
        ts_id = len(tileset_list) + 1
        uid_to_id[uid]  = ts_id
        tileset_list.append((stored, cols, rows))
        print(f'  Tileset [{ts_id}] {os.path.basename(stored)} ({cols}x{rows})')

    # Allocate flat arrays
    base_tiles             = [0] * (w * h)
    overlay_floor_tiles    = [0] * (w * h)
    overlay_interior_tiles = [0] * (w * h)
    overlay_above_tiles    = [0] * (w * h)
    collision              = [0] * (w * h)

    def pack(ts_id, src_x, src_y, cols):
        idx = (src_x // GRID) + (src_y // GRID) * cols
        if idx > 0xFFF:
            idx = 0xFFF
        return (ts_id << 12) | idx

    # Process layers bottom-to-top (LDtk stores them top-first, so reversed)
    for layer in reversed(level['layerInstances']):
        name = layer['__identifier']
        uid  = layer.get('__tilesetDefUid')
        tiles = layer.get('gridTiles', []) + layer.get('autoLayerTiles', [])
        if not tiles or uid not in uid_to_id:
            continue

        ts_id = uid_to_id[uid]
        ts    = uid_to_def[uid]
        cols  = ts['pxWid'] // ts['tileGridSize']

        for t in tiles:
            tx = t['px'][0] // GRID
            ty = t['px'][1] // GRID
            if tx < 0 or ty < 0 or tx >= w or ty >= h:
                continue
            flat = ty * w + tx
            packed = pack(ts_id, t['src'][0], t['src'][1], cols)

            if name in BASE_LAYERS:
                base_tiles[flat] = packed
            elif name in OVERLAY_FLOOR:
                overlay_floor_tiles[flat] = packed
            elif name in OVERLAY_INTERIOR:
                overlay_interior_tiles[flat] = packed
            elif name in OVERLAY_ABOVE:
                overlay_above_tiles[flat] = packed

            if name in COLLISION_LAYERS:
                collision[flat] = 1

    # Write world.dat
    with open(out_path, 'wb') as f:
        # Header: width, height, tile_size
        f.write(struct.pack('<iii', w, h, GRID))

        # Tileset table
        f.write(struct.pack('B', len(tileset_list)))
        for (path, cols, rows) in tileset_list:
            pb = path.encode('utf-8')
            f.write(struct.pack('B', len(pb)))
            f.write(pb)
            f.write(struct.pack('<HH', cols, rows))

        # Tile data
        f.write(struct.pack(f'<{w*h}H', *base_tiles))
        f.write(struct.pack(f'<{w*h}H', *overlay_floor_tiles))
        f.write(struct.pack(f'<{w*h}H', *overlay_interior_tiles))
        f.write(struct.pack(f'<{w*h}H', *overlay_above_tiles))
        f.write(bytes(collision))

    size = os.path.getsize(out_path)
    print(f'Written: {out_path} ({size} bytes)')


if __name__ == '__main__':
    ldtk = sys.argv[1] if len(sys.argv) > 1 else 'Game/data/map/map.json'
    out  = sys.argv[2] if len(sys.argv) > 2 else 'Game/bin/world.dat'
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    convert(ldtk, out)
