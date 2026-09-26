/**
 * @file
 * Load the client mapping from NPC type identifiers to display names.
 */

#include "npc_types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/client_log.h"

#define MAX_NPC_TYPE_ID 256

static char  g_names[MAX_NPC_TYPE_ID][32];

/** Generated presentation, parallel to g_names.
 *
 * Faction hue times role value, and the role's box scale, both resolved by the
 * generator rather than decided here. g_has_style distinguishes "drawn plain"
 * from "no row", so a client older than the content it is talking to falls back
 * to category colouring instead of drawing everything black. */
static float g_color[MAX_NPC_TYPE_ID][3];
static float g_size[MAX_NPC_TYPE_ID];
static unsigned char g_has_style[MAX_NPC_TYPE_ID];
static float g_outline[MAX_NPC_TYPE_ID][3];
static unsigned char g_has_outline[MAX_NPC_TYPE_ID];

static int   g_loaded = 0;

static const char* skip_ws(const char* p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

/**
 * Advance past a quoted JSON string and its escaped bytes.
 *
 * @param p  Input position expected to point at an opening quote.
 * @return      First byte after the closing quote, or the input position when unquoted.
 */
static const char* skip_str(const char* p) {
    if (*p != '"') return p;
    p++;
    while (*p && *p != '"') {
        if (*p == '\\' && *(p + 1)) p++; // skip escape
        p++;
    }
    if (*p == '"') p++;
    return p;
}

/**
 * Copy a quoted JSON string into a bounded buffer.
 *
 * @param p  Input position expected to point at an opening quote.
 * @param buf  Destination buffer that receives a NUL-terminated value.
 * @param buf_sz  Destination capacity in bytes; must be positive.
 */
static void read_str(const char* p, char* buf, int buf_sz) {
    buf[0] = '\0';
    if (*p != '"') return;
    p++;
    int i = 0;
    while (*p && *p != '"' && i < buf_sz - 1) {
        if (*p == '\\' && *(p + 1)) p++; // skip escape char
        buf[i++] = *p++;
    }
    buf[i] = '\0';
}

// Read a decimal integer starting at p
static int read_int(const char* p) {
    p = skip_ws(p);
    return (int)strtol(p, NULL, 10);
}

/** Read a JSON float starting at p. */
static float read_float(const char* p) {
    p = skip_ws(p);
    return (float)strtod(p, NULL);
}

/** Read a fixed-length array of floats, e.g. "color": [0.5, 0.4, 0.2].
 *
 * @param p      Position expected to point at the opening bracket.
 * @param out    Receives `n` values; left untouched when the shape is wrong.
 * @param n      How many values to read.
 * @return       1 when all `n` were read, 0 otherwise.
 */
static int read_float_array(const char* p, float* out, int n) {
    p = skip_ws(p);
    if (*p != '[') return 0;
    p++;
    for (int i = 0; i < n; i++) {
        p = skip_ws(p);
        if (*p == ']' || *p == '\0') return 0;
        out[i] = read_float(p);
        while (*p && *p != ',' && *p != ']') p++;
        if (*p == ',') p++;
    }
    return 1;
}

/**
 * Advance past one JSON scalar or balanced container value.
 *
 * @return      First byte following the parsed value.
 */
static const char* skip_value(const char* p) {
    p = skip_ws(p);
    if (*p == '"') return skip_str(p);
    if (*p == '{' || *p == '[') {
        char open = *p, close = (*p == '{') ? '}' : ']';
        int depth = 1; p++;
        while (*p && depth > 0) {
            if (*p == '"') { p = skip_str(p); continue; }
            if (*p == open)  depth++;
            if (*p == close) depth--;
            p++;
        }
        return p;
    }
    // number / boolean / null — skip until delimiter
    while (*p && *p != ',' && *p != '}' && *p != ']') p++;
    return p;
}

/**
 * Load NPC display names from a JSON array.
 *
 * Missing or unreadable data leaves the table loaded but empty.
 *
 * @param path  Path to the NPC type JSON file.
 */
void npc_types_init(const char* path) {
    memset(g_names, 0, sizeof(g_names));
    memset(g_color, 0, sizeof(g_color));
    memset(g_size, 0, sizeof(g_size));
    memset(g_has_style, 0, sizeof(g_has_style));
    memset(g_outline, 0, sizeof(g_outline));
    memset(g_has_outline, 0, sizeof(g_has_outline));
    g_loaded = 1;

    FILE* f = fopen(path, "r");
    if (!f) {
        CLOG_INFO("[NPC_TYPES] %s not found — NPC names will be generic", path);
        return;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    char* json = malloc((size_t)fsize + 1);
    if (!json) { fclose(f); return; }

    /* Terminate at what was actually read, not at the size ftell reported.
     * The file is opened in text mode, so on a platform that translates line
     * endings the read is legitimately shorter than the file -- and the
     * scanner below walks to a NUL, so terminating past the data walks it
     * through uninitialised heap. */
    size_t got = fread(json, 1, (size_t)fsize, f);
    json[got] = '\0';
    fclose(f);

    int count = 0;

    /* Anchor the walk on the npc_types array.
     *
     * It used to start at the document root, which made the root '{' itself look
     * like an entry: the first key inside it that was neither "id" nor "name"
     * had its value skipped by skip_value(), and for "npc_types" that value is
     * the entire array. The walk then ran off the end having seen no rows, so
     * the table was always empty and every NPC rendered as "NPC_<id>".
     */
    const char* p = strstr(json, "\"npc_types\"");
    if (p) p = strchr(p, '[');
    if (!p) {
        CLOG_WARN("[NPC_TYPES] %s has no npc_types array — names will be generic", path);
        free(json);
        return;
    }
    p++;   /* step past '[' into the array */

    // Walk through every '{' ... '}' object inside it
    while (*p && *p != ']') {
        p = skip_ws(p);
        /* skip_ws stops on the terminator, and every generated file ends with a
         * newline -- so without this the '!=' below is true, p++ steps past the
         * NUL, and the loop condition reads off the end of the buffer. */
        if (*p == '\0' || *p == ']') break;
        if (*p != '{') { p++; continue; }
        p++; // enter object

        int   id   = -1;
        char  name[32] = "";
        float color[3] = { 0.0f, 0.0f, 0.0f };
        float outline[3] = { 0.0f, 0.0f, 0.0f };
        float size = 1.0f;
        int   have_color = 0;
        int   have_outline = 0;

        while (*p && *p != '}') {
            p = skip_ws(p);
            if (*p != '"') { p++; continue; }

            char key[32];
            read_str(p, key, sizeof(key));
            p = skip_str(p);

            p = skip_ws(p);
            if (*p == ':') p++;
            p = skip_ws(p);

            if (strcmp(key, "id") == 0) {
                id = read_int(p);
                p = skip_value(p);
            } else if (strcmp(key, "name") == 0) {
                read_str(p, name, sizeof(name));
                p = skip_value(p);
            } else if (strcmp(key, "color") == 0) {
                have_color = read_float_array(p, color, 3);
                p = skip_value(p);
            } else if (strcmp(key, "outline") == 0) {
                have_outline = read_float_array(p, outline, 3);
                p = skip_value(p);
            } else if (strcmp(key, "size_scale") == 0) {
                size = read_float(p);
                p = skip_value(p);
            } else {
                p = skip_value(p);
            }

            p = skip_ws(p);
            if (*p == ',') p++;
        }
        if (*p == '}') p++;

        if (id >= 0 && id < MAX_NPC_TYPE_ID && name[0] != '\0') {
            memcpy(g_names[id], name, 31);
            g_names[id][31] = '\0';
            if (have_color) {
                g_color[id][0] = color[0];
                g_color[id][1] = color[1];
                g_color[id][2] = color[2];
                g_size[id] = (size > 0.0f) ? size : 1.0f;
                g_has_style[id] = 1;
            }
            if (have_outline) {
                g_outline[id][0] = outline[0];
                g_outline[id][1] = outline[1];
                g_outline[id][2] = outline[2];
                g_has_outline[id] = 1;
            }
            count++;
        }
    }

    free(json);
    CLOG_INFO("[NPC_TYPES] Loaded %d NPC types", count);
}

/**
 * Clear the NPC display-name table.
 */
void npc_types_cleanup(void) {
    memset(g_names, 0, sizeof(g_names));
    memset(g_color, 0, sizeof(g_color));
    memset(g_size, 0, sizeof(g_size));
    memset(g_has_style, 0, sizeof(g_has_style));
    memset(g_outline, 0, sizeof(g_outline));
    memset(g_has_outline, 0, sizeof(g_has_outline));
    g_loaded = 0;
}

int npc_type_get_style(uint8_t type_id, float* out_rgb, float* out_size) {
    if (!g_loaded || !g_has_style[type_id]) return 0;
    if (out_rgb) {
        out_rgb[0] = g_color[type_id][0];
        out_rgb[1] = g_color[type_id][1];
        out_rgb[2] = g_color[type_id][2];
    }
    if (out_size) *out_size = g_size[type_id];
    return 1;
}

int npc_type_get_outline(uint8_t type_id, float* out_rgb) {
    if (!g_loaded || !g_has_outline[type_id]) return 0;
    if (out_rgb) {
        out_rgb[0] = g_outline[type_id][0];
        out_rgb[1] = g_outline[type_id][1];
        out_rgb[2] = g_outline[type_id][2];
    }
    return 1;
}

/**
 * Find the loaded display name for an NPC type.
 *
 * The returned pointer refers to static storage until cleanup or reinitialization.
 *
 * @return      Display name, or NULL when the table or identifier is unavailable.
 */
const char* npc_type_get_name(uint8_t type_id) {
    if (!g_loaded || g_names[type_id][0] == '\0') return NULL;
    return g_names[type_id];
}
