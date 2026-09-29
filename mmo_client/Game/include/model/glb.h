#ifndef MODEL_GLB_H
#define MODEL_GLB_H

#include "model/json.h"

#include <stddef.h>

/* The container half of a binary glTF reader: opening the file, and resolving
   an accessor index down to a bounds-checked span of the binary chunk. What
   those spans mean is the caller's business, which is what lets one reader
   serve both a static prop and a skinned character.

   Every offset in the file is treated as hostile. Nothing is dereferenced
   until the last element of an accessor is known to end inside both its
   bufferView and the chunk, because a file that got an offset wrong would
   otherwise read whatever followed the buffer.

   Multi-byte values in a .glb are little-endian, and vertex data is memcpy'd
   straight out of the file, which assumes a little-endian host. Every target
   this builds for is x86; a big-endian port would have to byte-swap here. */

#define GLB_COMPONENT_BYTE           5120
#define GLB_COMPONENT_UNSIGNED_BYTE  5121
#define GLB_COMPONENT_SHORT          5122
#define GLB_COMPONENT_UNSIGNED_SHORT 5123
#define GLB_COMPONENT_UNSIGNED_INT   5125
#define GLB_COMPONENT_FLOAT          5126

typedef struct {
    unsigned char* bytes;        /* the whole file, owned until glb_close */
    JsonSlice root;
    const unsigned char* bin;
    size_t bin_size;
    /* The accessor and bufferView arrays, indexed once at open. A rigged
       character's file holds eight thousand accessors and asks for most of
       them twice; rescanning the JSON text for each would dominate load. */
    JsonSlice* accessors;
    int accessor_count;
    JsonSlice* views;
    int view_count;
} Glb;

typedef struct {
    const unsigned char* data;
    size_t stride;               /* bytes between elements, >= one element */
    int count;
    int component_type;
    int components;              /* 1 for SCALAR, 3 for VEC3, 16 for MAT4 */
} GlbAccessor;

/* Reads and validates the container. Non-zero on success, after which the
   caller owns `glb` until glb_close. */
int glb_open(const char* path, Glb* glb, char* error, int error_size);
void glb_close(Glb* glb);

/* Always returns 0, so callers can `return glb_fail(...)` in one line. */
int glb_fail(char* error, int error_size, const char* format, ...);

/* Binds accessor `index`, insisting it is `type` ("VEC3", "MAT4", ...) with
   `components` components. */
int glb_accessor(const Glb* glb, int index, const char* type, int components,
                 GlbAccessor* out, char* error, int error_size);

/* Copies `count * components` floats out, refusing a non-float accessor.
   Normalized integer attributes are not accepted rather than silently
   misread; nothing this loads uses them. */
int glb_read_floats(const GlbAccessor* accessor, float* out, char* error, int error_size);

/* Copies out as unsigned integers, widening whichever integer type the file
   chose, and rejects any value that is not below `limit`. Indices past the end
   of a vertex array and joints past the end of a skeleton are both the kind of
   mistake that reads off the end of an array at draw time. */
int glb_read_uints(const GlbAccessor* accessor, unsigned int* out, unsigned int limit,
                   const char* what, char* error, int error_size);

/* Decodes the image behind a texture index into a fresh RGB buffer, which the
   caller owns and frees with stbi_image_free. Non-zero when there is an image
   and it decoded.

   The bufferView it lives in is bounds-checked against the binary chunk the way
   an accessor's span is, because a view whose offset ran past the chunk would
   otherwise hand a decoder a pointer into whatever followed it.

   Three channels whatever the file holds: there is no blending in the world
   pass, so an alpha channel would only cost memory. A texture that will not
   decode costs a mesh its map and nothing else, so this reports through its
   return value rather than through `error` - a flat mesh is not a failed
   load. */
int glb_decode_image(const Glb* glb, int texture_index, unsigned char** pixels,
                     int* width, int* height);

#endif
