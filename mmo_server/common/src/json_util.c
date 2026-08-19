/**
 * @file
 * Parse JSON documents into a small owned tree.
 *
 * Recursive descent over the grammar in RFC 8259, with two deliberate limits: the
 * nesting depth is capped so a malformed data file cannot overflow the stack, and
 * numbers are read as doubles only. Neither costs anything for game data files.
 */

#include "json_util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** Bound nesting so a hand-edited data file cannot exhaust the parser's stack. */
#define JSON_MAX_DEPTH 64

/** Hold one parsed value; objects and arrays own their children. */
struct JsonValue {
    JsonType type;
    union {
        int    boolean;
        double number;
        char*  string;      /**< JSON_STRING: owned, NUL-terminated. */
        struct {
            JsonValue** items;   /**< JSON_ARRAY and JSON_OBJECT: owned children. */
            char**      keys;    /**< JSON_OBJECT only: owned member names. */
            int         count;
            int         capacity;
        } list;
    } as;
};

/** Track the cursor and remaining depth budget across the recursive descent. */
typedef struct {
    const char* p;
    int         depth;
    int         failed;
} JsonParser;

static JsonValue* parse_value(JsonParser* ps);

/**
 * Allocate a zeroed value of one type.
 *
 * @return An owned value, or NULL when allocation fails.
 */
static JsonValue* value_new(JsonType type) {
    JsonValue* v = calloc(1, sizeof(JsonValue));
    if (v) v->type = type;
    return v;
}

/**
 * Append one child, and optionally its member name, to an array or object.
 *
 * Takes ownership of `child` and `key` on success and frees both on failure, so a
 * caller never has to unwind a partially appended element.
 *
 * @return 1 on success, or 0 when allocation fails.
 */
static int list_push(JsonValue* parent, char* key, JsonValue* child) {
    if (parent->as.list.count == parent->as.list.capacity) {
        int next = parent->as.list.capacity ? parent->as.list.capacity * 2 : 8;

        JsonValue** items = realloc(parent->as.list.items, (size_t)next * sizeof(*items));
        if (!items) { free(key); json_free(child); return 0; }
        parent->as.list.items = items;

        if (parent->type == JSON_OBJECT) {
            char** keys = realloc(parent->as.list.keys, (size_t)next * sizeof(*keys));
            if (!keys) { free(key); json_free(child); return 0; }
            parent->as.list.keys = keys;
        }
        parent->as.list.capacity = next;
    }

    if (parent->type == JSON_OBJECT) parent->as.list.keys[parent->as.list.count] = key;
    parent->as.list.items[parent->as.list.count] = child;
    parent->as.list.count++;
    return 1;
}

/** Advance past whitespace. */
static void skip_ws(JsonParser* ps) {
    while (*ps->p && isspace((unsigned char)*ps->p)) ps->p++;
}

/**
 * Consume one expected character.
 *
 * @return 1 when the character matched and was consumed, or 0 otherwise.
 */
static int accept(JsonParser* ps, char c) {
    skip_ws(ps);
    if (*ps->p != c) return 0;
    ps->p++;
    return 1;
}

/**
 * Append one UTF-8 encoding of a code point.
 *
 * @return The number of bytes written, at most four.
 */
static int encode_utf8(unsigned int cp, char* out) {
    if (cp < 0x80)    { out[0] = (char)cp; return 1; }
    if (cp < 0x800)   { out[0] = (char)(0xC0 | (cp >> 6));
                        out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) { out[0] = (char)(0xE0 | (cp >> 12));
                        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        out[2] = (char)(0x80 | (cp & 0x3F)); return 3; }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/**
 * Read four hex digits as a code unit.
 *
 * @return 1 on success with the value in `out`, or 0 on a malformed escape.
 */
static int read_hex4(const char* p, unsigned int* out) {
    unsigned int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if      (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return 0;
    }
    *out = v;
    return 1;
}

/**
 * Parse a quoted string, resolving escapes including surrogate pairs.
 *
 * The decoded form is never longer than the source, so one allocation sized to the
 * raw span always suffices.
 *
 * @return An owned NUL-terminated string, or NULL on a malformed string.
 */
static char* parse_string(JsonParser* ps) {
    skip_ws(ps);
    if (*ps->p != '"') { ps->failed = 1; return NULL; }
    ps->p++;

    const char* scan = ps->p;
    while (*scan != '"') {
        if (!*scan) { ps->failed = 1; return NULL; }
        if (*scan == '\\') {
            if (!scan[1]) { ps->failed = 1; return NULL; }
            scan += 2;
            continue;
        }
        scan++;
    }

    /* Escapes only ever shrink: \uXXXX is six source bytes and at most four UTF-8
     * bytes, and every other escape is two bytes in and one out. The raw span is
     * therefore always a sufficient bound for the decoded form. */
    char* out = malloc((size_t)(scan - ps->p) + 1);
    if (!out) { ps->failed = 1; return NULL; }

    size_t len = 0;
    while (*ps->p && *ps->p != '"') {
        if (*ps->p != '\\') { out[len++] = *ps->p++; continue; }

        ps->p++;
        switch (*ps->p) {
            case '"':  out[len++] = '"';  ps->p++; break;
            case '\\': out[len++] = '\\'; ps->p++; break;
            case '/':  out[len++] = '/';  ps->p++; break;
            case 'b':  out[len++] = '\b'; ps->p++; break;
            case 'f':  out[len++] = '\f'; ps->p++; break;
            case 'n':  out[len++] = '\n'; ps->p++; break;
            case 'r':  out[len++] = '\r'; ps->p++; break;
            case 't':  out[len++] = '\t'; ps->p++; break;
            case 'u': {
                unsigned int cp = 0;
                if (!read_hex4(ps->p + 1, &cp)) { free(out); ps->failed = 1; return NULL; }
                ps->p += 5;
                if (cp >= 0xD800 && cp <= 0xDBFF && ps->p[0] == '\\' && ps->p[1] == 'u') {
                    unsigned int lo = 0;
                    if (read_hex4(ps->p + 2, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        ps->p += 6;
                    }
                }
                len += (size_t)encode_utf8(cp, out + len);
                break;
            }
            default: free(out); ps->failed = 1; return NULL;
        }
    }

    if (*ps->p != '"') { free(out); ps->failed = 1; return NULL; }
    ps->p++;
    out[len] = '\0';
    return out;
}

/**
 * Parse an array, consuming the closing bracket.
 *
 * @return An owned array value, or NULL on a syntax error.
 */
static JsonValue* parse_array(JsonParser* ps) {
    JsonValue* arr = value_new(JSON_ARRAY);
    if (!arr) { ps->failed = 1; return NULL; }

    if (accept(ps, ']')) return arr;

    for (;;) {
        JsonValue* item = parse_value(ps);
        if (!item) { json_free(arr); return NULL; }
        if (!list_push(arr, NULL, item)) { json_free(arr); ps->failed = 1; return NULL; }

        if (accept(ps, ',')) continue;
        if (accept(ps, ']')) return arr;

        json_free(arr);
        ps->failed = 1;
        return NULL;
    }
}

/**
 * Parse an object, consuming the closing brace.
 *
 * Duplicate keys are kept in document order; json_get() returns the first.
 *
 * @return An owned object value, or NULL on a syntax error.
 */
static JsonValue* parse_object(JsonParser* ps) {
    JsonValue* obj = value_new(JSON_OBJECT);
    if (!obj) { ps->failed = 1; return NULL; }

    if (accept(ps, '}')) return obj;

    for (;;) {
        char* key = parse_string(ps);
        if (!key) { json_free(obj); return NULL; }

        if (!accept(ps, ':')) { free(key); json_free(obj); ps->failed = 1; return NULL; }

        JsonValue* val = parse_value(ps);
        if (!val) { free(key); json_free(obj); return NULL; }
        if (!list_push(obj, key, val)) { json_free(obj); ps->failed = 1; return NULL; }

        if (accept(ps, ',')) continue;
        if (accept(ps, '}')) return obj;

        json_free(obj);
        ps->failed = 1;
        return NULL;
    }
}

/**
 * Parse any single value.
 *
 * @return An owned value, or NULL on a syntax error or on exceeding JSON_MAX_DEPTH.
 */
static JsonValue* parse_value(JsonParser* ps) {
    if (ps->depth >= JSON_MAX_DEPTH) { ps->failed = 1; return NULL; }
    skip_ws(ps);

    if (*ps->p == '{' || *ps->p == '[') {
        char open = *ps->p;
        ps->p++;
        ps->depth++;
        JsonValue* v = (open == '{') ? parse_object(ps) : parse_array(ps);
        ps->depth--;
        return v;
    }

    if (*ps->p == '"') {
        char* s = parse_string(ps);
        if (!s) return NULL;
        JsonValue* v = value_new(JSON_STRING);
        if (!v) { free(s); ps->failed = 1; return NULL; }
        v->as.string = s;
        return v;
    }

    if (strncmp(ps->p, "true", 4) == 0 || strncmp(ps->p, "false", 5) == 0) {
        int truth = (*ps->p == 't');
        ps->p += truth ? 4 : 5;
        JsonValue* v = value_new(JSON_BOOL);
        if (!v) { ps->failed = 1; return NULL; }
        v->as.boolean = truth;
        return v;
    }

    if (strncmp(ps->p, "null", 4) == 0) {
        ps->p += 4;
        JsonValue* v = value_new(JSON_NULL);
        if (!v) ps->failed = 1;
        return v;
    }

    char* end = NULL;
    double num = strtod(ps->p, &end);
    if (end == ps->p) { ps->failed = 1; return NULL; }
    ps->p = end;

    JsonValue* v = value_new(JSON_NUMBER);
    if (!v) { ps->failed = 1; return NULL; }
    v->as.number = num;
    return v;
}

/**
 * Parse a complete document and reject trailing content.
 *
 * @return An owned tree the caller frees with json_free(), or NULL on a syntax error.
 */
JsonValue* json_parse(const char* text) {
    if (!text) return NULL;

    JsonParser ps = { .p = text, .depth = 0, .failed = 0 };
    JsonValue* root = parse_value(&ps);
    if (!root) return NULL;

    skip_ws(&ps);
    if (*ps.p != '\0') { json_free(root); return NULL; }
    return root;
}

/**
 * Read and parse a JSON file.
 *
 * @param path  Path to the document.
 * @param err   Receives a static description of the failure; may be NULL.
 * @return      An owned tree the caller frees with json_free(), or NULL on failure.
 */
JsonValue* json_parse_file(const char* path, const char** err) {
    if (err) *err = NULL;
    if (!path) { if (err) *err = "no path given"; return NULL; }

    FILE* f = fopen(path, "rb");
    if (!f) { if (err) *err = "cannot open file"; return NULL; }

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); if (err) *err = "cannot seek file"; return NULL; }
    long size = ftell(f);
    if (size < 0) { fclose(f); if (err) *err = "cannot size file"; return NULL; }
    rewind(f);

    char* buffer = malloc((size_t)size + 1);
    if (!buffer) { fclose(f); if (err) *err = "out of memory"; return NULL; }

    size_t got = fread(buffer, 1, (size_t)size, f);
    fclose(f);
    buffer[got] = '\0';

    JsonValue* root = json_parse(buffer);
    free(buffer);

    if (!root && err) *err = "malformed JSON";
    return root;
}

/**
 * Release a tree and every value it owns.
 */
void json_free(JsonValue* value) {
    if (!value) return;

    if (value->type == JSON_STRING) {
        free(value->as.string);
    } else if (value->type == JSON_ARRAY || value->type == JSON_OBJECT) {
        for (int i = 0; i < value->as.list.count; i++) {
            json_free(value->as.list.items[i]);
            if (value->as.list.keys) free(value->as.list.keys[i]);
        }
        free(value->as.list.items);
        free(value->as.list.keys);
    }
    free(value);
}

/** Report a value's type, or JSON_NULL when the value is NULL. */
JsonType json_type(const JsonValue* value) {
    return value ? value->type : JSON_NULL;
}

/**
 * Look up a member of an object.
 *
 * @return The first member with this key, or NULL when absent or not an object.
 */
const JsonValue* json_get(const JsonValue* value, const char* key) {
    if (!value || value->type != JSON_OBJECT || !key) return NULL;
    for (int i = 0; i < value->as.list.count; i++) {
        if (strcmp(value->as.list.keys[i], key) == 0) return value->as.list.items[i];
    }
    return NULL;
}

/** Count an array's elements, or zero when `value` is not an array. */
int json_count(const JsonValue* value) {
    if (!value || value->type != JSON_ARRAY) return 0;
    return value->as.list.count;
}

/**
 * Index an array.
 *
 * @return The element, or NULL when out of range or when `value` is not an array.
 */
const JsonValue* json_at(const JsonValue* value, int index) {
    if (!value || value->type != JSON_ARRAY) return NULL;
    if (index < 0 || index >= value->as.list.count) return NULL;
    return value->as.list.items[index];
}

/** Count an object's members, or zero when `value` is not an object. */
int json_member_count(const JsonValue* value) {
    if (!value || value->type != JSON_OBJECT) return 0;
    return value->as.list.count;
}

/** Return the key of the object member at `index`, or NULL when out of range. */
const char* json_key_at(const JsonValue* value, int index) {
    if (!value || value->type != JSON_OBJECT) return NULL;
    if (index < 0 || index >= value->as.list.count) return NULL;
    return value->as.list.keys[index];
}

/** Return the value of the object member at `index`, or NULL when out of range. */
const JsonValue* json_member_at(const JsonValue* value, int index) {
    if (!value || value->type != JSON_OBJECT) return NULL;
    if (index < 0 || index >= value->as.list.count) return NULL;
    return value->as.list.items[index];
}

/** Read a string value, or `fallback` when absent or of another type. */
const char* json_as_string(const JsonValue* value, const char* fallback) {
    if (!value || value->type != JSON_STRING) return fallback;
    return value->as.string;
}

/** Read a numeric value, or `fallback` when absent or of another type. */
double json_as_number(const JsonValue* value, double fallback) {
    if (!value || value->type != JSON_NUMBER) return fallback;
    return value->as.number;
}

/** Read a numeric value truncated to int, or `fallback` when absent or of another type. */
int json_as_int(const JsonValue* value, int fallback) {
    if (!value || value->type != JSON_NUMBER) return fallback;
    return (int)value->as.number;
}

/** Read a boolean value, or `fallback` when absent or of another type. */
int json_as_bool(const JsonValue* value, int fallback) {
    if (!value || value->type != JSON_BOOL) return fallback;
    return value->as.boolean;
}

/** Read `key` from an object as a string, or `fallback` when absent. */
const char* json_get_string(const JsonValue* value, const char* key, const char* fallback) {
    return json_as_string(json_get(value, key), fallback);
}

/** Read `key` from an object as a number, or `fallback` when absent. */
double json_get_number(const JsonValue* value, const char* key, double fallback) {
    return json_as_number(json_get(value, key), fallback);
}

/** Read `key` from an object as an int, or `fallback` when absent. */
int json_get_int(const JsonValue* value, const char* key, int fallback) {
    return json_as_int(json_get(value, key), fallback);
}

/** Read `key` from an object as a boolean, or `fallback` when absent. */
int json_get_bool(const JsonValue* value, const char* key, int fallback) {
    return json_as_bool(json_get(value, key), fallback);
}
