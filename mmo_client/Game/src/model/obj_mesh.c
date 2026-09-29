/**
 * @file
 * Parse a static Wavefront OBJ into one indexed, cell-sorted mesh, and keep a
 * binary copy of the result so a large file is only parsed once.
 */

#include "model/obj_mesh.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NO_INDEX      UINT32_MAX
#define MAX_CORNERS   256            /* Largest polygon accepted. */
#define READ_CHUNK    (4u << 20)     /* Longest line accepted, too. */
#define MAX_CELLS     (1u << 20)

static void set_err(char* err, int err_len, const char* fmt, ...) {
    if (!err || err_len <= 0) return;
    va_list args;
    va_start(args, fmt);
    vsnprintf(err, (size_t)err_len, fmt, args);
    va_end(args);
}

/** Make room for `need` elements, doubling. */
static int grow(void** data, size_t* cap, size_t need, size_t elem) {
    if (need <= *cap) return 1;
    size_t n = *cap ? *cap : 4096;
    while (n < need) n *= 2;
    void* p = realloc(*data, n * elem);
    if (!p) return 0;
    *data = p;
    *cap = n;
    return 1;
}

/* ------------------------------------------------------------------------ */
/* Number parsing. strtof is correct but slow on a 450 MB file; Blender writes
 * plain "%f", so a direct decimal parse covers it, with strtof kept for
 * anything unusual (exponents, inf). */

static const char* skip_space(const char* s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

static const char* parse_float(const char* s, float* out) {
    s = skip_space(s);
    const char* start = s;
    int neg = 0;
    if (*s == '-' || *s == '+') { neg = (*s == '-'); s++; }

    uint64_t mant = 0;
    int digits = 0, frac = 0, any = 0;
    while (*s >= '0' && *s <= '9') {
        if (digits < 18) { mant = mant * 10 + (uint64_t)(*s - '0'); digits++; }
        else frac--;                    /* Integer digits past precision. */
        any = 1;
        s++;
    }
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9') {
            if (digits < 18) { mant = mant * 10 + (uint64_t)(*s - '0'); digits++; frac++; }
            any = 1;
            s++;
        }
    }
    if (!any || *s == 'e' || *s == 'E') {
        char* end = NULL;
        float f = strtof(start, &end);
        if (end == start) return NULL;
        *out = f;
        return end;
    }

    static const double POW10[] = {
        1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9,
        1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18
    };
    double v = (double)mant;
    if (frac > 0) v /= (frac <= 18) ? POW10[frac] : pow(10.0, frac);
    else if (frac < 0) v *= pow(10.0, -frac);
    *out = (float)(neg ? -v : v);
    return s;
}

static const char* parse_long(const char* s, long* out) {
    int neg = 0;
    if (*s == '-' || *s == '+') { neg = (*s == '-'); s++; }
    if (*s < '0' || *s > '9') return NULL;
    long v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        if (v > 0x7fffffffL) return NULL;
        s++;
    }
    *out = neg ? -v : v;
    return s;
}

/* ------------------------------------------------------------------------ */

typedef struct { uint32_t a, b, c; int32_t material; } Tri;

/** One (position, uv, normal) corner already turned into a vertex, within
 *  the current object. Slots from an earlier object have an older gen. */
typedef struct { uint32_t v, t, n, out, gen; } Slot;

typedef struct {
    float*     pos;   size_t pos_n,  pos_cap;    /* in floats */
    float*     uv;    size_t uv_n,   uv_cap;
    float*     nrm;   size_t nrm_n,  nrm_cap;
    ObjVertex* verts; size_t vert_n, vert_cap;
    Tri*       tris;  size_t tri_n,  tri_cap;

    Slot*    table;
    uint32_t table_cap, table_used, gen;

    int32_t  material;
    ObjMesh* mesh;
    char     dir[OBJ_PATH_LEN];
    long     line;
    char*    err;
    int      err_len;
} Parser;

static uint32_t slot_hash(uint32_t v, uint32_t t, uint32_t n) {
    uint32_t h = v * 0x9E3779B1u ^ (t + 0x7F4A7C15u) * 0x85EBCA77u ^ (n + 0x165667B1u) * 0xC2B2AE3Du;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    return h;
}

static int table_resize(Parser* p, uint32_t cap) {
    Slot* old = p->table;
    uint32_t old_cap = p->table_cap;
    Slot* table = calloc(cap, sizeof(Slot));
    if (!table) return 0;
    for (uint32_t i = 0; i < old_cap; i++) {
        if (old[i].gen != p->gen) continue;
        uint32_t h = slot_hash(old[i].v, old[i].t, old[i].n) & (cap - 1);
        while (table[h].gen == p->gen) h = (h + 1) & (cap - 1);
        table[h] = old[i];
    }
    free(old);
    p->table = table;
    p->table_cap = cap;
    return 1;
}

/** Vertices are shared only within one object: Blender never shares them
 *  across objects, and a table that forgets at each "o" stays small. */
static void new_object(Parser* p) {
    p->gen++;
    if (p->gen == 0) {             /* Wrapped: stale slots would look current. */
        memset(p->table, 0, (size_t)p->table_cap * sizeof(Slot));
        p->gen = 1;
    }
    p->table_used = 0;
}

static void pack_normal(ObjVertex* out, float nx, float ny, float nz) {
    float len = sqrtf(nx * nx + ny * ny + nz * nz);
    if (len < 1e-12f) { nx = 0.0f; ny = 1.0f; nz = 0.0f; len = 1.0f; }
    out->nx = (int8_t)lrintf(nx / len * 127.0f);
    out->ny = (int8_t)lrintf(ny / len * 127.0f);
    out->nz = (int8_t)lrintf(nz / len * 127.0f);
    out->pad = 0;
}

/** Append a vertex for a corner. face_n is used when the corner has no vn. */
static int emit_vertex(Parser* p, uint32_t v, uint32_t t, uint32_t n,
                       const float face_n[3], uint32_t* out) {
    if (!grow((void**)&p->verts, &p->vert_cap, p->vert_n + 1, sizeof(ObjVertex))) return 0;
    ObjVertex* o = &p->verts[p->vert_n];
    o->x = p->pos[v * 3 + 0];
    o->y = p->pos[v * 3 + 1];
    o->z = p->pos[v * 3 + 2];
    if (n != NO_INDEX) pack_normal(o, p->nrm[n * 3 + 0], p->nrm[n * 3 + 1], p->nrm[n * 3 + 2]);
    else               pack_normal(o, face_n[0], face_n[1], face_n[2]);
    if (t != NO_INDEX) {
        o->u = p->uv[t * 2 + 0];
        o->v = 1.0f - p->uv[t * 2 + 1];
    } else {
        o->u = o->v = 0.0f;
    }
    *out = (uint32_t)p->vert_n++;
    return 1;
}

static int corner_vertex(Parser* p, uint32_t v, uint32_t t, uint32_t n,
                         const float face_n[3], uint32_t* out) {
    /* Without its own normal a corner takes its face's, so it is not the same
     * vertex as that corner on a neighbouring face. */
    if (n == NO_INDEX) return emit_vertex(p, v, t, n, face_n, out);

    if ((p->table_used + 1) * 2 > p->table_cap &&
        !table_resize(p, p->table_cap ? p->table_cap * 2 : 1024))
        return 0;

    uint32_t mask = p->table_cap - 1;
    uint32_t h = slot_hash(v, t, n) & mask;
    while (p->table[h].gen == p->gen) {
        Slot* s = &p->table[h];
        if (s->v == v && s->t == t && s->n == n) { *out = s->out; return 1; }
        h = (h + 1) & mask;
    }
    if (!emit_vertex(p, v, t, n, face_n, out)) return 0;
    p->table[h] = (Slot){ v, t, n, *out, p->gen };
    p->table_used++;
    return 1;
}

/** OBJ indices are 1-based, or negative counting back from the newest. */
static int resolve_index(long raw, size_t count, uint32_t* out) {
    long i = raw > 0 ? raw - 1 : (long)count + raw;
    if (raw == 0 || i < 0 || (size_t)i >= count) return 0;
    *out = (uint32_t)i;
    return 1;
}

static int parse_face(Parser* p, const char* s) {
    uint32_t cv[MAX_CORNERS], ct[MAX_CORNERS], cn[MAX_CORNERS];
    int count = 0, any_missing_normal = 0;

    for (;;) {
        s = skip_space(s);
        if (*s == '\0' || *s == '\r' || *s == '\n' || *s == '#') break;
        if (count == MAX_CORNERS) {
            set_err(p->err, p->err_len, "line %ld: polygon has more than %d corners",
                    p->line, MAX_CORNERS);
            return 0;
        }
        long raw;
        const char* e = parse_long(s, &raw);
        if (!e || !resolve_index(raw, p->pos_n / 3, &cv[count])) {
            set_err(p->err, p->err_len, "line %ld: bad vertex index", p->line);
            return 0;
        }
        s = e;
        ct[count] = cn[count] = NO_INDEX;
        if (*s == '/') {
            s++;
            if (*s != '/') {
                e = parse_long(s, &raw);
                if (!e || !resolve_index(raw, p->uv_n / 2, &ct[count])) {
                    set_err(p->err, p->err_len, "line %ld: bad texture index", p->line);
                    return 0;
                }
                s = e;
            }
            if (*s == '/') {
                s++;
                e = parse_long(s, &raw);
                if (!e || !resolve_index(raw, p->nrm_n / 3, &cn[count])) {
                    set_err(p->err, p->err_len, "line %ld: bad normal index", p->line);
                    return 0;
                }
                s = e;
            }
        }
        if (cn[count] == NO_INDEX) any_missing_normal = 1;
        count++;
    }
    if (count < 3) return 1;       /* Points and lines draw nothing. */

    /* Newell's normal: right for any planar polygon, fine for a nearly
     * planar one, and only needed when the file gave no vn. */
    float fn[3] = { 0.0f, 1.0f, 0.0f };
    if (any_missing_normal) {
        fn[0] = fn[1] = fn[2] = 0.0f;
        for (int i = 0; i < count; i++) {
            const float* a = &p->pos[cv[i] * 3];
            const float* b = &p->pos[cv[(i + 1) % count] * 3];
            fn[0] += (a[1] - b[1]) * (a[2] + b[2]);
            fn[1] += (a[2] - b[2]) * (a[0] + b[0]);
            fn[2] += (a[0] - b[0]) * (a[1] + b[1]);
        }
    }

    uint32_t out[MAX_CORNERS];
    for (int i = 0; i < count; i++)
        if (!corner_vertex(p, cv[i], ct[i], cn[i], fn, &out[i])) {
            set_err(p->err, p->err_len, "out of memory");
            return 0;
        }

    if (!grow((void**)&p->tris, &p->tri_cap, p->tri_n + (size_t)(count - 2), sizeof(Tri))) {
        set_err(p->err, p->err_len, "out of memory");
        return 0;
    }
    for (int i = 1; i + 1 < count; i++) {
        if (out[0] == out[i] || out[i] == out[i + 1] || out[0] == out[i + 1]) continue;
        p->tris[p->tri_n++] = (Tri){ out[0], out[i], out[i + 1], p->material };
    }
    return 1;
}

static int parse_floats(Parser* p, const char* s, int n, float** arr, size_t* len, size_t* cap) {
    if (!grow((void**)arr, cap, *len + (size_t)n, sizeof(float))) {
        set_err(p->err, p->err_len, "out of memory");
        return 0;
    }
    for (int i = 0; i < n; i++) {
        float f;
        const char* e = parse_float(s, &f);
        if (!e) {
            set_err(p->err, p->err_len, "line %ld: expected %d numbers", p->line, n);
            return 0;
        }
        (*arr)[*len + (size_t)i] = f;
        s = e;
    }
    *len += (size_t)n;
    return 1;
}

/** Copy the rest of a line, trimmed, into out. */
static void rest_of_line(const char* s, char* out, size_t out_len) {
    s = skip_space(s);
    size_t n = strcspn(s, "\r\n");
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
    if (n >= out_len) n = out_len - 1;
    memcpy(out, s, n);
    out[n] = '\0';
}

static int find_material(ObjMesh* m, const char* name) {
    for (int i = 0; i < m->material_count; i++)
        if (strcmp(m->materials[i].name, name) == 0) return i;
    return -1;
}

static int add_material(Parser* p, const char* name) {
    ObjMesh* m = p->mesh;
    int i = find_material(m, name);
    if (i >= 0) return i;
    if (m->material_count == OBJ_MAX_MATERIALS) {
        set_err(p->err, p->err_len, "more than %d materials", OBJ_MAX_MATERIALS);
        return -1;
    }
    ObjMaterial* mat = &m->materials[m->material_count];
    memset(mat, 0, sizeof(*mat));
    snprintf(mat->name, sizeof(mat->name), "%s", name);
    mat->diffuse[0] = mat->diffuse[1] = mat->diffuse[2] = 1.0f;
    return m->material_count++;
}

static void join_path(char* out, size_t out_len, const char* dir, const char* name) {
    int absolute = name[0] == '/' || name[0] == '\\' || (name[0] && name[1] == ':');
    if (absolute || dir[0] == '\0') snprintf(out, out_len, "%s", name);
    else                            snprintf(out, out_len, "%s/%s", dir, name);
}

/** Read a .mtl. A missing one is not fatal: its materials draw untextured. */
static int parse_mtl(Parser* p, const char* name) {
    char path[OBJ_PATH_LEN];
    join_path(path, sizeof(path), p->dir, name);
    FILE* f = fopen(path, "rb");
    if (!f) return 1;

    char line[1024];
    int current = -1;
    while (fgets(line, sizeof(line), f)) {
        const char* s = skip_space(line);
        char value[OBJ_PATH_LEN];
        if (strncmp(s, "newmtl", 6) == 0 && (s[6] == ' ' || s[6] == '\t')) {
            rest_of_line(s + 6, value, sizeof(value));
            current = add_material(p, value);
            if (current < 0) { fclose(f); return 0; }
        } else if (current >= 0 && strncmp(s, "map_Kd", 6) == 0 &&
                   (s[6] == ' ' || s[6] == '\t')) {
            /* Options (-s, -o ...) come first; the file name is last. */
            rest_of_line(s + 6, value, sizeof(value));
            const char* file = strrchr(value, ' ');
            file = file ? file + 1 : value;
            join_path(p->mesh->materials[current].texture,
                      sizeof(p->mesh->materials[current].texture), p->dir, file);
        } else if (current >= 0 && s[0] == 'K' && s[1] == 'd' && (s[2] == ' ' || s[2] == '\t')) {
            float* kd = p->mesh->materials[current].diffuse;
            const char* e = s + 2;
            for (int i = 0; i < 3 && e; i++) e = parse_float(e, &kd[i]);
        }
    }
    fclose(f);
    return 1;
}

static int parse_line(Parser* p, const char* s) {
    s = skip_space(s);
    char value[OBJ_PATH_LEN];
    switch (s[0]) {
    case 'v':
        if (s[1] == ' ' || s[1] == '\t')
            return parse_floats(p, s + 1, 3, &p->pos, &p->pos_n, &p->pos_cap);
        if (s[1] == 't' && (s[2] == ' ' || s[2] == '\t'))
            return parse_floats(p, s + 2, 2, &p->uv, &p->uv_n, &p->uv_cap);
        if (s[1] == 'n' && (s[2] == ' ' || s[2] == '\t'))
            return parse_floats(p, s + 2, 3, &p->nrm, &p->nrm_n, &p->nrm_cap);
        return 1;
    case 'f':
        if (s[1] == ' ' || s[1] == '\t') {
            if (p->material < 0 && (p->material = add_material(p, "default")) < 0) return 0;
            return parse_face(p, s + 1);
        }
        return 1;
    case 'o':
    case 'g':
        if (s[1] == ' ' || s[1] == '\t' || s[1] == '\r' || s[1] == '\n' || s[1] == '\0')
            new_object(p);
        return 1;
    case 'u':
        if (strncmp(s, "usemtl", 6) == 0 && (s[6] == ' ' || s[6] == '\t')) {
            rest_of_line(s + 6, value, sizeof(value));
            p->material = add_material(p, value);
            return p->material >= 0;
        }
        return 1;
    case 'm':
        if (strncmp(s, "mtllib", 6) == 0 && (s[6] == ' ' || s[6] == '\t')) {
            rest_of_line(s + 6, value, sizeof(value));
            return parse_mtl(p, value);
        }
        return 1;
    default:
        return 1;                  /* Comments, smoothing groups, lines. */
    }
}

/** Feed the file through parse_line() one line at a time. */
static int parse_file(Parser* p, FILE* f) {
    char* buf = malloc(READ_CHUNK + 1);
    if (!buf) { set_err(p->err, p->err_len, "out of memory"); return 0; }

    size_t have = 0;
    int ok = 1, eof = 0;
    while (ok && !eof) {
        size_t got = fread(buf + have, 1, READ_CHUNK - have, f);
        have += got;
        eof = (got == 0) || feof(f);
        buf[have] = '\0';

        char* line = buf;
        for (;;) {
            char* nl = memchr(line, '\n', have - (size_t)(line - buf));
            if (!nl) break;
            *nl = '\0';
            p->line++;
            if (!parse_line(p, line)) { ok = 0; break; }
            line = nl + 1;
        }
        if (!ok) break;

        size_t left = have - (size_t)(line - buf);
        if (eof) {
            if (left > 0) { p->line++; ok = parse_line(p, line); }
            break;
        }
        if (left == READ_CHUNK) {
            set_err(p->err, p->err_len, "line %ld is too long", p->line + 1);
            ok = 0;
            break;
        }
        memmove(buf, line, left);
        have = left;
    }
    if (ok && ferror(f)) { set_err(p->err, p->err_len, "read error"); ok = 0; }
    free(buf);
    return ok;
}

/* ------------------------------------------------------------------------ */

/** Sort the triangles by (cell, material) into the mesh's index buffer and
 *  cut it into batches. */
static int build_batches(Parser* p, float cell_size) {
    ObjMesh* m = p->mesh;
    for (int a = 0; a < 3; a++) { m->min[a] = 0.0f; m->max[a] = 0.0f; }
    for (size_t i = 0; i < p->vert_n; i++) {
        const float v[3] = { p->verts[i].x, p->verts[i].y, p->verts[i].z };
        for (int a = 0; a < 3; a++) {
            if (i == 0 || v[a] < m->min[a]) m->min[a] = v[a];
            if (i == 0 || v[a] > m->max[a]) m->max[a] = v[a];
        }
    }
    if (p->tri_n == 0) return 1;
    if (p->tri_n * 3 > UINT32_MAX) {
        set_err(p->err, p->err_len, "too many triangles");
        return 0;
    }

    if (!(cell_size > 0.0f)) cell_size = 1e30f;
    float size_x = m->max[0] - m->min[0], size_z = m->max[2] - m->min[2];
    uint32_t gx, gz;
    for (;;) {
        gx = (uint32_t)(size_x / cell_size) + 1;
        gz = (uint32_t)(size_z / cell_size) + 1;
        if ((uint64_t)gx * gz <= MAX_CELLS) break;
        cell_size *= 2.0f;
    }

    size_t mats = (size_t)(m->material_count > 0 ? m->material_count : 1);
    size_t buckets = (size_t)gx * gz * mats;
    uint32_t* start = calloc(buckets + 1, sizeof(uint32_t));
    uint32_t* bucket_of = malloc(p->tri_n * sizeof(uint32_t));
    m->indices = malloc(p->tri_n * 3 * sizeof(uint32_t));
    if (!start || !bucket_of || !m->indices) {
        free(start);
        free(bucket_of);
        set_err(p->err, p->err_len, "out of memory");
        return 0;
    }

    for (size_t i = 0; i < p->tri_n; i++) {
        const Tri* t = &p->tris[i];
        const ObjVertex *a = &p->verts[t->a], *b = &p->verts[t->b], *c = &p->verts[t->c];
        float cx = (a->x + b->x + c->x) / 3.0f, cz = (a->z + b->z + c->z) / 3.0f;
        uint32_t ix = (uint32_t)((cx - m->min[0]) / cell_size);
        uint32_t iz = (uint32_t)((cz - m->min[2]) / cell_size);
        if (ix >= gx) ix = gx - 1;
        if (iz >= gz) iz = gz - 1;
        bucket_of[i] = (uint32_t)(((size_t)iz * gx + ix) * mats + (size_t)t->material);
        start[bucket_of[i] + 1]++;
    }

    size_t used = 0;
    for (size_t b = 0; b < buckets; b++) {
        if (start[b + 1]) used++;
        start[b + 1] += start[b];
    }

    m->batches = malloc((used ? used : 1) * sizeof(ObjBatch));
    if (!m->batches) {
        free(start);
        free(bucket_of);
        set_err(p->err, p->err_len, "out of memory");
        return 0;
    }
    for (size_t b = 0; b < buckets; b++) {
        if (start[b + 1] == start[b]) continue;
        ObjBatch* batch = &m->batches[m->batch_count++];
        batch->first = start[b] * 3;
        batch->count = (start[b + 1] - start[b]) * 3;
        batch->material = (int32_t)(b % mats);
    }

    /* Scatter: start[] becomes each bucket's write cursor. */
    for (size_t i = 0; i < p->tri_n; i++) {
        uint32_t at = start[bucket_of[i]]++ * 3;
        m->indices[at + 0] = p->tris[i].a;
        m->indices[at + 1] = p->tris[i].b;
        m->indices[at + 2] = p->tris[i].c;
    }
    m->index_count = (uint32_t)(p->tri_n * 3);
    free(start);
    free(bucket_of);

    for (int32_t b = 0; b < m->batch_count; b++) {
        ObjBatch* batch = &m->batches[b];
        for (uint32_t i = 0; i < batch->count; i++) {
            const ObjVertex* v = &p->verts[m->indices[batch->first + i]];
            const float xyz[3] = { v->x, v->y, v->z };
            for (int a = 0; a < 3; a++) {
                if (i == 0 || xyz[a] < batch->min[a]) batch->min[a] = xyz[a];
                if (i == 0 || xyz[a] > batch->max[a]) batch->max[a] = xyz[a];
            }
        }
    }
    return 1;
}

static void parser_free(Parser* p) {
    free(p->pos);
    free(p->uv);
    free(p->nrm);
    free(p->verts);
    free(p->tris);
    free(p->table);
}

int obj_mesh_load(ObjMesh* out, const char* path, float cell_size, char* err, int err_len) {
    memset(out, 0, sizeof(*out));
    FILE* f = fopen(path, "rb");
    if (!f) {
        set_err(err, err_len, "cannot open %s", path);
        return 0;
    }

    Parser p;
    memset(&p, 0, sizeof(p));
    p.mesh = out;
    p.material = -1;
    p.err = err;
    p.err_len = err_len;
    p.gen = 1;
    snprintf(p.dir, sizeof(p.dir), "%s", path);
    char* slash = strrchr(p.dir, '/');
    char* bslash = strrchr(p.dir, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
    if (slash) *slash = '\0';
    else       p.dir[0] = '\0';

    int ok = parse_file(&p, f);
    fclose(f);
    if (ok) ok = build_batches(&p, cell_size);

    if (ok) {
        out->vertex_count = (uint32_t)p.vert_n;
        out->vertices = p.verts;       /* Handed over, not copied. */
        p.verts = NULL;
    } else {
        obj_mesh_free(out);
    }
    parser_free(&p);
    return ok;
}

void obj_mesh_free(ObjMesh* mesh) {
    free(mesh->vertices);
    free(mesh->indices);
    free(mesh->batches);
    memset(mesh, 0, sizeof(*mesh));
}

/* ------------------------------------------------------------------------ */
/* The cache: the finished mesh, byte for byte, behind a header that names the
 * OBJ it came from. The OBJ is recognised by its size and a hash of its first
 * and last megabyte, which any re-export changes, without a platform stat(). */

#define CACHE_MAGIC   "MMOOBJC1"
#define CACHE_VERSION 1u
#define FINGERPRINT_SPAN (1L << 20)

typedef struct {
    char     magic[8];
    uint32_t version;
    uint32_t vertex_size;
    uint64_t source_size;
    uint64_t source_hash;
    float    cell_size;
    uint32_t vertex_count;
    uint32_t index_count;
    int32_t  batch_count;
    int32_t  material_count;
    float    min[3], max[3];
} CacheHeader;

static uint64_t fnv1a(uint64_t h, const unsigned char* data, size_t n) {
    for (size_t i = 0; i < n; i++) {
        h ^= data[i];
        h *= 0x100000001B3ull;
    }
    return h;
}

/** Size and head/tail hash of a file. 0 when it cannot be read. */
static int fingerprint(const char* path, uint64_t* size, uint64_t* hash) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char* buf = malloc((size_t)FINGERPRINT_SPAN);
    long end = -1;
    if (buf && fseek(f, 0, SEEK_END) == 0) end = ftell(f);
    if (end < 0) { free(buf); fclose(f); return 0; }

    uint64_t h = 0xCBF29CE484222325ull;
    rewind(f);
    size_t n = fread(buf, 1, (size_t)FINGERPRINT_SPAN, f);
    h = fnv1a(h, buf, n);
    if (end > FINGERPRINT_SPAN && fseek(f, -FINGERPRINT_SPAN, SEEK_END) == 0) {
        n = fread(buf, 1, (size_t)FINGERPRINT_SPAN, f);
        h = fnv1a(h, buf, n);
    }
    free(buf);
    fclose(f);
    *size = (uint64_t)end;
    *hash = h;
    return 1;
}

static int read_cache(ObjMesh* out, const char* path, const CacheHeader* want) {
    memset(out, 0, sizeof(*out));
    FILE* f = fopen(path, "rb");
    if (!f) return 0;

    CacheHeader h;
    int ok = fread(&h, sizeof(h), 1, f) == 1 &&
             memcmp(h.magic, CACHE_MAGIC, 8) == 0 &&
             h.version == CACHE_VERSION &&
             h.vertex_size == sizeof(ObjVertex) &&
             h.source_size == want->source_size &&
             h.source_hash == want->source_hash &&
             h.cell_size == want->cell_size &&
             h.material_count >= 0 && h.material_count <= OBJ_MAX_MATERIALS &&
             h.batch_count >= 0 && h.index_count % 3 == 0;
    if (ok) {
        out->vertex_count   = h.vertex_count;
        out->index_count    = h.index_count;
        out->batch_count    = h.batch_count;
        out->material_count = h.material_count;
        memcpy(out->min, h.min, sizeof(out->min));
        memcpy(out->max, h.max, sizeof(out->max));
        out->vertices = malloc((size_t)h.vertex_count * sizeof(ObjVertex) + 1);
        out->indices  = malloc((size_t)h.index_count * sizeof(uint32_t) + 1);
        out->batches  = malloc((size_t)h.batch_count * sizeof(ObjBatch) + 1);
        ok = out->vertices && out->indices && out->batches &&
             fread(out->materials, sizeof(ObjMaterial), (size_t)h.material_count, f) ==
                 (size_t)h.material_count &&
             fread(out->batches, sizeof(ObjBatch), (size_t)h.batch_count, f) ==
                 (size_t)h.batch_count &&
             fread(out->vertices, sizeof(ObjVertex), h.vertex_count, f) == h.vertex_count &&
             fread(out->indices, sizeof(uint32_t), h.index_count, f) == h.index_count &&
             fgetc(f) == EOF;
    }
    fclose(f);

    /* A damaged cache must not reach the GPU as out-of-range indices. */
    for (int32_t b = 0; ok && b < out->batch_count; b++) {
        const ObjBatch* batch = &out->batches[b];
        ok = batch->material >= 0 && batch->material < out->material_count &&
             batch->first <= out->index_count &&
             batch->count <= out->index_count - batch->first;
    }
    for (uint32_t i = 0; ok && i < out->index_count; i++)
        ok = out->indices[i] < out->vertex_count;
    for (int32_t i = 0; ok && i < out->material_count; i++) {
        out->materials[i].name[OBJ_NAME_LEN - 1] = '\0';
        out->materials[i].texture[OBJ_PATH_LEN - 1] = '\0';
    }

    if (!ok) obj_mesh_free(out);
    return ok;
}

static int write_cache(const ObjMesh* m, const char* path, const CacheHeader* base) {
    char tmp[OBJ_PATH_LEN + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE* f = fopen(tmp, "wb");
    if (!f) return 0;

    CacheHeader h = *base;
    h.vertex_count   = m->vertex_count;
    h.index_count    = m->index_count;
    h.batch_count    = m->batch_count;
    h.material_count = m->material_count;
    memcpy(h.min, m->min, sizeof(h.min));
    memcpy(h.max, m->max, sizeof(h.max));

    int ok = fwrite(&h, sizeof(h), 1, f) == 1 &&
             fwrite(m->materials, sizeof(ObjMaterial), (size_t)m->material_count, f) ==
                 (size_t)m->material_count &&
             fwrite(m->batches, sizeof(ObjBatch), (size_t)m->batch_count, f) ==
                 (size_t)m->batch_count &&
             fwrite(m->vertices, sizeof(ObjVertex), m->vertex_count, f) == m->vertex_count &&
             fwrite(m->indices, sizeof(uint32_t), m->index_count, f) == m->index_count;
    if (fclose(f) != 0) ok = 0;

    /* Windows' rename() will not replace an existing file. */
    if (ok) {
        remove(path);
        ok = rename(tmp, path) == 0;
    }
    if (!ok) remove(tmp);
    return ok;
}

int obj_mesh_load_cached(ObjMesh* out, const char* obj_path, const char* cache_path,
                         float cell_size, int* from_cache, char* err, int err_len) {
    if (from_cache) *from_cache = 0;
    CacheHeader want;
    memset(&want, 0, sizeof(want));
    memcpy(want.magic, CACHE_MAGIC, 8);
    want.version = CACHE_VERSION;
    want.vertex_size = sizeof(ObjVertex);
    want.cell_size = cell_size;

    if (!fingerprint(obj_path, &want.source_size, &want.source_hash))
        return obj_mesh_load(out, obj_path, cell_size, err, err_len);

    if (read_cache(out, cache_path, &want)) {
        if (from_cache) *from_cache = 1;
        return 1;
    }
    if (!obj_mesh_load(out, obj_path, cell_size, err, err_len)) return 0;
    write_cache(out, cache_path, &want);   /* Next start is faster; or not. */
    return 1;
}
