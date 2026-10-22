"""Turn per-tile ray samples into walkable spans. Pure: no bpy."""


def cluster_floors(cells, min_cells=8, gap=6.0):
    """Group upward-facing hits into floors; keep those most of a tile has.

    cells is one list per sub-sample of (z, ceil) hits. A floor needs hits from
    at least min_cells sub-samples, so a 0.2 m wall top does not count.
    """
    hits = sorted((z, ceil, i) for i, cell in enumerate(cells) for z, ceil in cell)
    clusters, current = [], []
    for hit in hits:
        if current and hit[0] - current[-1][0] > gap:
            clusters.append(current)
            current = []
        current.append(hit)
    if current:
        clusters.append(current)

    spans = []
    for cluster in clusters:
        if len({h[2] for h in cluster}) < min_cells:
            continue
        zs = sorted(h[0] for h in cluster)
        spans.append((zs[len(zs) // 2], min(h[1] for h in cluster)))
    return spans


def clip_spans(spans, blockers, body=40.0, step=12.0):
    """Drop a span when a solid rises past a step and starts inside the body."""
    kept = []
    for floor, ceil in spans:
        blocked = any(hi > floor + step and lo < floor + body for lo, hi in blockers)
        if not blocked:
            kept.append((floor, ceil))
    return kept


def usable(spans, body=40.0):
    return [(f, c) for f, c in spans if c - f >= body]


def resolve_overlaps(spans):
    """Sort spans and cut each one's ceiling at the floor of the next, so the
    list is ascending and no span overlaps another (what the loader demands).
    A span left with no height is dropped."""
    ordered = sorted(spans)
    out = []
    for i, (floor, ceil) in enumerate(ordered):
        if i + 1 < len(ordered):
            ceil = min(ceil, ordered[i + 1][0])
        if ceil > floor:
            out.append((floor, ceil))
    return out


def cluster_cells(cells, gap=6.0):
    """Group upward-facing hits into floors without discarding any.

    Returns a list of (floor, ceil, cell_ids): the median height of each group,
    its lowest ceiling, and which sub-samples hit it. Pair with merge_close().
    """
    hits = sorted((z, ceil, i) for i, cell in enumerate(cells) for z, ceil in cell)
    groups, current = [], []
    for hit in hits:
        if current and hit[0] - current[-1][0] > gap:
            groups.append(current)
            current = []
        current.append(hit)
    if current:
        groups.append(current)

    out = []
    for group in groups:
        zs = sorted(h[0] for h in group)
        out.append((zs[len(zs) // 2], min(h[1] for h in group), {h[2] for h in group}))
    return out


def merge_close(clusters, step=12.0, min_cells=8):
    """Merge floors within a step of each other (a doorway tile is half slab,
    half ground) and keep those that most of a tile has.

    clusters are (floor, ceil, cell_ids) as cluster_cells() returns, after any
    that a solid blocks have been removed, so a floor behind a bench is not
    merged into the bench's top.
    """
    merged = []
    for floor, ceil, ids in sorted(clusters, key=lambda c: c[0]):
        if merged and floor - merged[-1][3] <= step:
            f0, c0, i0, _ = merged[-1]
            n0, n1 = len(i0), len(ids)
            merged[-1] = ((f0 * n0 + floor * n1) / (n0 + n1), min(c0, ceil), i0 | ids, floor)
        else:
            merged.append((floor, ceil, set(ids), floor))
    return [(f, c) for f, c, ids, _ in merged if len(ids) >= min_cells]


def _touches_ground(grid, tile, floor, step):
    """Does a neighbouring tile have a span within a step of `floor`?"""
    tx, ty = tile
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            if (dx or dy) and any(abs(f - floor) <= step
                                  for f, _ in grid.get((tx + dx, ty + dy), ())):
                return True
    return False


def recover_spans(grid, clusters, skip, step=12.0, body=40.0, max_spans=8,
                  max_passes=8):
    """Add the floors the coarse bake dropped for covering too little of a tile.

    grid maps a tile to its ascending [(floor, ceil)]; clusters maps a tile to
    the (floor, ceil) of every fine floor found in it; skip holds tiles that
    must gain nothing. A fine floor becomes a span when the tile has none within
    a step of it and a neighbouring tile does, so a platform's edge keeps the
    ground below it, while a wall top or a lamp top, far above any neighbour's
    floor, never becomes one. A floor less than a body above another cuts that
    span's ceiling below a body's height; both are kept, since the floor mask
    gives each cell its own ceiling. Repeats until nothing is added, so a thin
    strip of ground chains outward. Returns a new grid; the input is not
    changed.
    """
    out = {tile: list(spans) for tile, spans in grid.items()}
    for _ in range(max_passes):
        added = {}
        for tile, found in clusters.items():
            have = out.get(tile, [])
            if tile in skip or len(have) >= max_spans:
                continue
            local = list(have)
            for floor, ceil in found:
                if len(local) >= max_spans:
                    break
                if ceil - floor < body:
                    continue
                if any(abs(floor - f) <= step for f, _ in local):
                    continue
                if not _touches_ground(out, tile, floor, step):
                    continue
                trial = resolve_overlaps(usable(local + [(floor, ceil)], body))
                # a floor that squeezes an existing span out is not worth having
                if all(any(f == f2 for f2, _ in trial) for f, _ in have):
                    local.append((floor, ceil))
            if len(local) > len(have):
                added[tile] = local
        if not added:
            break
        for tile, spans in added.items():
            out[tile] = resolve_overlaps(usable(spans, body))[:max_spans]
    return out
