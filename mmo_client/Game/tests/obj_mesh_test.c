/**
 * @file
 * Check the OBJ loader behind the city scene, without OpenGL.
 *
 * Writes a small OBJ and MTL of its own, so it runs on a fresh clone where
 * the (untracked, 450 MB) city export is absent: indices of every form, fan
 * triangulation, the V flip, per-object vertex sharing, cell batching, the
 * material's texture path, and a cache that is reused, then rebuilt once the
 * OBJ changes.
 */

#include "model/obj_mesh.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond, what)                                                   \
    do {                                                                    \
        if (cond) {                                                         \
            printf("  ok   %s\n", (what));                                  \
        } else {                                                            \
            printf("  FAIL %s  (%s:%d)\n", (what), __FILE__, __LINE__);     \
            g_failures++;                                                   \
        }                                                                   \
    } while (0)

#define DIR   "Game/tests/bin"
#define OBJ   DIR "/obj_mesh_test.obj"
#define MTL   DIR "/obj_mesh_test.mtl"
#define CACHE DIR "/obj_mesh_test.objcache"

static void write_file(const char* path, const char* text) {
    FILE* f = fopen(path, "wb");
    if (!f) { printf("  FAIL cannot write %s\n", path); exit(1); }
    fputs(text, f);
    fclose(f);
}

/** Cut the last `bytes` bytes off a file. */
static void drop_tail(const char* path, size_t bytes) {
    FILE* f = fopen(path, "rb");
    if (!f) return;
    char* buf = malloc(1 << 16);
    size_t got = buf ? fread(buf, 1, 1 << 16, f) : 0;
    fclose(f);
    f = fopen(path, "wb");
    if (f && got > bytes) fwrite(buf, 1, got - bytes, f);
    if (f) fclose(f);
    free(buf);
}

/* Two objects in two cells 10 units apart (cell size 4):
 *  - "floor": a quad with v/vt/vn, in material "stone" (textured);
 *  - "roof": a pentagon with v//vn and negative indices, in "plain";
 *  plus a CRLF line, a comment and a smoothing group. */
static const char* SCENE =
    "# test scene\n"
    "mtllib obj_mesh_test.mtl\n"
    "o floor\n"
    "v 0 0 0\nv 2 0 0\nv 2 0 2\nv 0 0 2\n"
    "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n"
    "vn 0 1 0\n"
    "usemtl stone\n"
    "s off\n"
    "f 1/1/1 2/2/1 3/3/1 4/4/1\r\n"
    "o roof\n"
    "v 10 3 0\nv 12 3 0\nv 13 3 1\nv 11 3 2\nv 9 3 1\n"
    "vn 0 -1 0\n"
    "usemtl plain\n"
    "f -5//-1 -4//-1 -3//-1 -2//-1 -1//-1\n";

static const char* MATERIALS =
    "newmtl stone\n"
    "Kd 0.5 0.25 1.0\n"
    "map_Kd -s 1 1 1 stone.png\n"
    "newmtl plain\n";

static void test_parse(void) {
    printf("TEST: parse a small scene\n");
    ObjMesh m;
    char err[256] = "";
    int ok = obj_mesh_load(&m, OBJ, 4.0f, err, (int)sizeof(err));
    CHECK(ok, "loads");
    if (!ok) { printf("       %s\n", err); return; }

    CHECK(m.vertex_count == 9, "shares nothing across objects, repeats nothing within one");
    CHECK(m.index_count == (2 + 3) * 3, "quad fans to 2 triangles, pentagon to 3");
    CHECK(m.material_count == 2, "two materials");
    CHECK(strcmp(m.materials[0].texture, DIR "/stone.png") == 0,
          "texture path is the map_Kd file, beside the OBJ");
    CHECK(m.materials[1].texture[0] == '\0', "a material without map_Kd has no texture");
    CHECK(fabsf(m.materials[0].diffuse[1] - 0.25f) < 1e-6f, "Kd is read");
    CHECK(m.batch_count == 2, "one batch per occupied cell and material");
    CHECK(fabsf(m.min[1] - 0.0f) < 1e-6f && fabsf(m.max[1] - 3.0f) < 1e-6f &&
          fabsf(m.min[0] - 0.0f) < 1e-6f && fabsf(m.max[0] - 13.0f) < 1e-6f,
          "bounds cover every vertex");

    /* vt (1, 1) at vertex 3 becomes v = 0 after the flip. */
    const ObjVertex* v3 = &m.vertices[2];
    CHECK(fabsf(v3->x - 2.0f) < 1e-6f && fabsf(v3->z - 2.0f) < 1e-6f &&
          fabsf(v3->u - 1.0f) < 1e-6f && fabsf(v3->v - 0.0f) < 1e-6f,
          "positions kept, V flipped");
    CHECK(m.vertices[0].ny == 127 && m.vertices[5].ny == -127, "normals packed from vn");

    int floor_seen = 0, roof_seen = 0;
    for (int b = 0; b < m.batch_count; b++) {
        const ObjBatch* batch = &m.batches[b];
        if (batch->material == 0) {
            floor_seen = batch->count == 6 && batch->max[0] <= 2.0f;
        } else {
            roof_seen = batch->count == 9 && batch->min[0] >= 9.0f;
        }
        for (uint32_t i = 0; i < batch->count; i++) {
            const ObjVertex* v = &m.vertices[m.indices[batch->first + i]];
            if (v->x < batch->min[0] || v->x > batch->max[0]) g_failures++;
        }
    }
    CHECK(floor_seen && roof_seen, "each object's triangles land in their own cell");
    obj_mesh_free(&m);
}

static void test_errors(void) {
    printf("TEST: bad input fails cleanly\n");
    ObjMesh m;
    char err[256] = "";
    CHECK(!obj_mesh_load(&m, DIR "/no_such_file.obj", 4.0f, err, (int)sizeof(err)) &&
          m.vertices == NULL && err[0] != '\0', "missing file");

    write_file(DIR "/obj_mesh_test_bad.obj", "v 0 0 0\nv 1 0 0\nf 1 2 7\n");
    err[0] = '\0';
    CHECK(!obj_mesh_load(&m, DIR "/obj_mesh_test_bad.obj", 4.0f, err, (int)sizeof(err)) &&
          strstr(err, "line 3") != NULL, "out-of-range index names its line");
    remove(DIR "/obj_mesh_test_bad.obj");

    write_file(DIR "/obj_mesh_test_nonormal.obj",
               "v 0 0 0\nv 0 0 1\nv 1 0 0\nf 1 2 3\n");
    CHECK(obj_mesh_load(&m, DIR "/obj_mesh_test_nonormal.obj", 4.0f, err, (int)sizeof(err)) &&
          m.index_count == 3 && m.vertices[0].ny == 127,
          "a face without vn gets its face normal");
    obj_mesh_free(&m);
    remove(DIR "/obj_mesh_test_nonormal.obj");
}

static void test_cache(void) {
    printf("TEST: cache is reused, and rebuilt when the OBJ changes\n");
    remove(CACHE);
    ObjMesh a, b;
    char err[256] = "";
    int from_cache = -1;

    CHECK(obj_mesh_load_cached(&a, OBJ, CACHE, 4.0f, &from_cache, err, (int)sizeof(err)) &&
          from_cache == 0, "first load parses");
    CHECK(obj_mesh_load_cached(&b, OBJ, CACHE, 4.0f, &from_cache, err, (int)sizeof(err)) &&
          from_cache == 1, "second load reads the cache");
    CHECK(a.vertex_count == b.vertex_count && a.index_count == b.index_count &&
          a.batch_count == b.batch_count &&
          memcmp(a.vertices, b.vertices, a.vertex_count * sizeof(ObjVertex)) == 0 &&
          memcmp(a.indices, b.indices, a.index_count * sizeof(uint32_t)) == 0 &&
          strcmp(a.materials[0].texture, b.materials[0].texture) == 0,
          "cache holds exactly what parsing made");
    obj_mesh_free(&b);

    CHECK(obj_mesh_load_cached(&b, OBJ, CACHE, 8.0f, &from_cache, err, (int)sizeof(err)) &&
          from_cache == 0, "a different cell size reparses");
    obj_mesh_free(&b);

    char changed[2048];
    snprintf(changed, sizeof(changed), "%sv 0 0 0\n", SCENE);
    write_file(OBJ, changed);
    CHECK(obj_mesh_load_cached(&b, OBJ, CACHE, 8.0f, &from_cache, err, (int)sizeof(err)) &&
          from_cache == 0, "an edited OBJ reparses");
    obj_mesh_free(&b);

    /* A truncated cache is refused, not trusted. */
    drop_tail(CACHE, 4);
    CHECK(obj_mesh_load_cached(&b, OBJ, CACHE, 8.0f, &from_cache, err, (int)sizeof(err)) &&
          from_cache == 0 && b.index_count == a.index_count, "a damaged cache is ignored");
    obj_mesh_free(&b);
    obj_mesh_free(&a);
    remove(CACHE);
}

int main(void) {
    write_file(OBJ, SCENE);
    write_file(MTL, MATERIALS);
    test_parse();
    test_errors();
    test_cache();
    remove(OBJ);
    remove(MTL);

    if (g_failures) {
        printf("\n%d check(s) FAILED\n", g_failures);
        return 1;
    }
    printf("\nAll OBJ mesh checks passed\n");
    return 0;
}
