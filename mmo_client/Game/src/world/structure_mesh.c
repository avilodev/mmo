/**
 * @file
 * Build lit 3D geometry for a chunk's solid tiles.
 *
 * Two kinds of structure come out of here. Buildings, blocks and rock are
 * built tile by tile, so they cover their solid tiles exactly. Props -- a
 * column, a lamp, a tree -- are one model per footprint, emitted by the
 * footprint's north-west tile and sized from the footprint, so the solid
 * tiles the server blocks are exactly the ones the prop stands on.
 */

#include "world/structure_mesh.h"
#include "world/tile_palette.h"
#include "render/world_light.h"

#include <math.h>
#include <string.h>

/** Bottom-of-wall darkening, a cheap stand-in for ambient occlusion so walls
 *  read as meeting the ground rather than floating on it. */
#define STRUCTURE_WALL_FOOT_SHADE 0.72f

/** A lantern glows: brighter than anything the sun lights. */
#define STRUCTURE_LANTERN_GLOW 1.25f

/** Props are never more than this many tiles across. */
#define STRUCTURE_PROP_MAX_TILES 4

#define PI_F 3.14159265f

StructureTile structure_tile_of(uint16_t base, uint16_t above, uint8_t collision) {
    StructureTile t = { STRUCTURE_NONE, base };
    if (!collision) return t;
    switch (base) {
        case PAL_CITY_WALL:
        case PAL_COURTYARD_WALL:
        case PAL_GATE_TOWER:    t.kind = STRUCTURE_BLOCK;  return t;
        case PAL_MOUNTAIN_ROCK: t.kind = STRUCTURE_ROCK;   return t;
        case PAL_COLUMN:        t.kind = STRUCTURE_COLUMN; return t;
        case PAL_LAMP_POST:     t.kind = STRUCTURE_LAMP;   return t;
        case PAL_TREE:          t.kind = STRUCTURE_TREE;   return t;
        default: break;
    }
    if (above == PAL_BUILDING_ROOF) { t.kind = STRUCTURE_BUILDING; t.style = above; }
    return t;
}

static StructureTile tile(const StructureMeshInput* in, int tx, int ty) {
    StructureTile none = { STRUCTURE_NONE, 0 };
    if (tx < 0 || ty < 0 || tx >= in->world_w || ty >= in->world_h) return none;
    return in->tile_at(in->ctx, tx, ty);
}

static StructureKind kind(const StructureMeshInput* in, int tx, int ty) {
    return tile(in, tx, ty).kind;
}

static float block_height(uint16_t style) {
    switch (style) {
        case PAL_COURTYARD_WALL: return STRUCTURE_COURTYARD_WALL_HEIGHT;
        case PAL_GATE_TOWER:     return STRUCTURE_GATE_TOWER_HEIGHT;
        default:                 return STRUCTURE_CITY_WALL_HEIGHT;
    }
}

/* ---- heights ----------------------------------------------------------- */

/** Whether all four tiles touching grid vertex (vx, vy) are `k`. */
static int corner_surrounded(const StructureMeshInput* in, int vx, int vy, StructureKind k) {
    return kind(in, vx - 1, vy - 1) == k && kind(in, vx, vy - 1) == k &&
           kind(in, vx - 1, vy)     == k && kind(in, vx, vy)     == k;
}

/** Consecutive tiles of `k` from (tx, ty) stepping by (sx, sy), capped. */
static int run_length(const StructureMeshInput* in, int tx, int ty, int sx, int sy,
                      StructureKind k, int limit) {
    int n = 0;
    while (n < limit && kind(in, tx + sx * n, ty + sy * n) == k) n++;
    return n;
}

float structure_roof_height(const StructureMeshInput* in, int vx, int vy) {
    const StructureKind b = STRUCTURE_BUILDING;
    const int lim = STRUCTURE_SCAN_LIMIT;
    if (!corner_surrounded(in, vx, vy, b)) return STRUCTURE_BUILDING_EAVE;

    /* Distance from this corner to the nearest edge of the building, in
     * tiles, measured along the axes. Rising with it gives a hip roof. */
    int d = run_length(in, vx - 1, vy, -1, 0, b, lim);
    int r = run_length(in, vx,     vy,  1, 0, b, lim);
    int u = run_length(in, vx, vy - 1,  0, -1, b, lim);
    int w = run_length(in, vx, vy,      0,  1, b, lim);
    if (r < d) d = r;
    if (u < d) d = u;
    if (w < d) d = w;

    float rise = (float)d * STRUCTURE_ROOF_RISE;
    if (rise > STRUCTURE_ROOF_MAX_RISE) rise = STRUCTURE_ROOF_MAX_RISE;
    return STRUCTURE_BUILDING_EAVE + rise;
}

/** A stable value in [0, 1) for a grid point. */
static float hash01(int x, int y) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return (float)(h & 0xFFFFu) / 65536.0f;
}

static float rock_height(const StructureMeshInput* in, int vx, int vy) {
    if (!corner_surrounded(in, vx, vy, STRUCTURE_ROCK)) return 0.0f;
    return STRUCTURE_ROCK_BASE + hash01(vx, vy) * STRUCTURE_ROCK_NOISE;
}

/* ---- emitting ---------------------------------------------------------- */

/** Where vertices go, and how many more fit. */
typedef struct {
    ChunkVertex* v;
    int n;
    int cap;
} Builder;

/** A point in world (x, y) at height h. */
typedef struct { float x, y, h; } P3;

static uint8_t to_byte(float c) {
    if (c <= 0.0f) return 0;
    if (c >= 1.0f) return 255;
    return (uint8_t)lroundf(c * 255.0f);
}

static void put(ChunkVertex* v, P3 p, const float rgb[3], float light) {
    v->x = p.x; v->y = p.y; v->h = p.h;
    v->u = 0.0f; v->v = 0.0f;
    v->r = to_byte(rgb[0] * light);
    v->g = to_byte(rgb[1] * light);
    v->b = to_byte(rgb[2] * light);
    v->a = 255;
}

/** One triangle with each corner's own brightness. */
static void tri3(Builder* b, P3 p0, P3 p1, P3 p2, const float rgb[3],
                 float l0, float l1, float l2) {
    if (b->n + 3 > b->cap) return;
    put(&b->v[b->n++], p0, rgb, l0);
    put(&b->v[b->n++], p1, rgb, l1);
    put(&b->v[b->n++], p2, rgb, l2);
}

/**
 * One triangle, lit by its own face normal. The normal is turned to face up
 * when `up`, and otherwise away from `ref` -- the inside of whatever the
 * triangle is the skin of.
 */
static void lit_tri(Builder* b, P3 p0, P3 p1, P3 p2, const float rgb[3], int up, P3 ref) {
    /* GL space is (x, h, y). */
    float e1x = p1.x - p0.x, e1h = p1.h - p0.h, e1y = p1.y - p0.y;
    float e2x = p2.x - p0.x, e2h = p2.h - p0.h, e2y = p2.y - p0.y;
    float nx = e1h * e2y - e1y * e2h;
    float nh = e1y * e2x - e1x * e2y;
    float ny = e1x * e2h - e1h * e2x;
    float len = sqrtf(nx * nx + nh * nh + ny * ny);
    if (len < 1e-6f) return;
    nx /= len; nh /= len; ny /= len;

    float flip;
    if (up) {
        flip = nh;
    } else {
        float cx = (p0.x + p1.x + p2.x) / 3.0f - ref.x;
        float ch = (p0.h + p1.h + p2.h) / 3.0f - ref.h;
        float cy = (p0.y + p1.y + p2.y) / 3.0f - ref.y;
        flip = nx * cx + nh * ch + ny * cy;
    }
    if (flip < 0.0f) { nx = -nx; nh = -nh; ny = -ny; }

    float light = world_light(nx, nh, ny);
    tri3(b, p0, p1, p2, rgb, light, light, light);
}

/** A tile's top from its four corner heights (00 = north-west). */
static void emit_top(Builder* b, float x0, float y0, float s,
                     float h00, float h10, float h11, float h01, const float rgb[3]) {
    P3 c00 = { x0,     y0,     h00 }, c10 = { x0 + s, y0,     h10 };
    P3 c11 = { x0 + s, y0 + s, h11 }, c01 = { x0,     y0 + s, h01 };
    P3 none = { 0 };
    /* Fold along the diagonal whose ends differ most: that is where a hip
     * roof's crease runs, and on a plain slope either fold is the same. */
    if (fabsf(h00 - h11) >= fabsf(h10 - h01)) {
        lit_tri(b, c00, c10, c11, rgb, 1, none);
        lit_tri(b, c00, c11, c01, rgb, 1, none);
    } else {
        lit_tri(b, c10, c11, c01, rgb, 1, none);
        lit_tri(b, c10, c01, c00, rgb, 1, none);
    }
}

/** A vertical wall quad from (ax, ay) to (bx, by), `bottom` to `top`, facing
 *  GL direction (nx, 0, nz). Darker at the foot when it stands on the ground. */
static void emit_side(Builder* b, float ax, float ay, float bx, float by,
                      float bottom, float top, float nx, float nz, const float rgb[3]) {
    float light = world_light(nx, 0.0f, nz);
    float foot  = (bottom <= 0.0f) ? light * STRUCTURE_WALL_FOOT_SHADE : light;
    P3 a0 = { ax, ay, bottom }, b0 = { bx, by, bottom };
    P3 a1 = { ax, ay, top    }, b1 = { bx, by, top    };
    tri3(b, a0, b0, b1, rgb, foot, foot, light);
    tri3(b, a0, b1, a1, rgb, foot, light, light);
}

/** Walls on each edge of tile (tx, ty) where the neighbour is lower: from the
 *  ground where it is not the same kind, from its top where it is a lower
 *  block. `neighbour_top` answers the second. */
static void emit_sides(Builder* b, const StructureMeshInput* in, int tx, int ty,
                       StructureKind k, float height, const float rgb[3]) {
    float s = (float)in->tile_size;
    float x0 = (float)tx * s, y0 = (float)ty * s, x1 = x0 + s, y1 = y0 + s;
    static const int DIRS[4][2] = { { 0, -1 }, { 0, 1 }, { -1, 0 }, { 1, 0 } };
    for (int d = 0; d < 4; d++) {
        StructureTile n = tile(in, tx + DIRS[d][0], ty + DIRS[d][1]);
        float bottom = 0.0f;
        if (n.kind == k) {
            if (k != STRUCTURE_BLOCK) continue;
            bottom = block_height(n.style);
            if (bottom >= height) continue;
        }
        switch (d) {
            case 0: emit_side(b, x0, y0, x1, y0, bottom, height,  0.0f, -1.0f, rgb); break;
            case 1: emit_side(b, x1, y1, x0, y1, bottom, height,  0.0f,  1.0f, rgb); break;
            case 2: emit_side(b, x0, y1, x0, y0, bottom, height, -1.0f,  0.0f, rgb); break;
            default: emit_side(b, x1, y0, x1, y1, bottom, height, 1.0f,  0.0f, rgb); break;
        }
    }
}

/* ---- props ------------------------------------------------------------- */

/**
 * A ring-to-ring band around a vertical axis: a cylinder, a cone, a pyramid
 * (4 sides) or a box (4 sides, turned 45 degrees). `r1` of 0 is a point.
 * `cap` closes the top.
 */
static void emit_frustum(Builder* b, float cx, float cy, float h0, float h1,
                         float r0, float r1, int sides, float turn, int cap,
                         const float rgb[3]) {
    P3 axis = { cx, cy, (h0 + h1) * 0.5f };
    P3 top  = { cx, cy, h1 };
    for (int i = 0; i < sides; i++) {
        float a0 = turn + 2.0f * PI_F * (float)i / (float)sides;
        float a1 = turn + 2.0f * PI_F * (float)(i + 1) / (float)sides;
        P3 b0 = { cx + cosf(a0) * r0, cy + sinf(a0) * r0, h0 };
        P3 b1 = { cx + cosf(a1) * r0, cy + sinf(a1) * r0, h0 };
        P3 t0 = { cx + cosf(a0) * r1, cy + sinf(a0) * r1, h1 };
        P3 t1 = { cx + cosf(a1) * r1, cy + sinf(a1) * r1, h1 };
        lit_tri(b, b0, b1, t1, rgb, 0, axis);
        if (r1 > 0.0f) lit_tri(b, b0, t1, t0, rgb, 0, axis);
        if (cap && r1 > 0.0f) lit_tri(b, top, t0, t1, rgb, 1, axis);
    }
}

/** A box from a square half-size, as a frustum turned to be axis-aligned. */
static void emit_box(Builder* b, float cx, float cy, float h0, float h1, float half,
                     const float rgb[3]) {
    float r = half * 1.41421356f;
    emit_frustum(b, cx, cy, h0, h1, r, r, 4, PI_F / 4.0f, 1, rgb);
}

static void scale_rgb(const float in[3], float k, float out[3]) {
    out[0] = in[0] * k; out[1] = in[1] * k; out[2] = in[2] * k;
}

/** A stone column: plinth, base drum, shaft and capital. */
static void emit_column(Builder* b, float cx, float cy, float size) {
    const float* stone = TILE_PALETTE[PAL_COLUMN];
    float plinth[3];
    scale_rgb(stone, 0.85f, plinth);
    emit_box(b, cx, cy, 0.0f, 6.0f, size * 0.46f, plinth);
    emit_frustum(b, cx, cy, 6.0f, 10.0f, size * 0.34f, size * 0.30f, 8, 0.0f, 1, stone);
    emit_frustum(b, cx, cy, 10.0f, 74.0f, size * 0.25f, size * 0.21f, 8, 0.0f, 0, stone);
    emit_box(b, cx, cy, 74.0f, 80.0f, size * 0.36f, plinth);
}

/** A lamp post with a lantern that glows rather than takes the sun. */
static void emit_lamp(Builder* b, float cx, float cy, float size) {
    const float* iron = TILE_PALETTE[PAL_LAMP_POST];
    static const float LANTERN[3] = { 1.0f, 0.82f, 0.42f };
    emit_box(b, cx, cy, 0.0f, 4.0f, size * 0.32f, iron);
    emit_frustum(b, cx, cy, 4.0f, 42.0f, 1.8f, 1.4f, 6, 0.0f, 0, iron);

    /* Lantern: a box whose every face is lit from within. */
    int start = b->n;
    emit_box(b, cx, cy, 42.0f, 52.0f, 4.5f, LANTERN);
    for (int i = start; i < b->n; i++) {
        b->v[i].r = to_byte(LANTERN[0] * STRUCTURE_LANTERN_GLOW);
        b->v[i].g = to_byte(LANTERN[1] * STRUCTURE_LANTERN_GLOW);
        b->v[i].b = to_byte(LANTERN[2] * STRUCTURE_LANTERN_GLOW);
    }
    emit_frustum(b, cx, cy, 52.0f, 58.0f, 7.0f, 0.0f, 4, PI_F / 4.0f, 0, iron);
}

/** A tree in a stone planter: trunk and three stacked cones of leaves. */
static void emit_tree(Builder* b, float cx, float cy, float size, int seed_x, int seed_y) {
    static const float BARK[3] = { 0.40f, 0.28f, 0.18f };
    const float* leaves = TILE_PALETTE[PAL_TREE];
    const float* stone  = TILE_PALETTE[PAL_COURTYARD_WALL];

    /* Each tree a little different, so a grove is not a row of copies. */
    float grow = 0.9f + 0.25f * hash01(seed_x, seed_y);
    float turn = hash01(seed_y, seed_x) * PI_F;

    emit_box(b, cx, cy, 0.0f, 7.0f, size * 0.44f, stone);
    emit_frustum(b, cx, cy, 7.0f, 30.0f * grow, 3.2f, 2.2f, 6, turn, 0, BARK);

    float shade[3];
    scale_rgb(leaves, 0.9f, shade);
    emit_frustum(b, cx, cy, 24.0f * grow, 54.0f * grow, 20.0f * grow, 0.0f, 7, turn, 0, shade);
    emit_frustum(b, cx, cy, 40.0f * grow, 68.0f * grow, 16.0f * grow, 0.0f, 7, turn + 0.4f, 0, leaves);
    scale_rgb(leaves, 1.15f, shade);
    emit_frustum(b, cx, cy, 56.0f * grow, 82.0f * grow, 11.0f * grow, 0.0f, 7, turn + 0.8f, 0, shade);
}

/**
 * Emit the prop whose footprint starts at (tx, ty), if one does: a tile is a
 * footprint's start when neither its west nor its north neighbour is the same
 * prop. The footprint's extent comes from the tiles, so the prop is centred on
 * what the server blocks.
 */
static void emit_prop(Builder* b, const StructureMeshInput* in, int tx, int ty, StructureKind k) {
    if (kind(in, tx - 1, ty) == k || kind(in, tx, ty - 1) == k) return;
    int w = run_length(in, tx, ty, 1, 0, k, STRUCTURE_PROP_MAX_TILES);
    int h = run_length(in, tx, ty, 0, 1, k, STRUCTURE_PROP_MAX_TILES);
    float s  = (float)in->tile_size;
    float cx = ((float)tx + (float)w * 0.5f) * s;
    float cy = ((float)ty + (float)h * 0.5f) * s;
    float size = (float)(w < h ? w : h) * s;

    switch (k) {
        case STRUCTURE_COLUMN: emit_column(b, cx, cy, size); break;
        case STRUCTURE_LAMP:   emit_lamp(b, cx, cy, size); break;
        case STRUCTURE_TREE:   emit_tree(b, cx, cy, size, tx, ty); break;
        default: break;
    }
}

/* ---- the chunk --------------------------------------------------------- */

int structure_mesh_build(ChunkVertex* out, const StructureMeshInput* in) {
    const float* roof = TILE_PALETTE[PAL_BUILDING_ROOF];
    const float* wall = TILE_PALETTE[PAL_BUILDING_WALL];
    const float* rock = TILE_PALETTE[PAL_MOUNTAIN_ROCK];
    const float s = (float)in->tile_size;

    Builder b = { out, 0, STRUCTURE_MESH_MAX_VERTS };
    int tx0 = in->chunk_x * CHUNK_SIZE, ty0 = in->chunk_y * CHUNK_SIZE;
    for (int ty = ty0; ty < ty0 + CHUNK_SIZE && ty < in->world_h; ty++) {
        for (int tx = tx0; tx < tx0 + CHUNK_SIZE && tx < in->world_w; tx++) {
            StructureTile t = tile(in, tx, ty);
            float x = (float)tx * s, y = (float)ty * s;
            switch (t.kind) {
                case STRUCTURE_BUILDING:
                    emit_top(&b, x, y, s,
                             structure_roof_height(in, tx,     ty),
                             structure_roof_height(in, tx + 1, ty),
                             structure_roof_height(in, tx + 1, ty + 1),
                             structure_roof_height(in, tx,     ty + 1), roof);
                    emit_sides(&b, in, tx, ty, t.kind, STRUCTURE_BUILDING_EAVE, wall);
                    break;
                case STRUCTURE_BLOCK: {
                    const float h = block_height(t.style);
                    const float* rgb = TILE_PALETTE[t.style < PAL_COUNT ? t.style : PAL_CITY_WALL];
                    emit_top(&b, x, y, s, h, h, h, h, rgb);
                    emit_sides(&b, in, tx, ty, t.kind, h, rgb);
                    break;
                }
                case STRUCTURE_ROCK:
                    emit_top(&b, x, y, s,
                             rock_height(in, tx,     ty),
                             rock_height(in, tx + 1, ty),
                             rock_height(in, tx + 1, ty + 1),
                             rock_height(in, tx,     ty + 1), rock);
                    break;
                case STRUCTURE_COLUMN:
                case STRUCTURE_LAMP:
                case STRUCTURE_TREE:
                    emit_prop(&b, in, tx, ty, t.kind);
                    break;
                default:
                    break;
            }
        }
    }
    return b.n;
}
