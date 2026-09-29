#include "model/glb.h"
#include "utils/stb_image.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GLB_MAGIC       0x46546C67u   /* "glTF" */
#define GLB_CHUNK_JSON  0x4E4F534Au   /* "JSON" */
#define GLB_CHUNK_BIN   0x004E4942u   /* "BIN\0" */
#define GLB_HEADER_SIZE 20            /* file header plus the first chunk header */

int glb_fail(char* error, int error_size, const char* format, ...) {
    if (error && error_size > 0) {
        va_list args;
        va_start(args, format);
        vsnprintf(error, (size_t)error_size, format, args);
        va_end(args);
    }
    return 0;
}

static unsigned int read_u32(const unsigned char* p) {
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

static int component_size(int component_type) {
    switch (component_type) {
        case GLB_COMPONENT_BYTE: case GLB_COMPONENT_UNSIGNED_BYTE:  return 1;
        case GLB_COMPONENT_SHORT: case GLB_COMPONENT_UNSIGNED_SHORT: return 2;
        case GLB_COMPONENT_UNSIGNED_INT: case GLB_COMPONENT_FLOAT:   return 4;
        default: return 0;
    }
}

static unsigned char* read_file(const char* path, size_t* out_size) {
    if (!path) return NULL;
    FILE* file = fopen(path, "rb");
    if (!file) return NULL;
    unsigned char* bytes = NULL;
    long size = 0;
    if (fseek(file, 0, SEEK_END) == 0 && (size = ftell(file)) > 0) {
        rewind(file);
        bytes = malloc((size_t)size);
        if (bytes && fread(bytes, 1, (size_t)size, file) != (size_t)size) {
            free(bytes);
            bytes = NULL;
        }
    }
    fclose(file);
    if (bytes) *out_size = (size_t)size;
    return bytes;
}

/* Indexes one top-level array into a freshly allocated slice table. An absent
   array is not an error: a file with no skins simply has none. */
static int index_array(JsonSlice root, const char* key, JsonSlice** out, int* out_count,
                       char* error, int error_size) {
    JsonSlice array;
    *out = NULL;
    *out_count = 0;
    if (!json_member(root, key, &array)) return 1;
    int count = json_array_count(array);
    if (count < 0) return glb_fail(error, error_size, "the %s array is malformed", key);
    if (count == 0) return 1;
    JsonSlice* table = malloc(sizeof *table * (size_t)count);
    if (!table) return glb_fail(error, error_size, "out of memory indexing %d %s", count, key);
    if (json_array_index(array, table, count) != count) {
        free(table);
        return glb_fail(error, error_size, "the %s array is malformed", key);
    }
    *out = table;
    *out_count = count;
    return 1;
}

int glb_open(const char* path, Glb* glb, char* error, int error_size) {
    if (!glb) return glb_fail(error, error_size, "no container to fill");
    memset(glb, 0, sizeof *glb);

    size_t size = 0;
    unsigned char* bytes = read_file(path, &size);
    if (!bytes) return glb_fail(error, error_size, "could not read %s", path);
    glb->bytes = bytes;

    if (size < GLB_HEADER_SIZE)
        goto too_small;
    if (read_u32(bytes) != GLB_MAGIC) {
        glb_fail(error, error_size, "file is not a glb (bad magic)");
        goto failed;
    }
    if (read_u32(bytes + 4) != 2) {
        glb_fail(error, error_size, "glb version %u; only version 2 is supported",
                 read_u32(bytes + 4));
        goto failed;
    }

    unsigned int json_length = read_u32(bytes + 12);
    if (read_u32(bytes + 16) != GLB_CHUNK_JSON) {
        glb_fail(error, error_size, "first chunk of the glb is not JSON");
        goto failed;
    }
    if (json_length > size - GLB_HEADER_SIZE) {
        glb_fail(error, error_size, "JSON chunk runs past the end of the file");
        goto failed;
    }

    size_t bin_header = GLB_HEADER_SIZE + (size_t)json_length;
    if (size - bin_header < 8) {
        glb_fail(error, error_size, "glb has no binary chunk");
        goto failed;
    }
    unsigned int bin_length = read_u32(bytes + bin_header);
    if (read_u32(bytes + bin_header + 4) != GLB_CHUNK_BIN) {
        glb_fail(error, error_size, "second chunk of the glb is not BIN");
        goto failed;
    }
    if (bin_length > size - bin_header - 8) {
        glb_fail(error, error_size, "binary chunk runs past the end of the file");
        goto failed;
    }

    glb->root.begin = (const char*)bytes + GLB_HEADER_SIZE;
    glb->root.length = json_length;
    glb->bin = bytes + bin_header + 8;
    glb->bin_size = bin_length;

    if (!index_array(glb->root, "accessors", &glb->accessors, &glb->accessor_count,
                     error, error_size) ||
        !index_array(glb->root, "bufferViews", &glb->views, &glb->view_count,
                     error, error_size))
        goto failed;
    return 1;

too_small:
    glb_fail(error, error_size, "file is too small to be a glb");
failed:
    glb_close(glb);
    return 0;
}

void glb_close(Glb* glb) {
    if (!glb) return;
    free(glb->bytes);
    free(glb->accessors);
    free(glb->views);
    memset(glb, 0, sizeof *glb);
}

int glb_accessor(const Glb* glb, int index, const char* expected_type, int components,
                 GlbAccessor* out, char* error, int error_size) {
    JsonSlice view, value;
    if (index < 0 || index >= glb->accessor_count)
        return glb_fail(error, error_size, "accessor %d is missing", index);
    JsonSlice accessor = glb->accessors[index];

    char type[16];
    if (!json_member(accessor, "type", &value) || !json_string(value, type, sizeof type) ||
        strcmp(type, expected_type) != 0)
        return glb_fail(error, error_size, "accessor %d is not %s", index, expected_type);
    if (!json_member(accessor, "componentType", &value) ||
        !json_int(value, &out->component_type) || component_size(out->component_type) == 0)
        return glb_fail(error, error_size, "accessor %d has an unreadable componentType", index);
    if (!json_member(accessor, "count", &value) || !json_int(value, &out->count) ||
        out->count <= 0)
        return glb_fail(error, error_size, "accessor %d has no usable count", index);
    out->components = components;

    int view_index = 0, accessor_offset = 0;
    if (!json_member(accessor, "bufferView", &value) || !json_int(value, &view_index))
        return glb_fail(error, error_size,
                        "accessor %d has no bufferView (sparse accessors are not supported)",
                        index);
    if (json_member(accessor, "byteOffset", &value) && !json_int(value, &accessor_offset))
        return glb_fail(error, error_size, "accessor %d has a malformed byteOffset", index);

    int view_offset = 0, view_length = 0, view_stride = 0;
    if (view_index < 0 || view_index >= glb->view_count)
        return glb_fail(error, error_size, "bufferView %d is missing", view_index);
    view = glb->views[view_index];
    if (!json_member(view, "byteLength", &value) || !json_int(value, &view_length))
        return glb_fail(error, error_size, "bufferView %d has no byteLength", view_index);
    if ((json_member(view, "byteOffset", &value) && !json_int(value, &view_offset)) ||
        (json_member(view, "byteStride", &value) && !json_int(value, &view_stride)))
        return glb_fail(error, error_size, "bufferView %d has malformed offsets", view_index);
    if (view_offset < 0 || view_length < 0 || view_stride < 0 || accessor_offset < 0)
        return glb_fail(error, error_size, "bufferView %d has negative offsets", view_index);

    size_t element = (size_t)component_size(out->component_type) * (size_t)components;
    size_t stride = view_stride > 0 ? (size_t)view_stride : element;
    if (stride < element)
        return glb_fail(error, error_size, "bufferView %d strides tighter than its elements",
                        view_index);

    size_t span = stride * (size_t)(out->count - 1) + element;
    if ((size_t)accessor_offset > (size_t)view_length ||
        span > (size_t)view_length - (size_t)accessor_offset)
        return glb_fail(error, error_size, "accessor %d runs past bufferView %d",
                        index, view_index);
    size_t start = (size_t)view_offset + (size_t)accessor_offset;
    if (start > glb->bin_size || span > glb->bin_size - start)
        return glb_fail(error, error_size, "accessor %d runs past the binary chunk", index);

    out->data = glb->bin + start;
    out->stride = stride;
    return 1;
}

int glb_read_floats(const GlbAccessor* accessor, float* out, char* error, int error_size) {
    if (accessor->component_type != GLB_COMPONENT_FLOAT)
        return glb_fail(error, error_size, "expected float data, found component type %d",
                        accessor->component_type);
    size_t element = sizeof(float) * (size_t)accessor->components;
    for (int i = 0; i < accessor->count; ++i)
        memcpy(out + (size_t)i * (size_t)accessor->components,
               accessor->data + accessor->stride * (size_t)i, element);
    return 1;
}

int glb_read_uints(const GlbAccessor* accessor, unsigned int* out, unsigned int limit,
                   const char* what, char* error, int error_size) {
    int per_element = accessor->components;
    for (int i = 0; i < accessor->count; ++i) {
        const unsigned char* at = accessor->data + accessor->stride * (size_t)i;
        for (int c = 0; c < per_element; ++c) {
            unsigned int value;
            if (accessor->component_type == GLB_COMPONENT_UNSIGNED_BYTE) {
                value = at[c];
            } else if (accessor->component_type == GLB_COMPONENT_UNSIGNED_SHORT) {
                unsigned short narrow;
                memcpy(&narrow, at + (size_t)c * 2, sizeof narrow);
                value = narrow;
            } else if (accessor->component_type == GLB_COMPONENT_UNSIGNED_INT) {
                memcpy(&value, at + (size_t)c * 4, sizeof value);
            } else {
                return glb_fail(error, error_size,
                                "%s uses component type %d, which is not an unsigned integer",
                                what, accessor->component_type);
            }
            if (value >= limit)
                return glb_fail(error, error_size, "%s %d points at %u of %u",
                                what, i * per_element + c, value, limit);
            out[(size_t)i * (size_t)per_element + (size_t)c] = value;
        }
    }
    return 1;
}

/* ---- images ------------------------------------------------------------- */

/* The bytes of a bufferView, checked the way an accessor's span is. */
static const unsigned char* view_bytes(const Glb* glb, int index, size_t* size) {
    if (index < 0 || index >= glb->view_count) return NULL;
    JsonSlice value;
    int offset = 0, length = 0;
    if (json_member(glb->views[index], "byteOffset", &value)) json_int(value, &offset);
    if (!json_member(glb->views[index], "byteLength", &value) ||
        !json_int(value, &length)) return NULL;
    if (offset < 0 || length <= 0) return NULL;
    if ((size_t)offset > glb->bin_size ||
        (size_t)length > glb->bin_size - (size_t)offset) return NULL;
    *size = (size_t)length;
    return glb->bin + offset;
}

int glb_decode_image(const Glb* glb, int texture_index, unsigned char** pixels,
                     int* width, int* height) {
    JsonSlice textures, texture, images, image, value;
    int source = 0, view = 0;

    *pixels = NULL;
    if (!glb || !json_member(glb->root, "textures", &textures) ||
        !json_array_at(textures, texture_index, &texture) ||
        !json_member(texture, "source", &value) || !json_int(value, &source)) return 0;
    if (!json_member(glb->root, "images", &images) ||
        !json_array_at(images, source, &image) ||
        !json_member(image, "bufferView", &value) || !json_int(value, &view)) return 0;

    size_t size = 0;
    const unsigned char* bytes = view_bytes(glb, view, &size);
    if (!bytes || size > (size_t)0x7FFFFFFF) return 0;

    int channels = 0;
    *pixels = stbi_load_from_memory(bytes, (int)size, width, height, &channels, 3);
    return *pixels != NULL;
}
