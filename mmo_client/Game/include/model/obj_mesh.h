#ifndef OBJ_MESH_H
#define OBJ_MESH_H

/**
 * @file
 * Load a static Wavefront OBJ (Blender's exporter) into one indexed mesh,
 * split into ground-grid cells so a renderer can skip the parts of a large
 * scene that are out of view.
 *
 * GL-free, so it is tested headless; render/city_renderer.c uploads the
 * result. Coordinates stay in the file's units and axes (Blender's OBJ export
 * is Y up, -Z forward, which is already GL world space: D1). V is flipped so
 * a texture uploaded top row first samples the right way up.
 */

#include <stdint.h>

#define OBJ_MAX_MATERIALS   16
#define OBJ_NAME_LEN        64
#define OBJ_PATH_LEN        260

/** One vertex as the GPU takes it: 24 bytes. */
typedef struct {
    float  x, y, z;
    int8_t nx, ny, nz, pad;   /**< Unit normal * 127. */
    float  u, v;
} ObjVertex;

/** One cell's triangles of one material: a single draw call. */
typedef struct {
    uint32_t first;           /**< Into ObjMesh.indices. */
    uint32_t count;           /**< Indices, a multiple of 3. */
    int32_t  material;
    float    min[3], max[3];  /**< Bounds of those triangles, file units. */
} ObjBatch;

typedef struct {
    char name[OBJ_NAME_LEN];
    char texture[OBJ_PATH_LEN];   /**< map_Kd, relative to the process; "" if none. */
    float diffuse[3];             /**< Kd; white when the file gives none. */
} ObjMaterial;

typedef struct {
    ObjVertex*  vertices;
    uint32_t    vertex_count;
    uint32_t*   indices;
    uint32_t    index_count;
    ObjBatch*   batches;
    int32_t     batch_count;
    ObjMaterial materials[OBJ_MAX_MATERIALS];
    int32_t     material_count;
    float       min[3], max[3];   /**< Bounds of every vertex. */
} ObjMesh;

/** Parse an OBJ file (and its mtllib). Polygons are fan-triangulated; each
 *  triangle goes to the cell of side cell_size (file units, on X/Z) holding its
 *  centroid.
 *  @return Nonzero on success; on failure the mesh is empty and err says why. */
int obj_mesh_load(ObjMesh* out, const char* path, float cell_size,
                  char* err, int err_len);

/** obj_mesh_load(), through a binary cache: read cache_path if it was written
 *  from this exact OBJ (same size, same first and last megabyte) with this
 *  cell size, else parse the OBJ and (re)write the cache. *from_cache says
 *  which happened. A cache that cannot be written is not an error. */
int obj_mesh_load_cached(ObjMesh* out, const char* obj_path, const char* cache_path,
                         float cell_size, int* from_cache, char* err, int err_len);

void obj_mesh_free(ObjMesh* mesh);

#endif /* OBJ_MESH_H */
