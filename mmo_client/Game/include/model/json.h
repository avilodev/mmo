#ifndef MODEL_JSON_H
#define MODEL_JSON_H

#include <stddef.h>

typedef struct { const char* begin; size_t length; } JsonSlice;

char* json_read_file(const char* path, char* error, int error_size);
int json_member(JsonSlice object, const char* key, JsonSlice* value);
int json_string(JsonSlice value, char* out, size_t capacity);
int json_int(JsonSlice value, int* out);
int json_float(JsonSlice value, float* out);
int json_array_count(JsonSlice array);
int json_array_at(JsonSlice array, int index, JsonSlice* value);

/* Walks `array` once and writes the slice of every element into `out`, which
   must hold `capacity` entries. Returns the number written, or -1 if the array
   is malformed or holds more than `capacity` elements.

   json_array_at rescans from the front, so reading n elements one at a time
   costs O(n^2). That is invisible for a weapon table and ruinous for a glTF
   file whose accessor array has eight thousand entries, each looked up twice.
   Index the array once, then subscript the result. */
int json_array_index(JsonSlice array, JsonSlice* out, int capacity);
JsonSlice json_slice(const char* text);

#endif
