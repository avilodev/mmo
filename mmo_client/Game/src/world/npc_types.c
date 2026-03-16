#include "npc_types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_NPC_TYPE_ID 256

static char  g_names[MAX_NPC_TYPE_ID][32];
static int   g_loaded = 0;

// ============================================================================
// MINIMAL JSON HELPERS  (no external deps, same approach as items.json parser)
// ============================================================================

static const char* skip_ws(const char* p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

// Advance past a quoted string (p must point at the opening '"')
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

// Copy the contents of a quoted string into buf (p points at '"')
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

// Skip over a single JSON value (string, number, true/false/null, or nested {}/[])
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

// ============================================================================
// PUBLIC API
// ============================================================================

void npc_types_init(const char* path) {
    memset(g_names, 0, sizeof(g_names));
    g_loaded = 1;

    FILE* f = fopen(path, "r");
    if (!f) {
        printf("[NPC_TYPES] %s not found — NPC names will be generic\n", path);
        return;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    char* json = malloc((size_t)fsize + 1);
    if (!json) { fclose(f); return; }
    fread(json, 1, (size_t)fsize, f);
    json[fsize] = '\0';
    fclose(f);

    int count = 0;
    const char* p = json;

    // Walk through every '{' ... '}' object in the top-level array
    while (*p) {
        p = skip_ws(p);
        if (*p != '{') { p++; continue; }
        p++; // enter object

        int  id   = -1;
        char name[32] = "";

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
            count++;
        }
    }

    free(json);
    printf("[NPC_TYPES] Loaded %d NPC types\n", count);
}

void npc_types_cleanup(void) {
    memset(g_names, 0, sizeof(g_names));
    g_loaded = 0;
}

const char* npc_type_get_name(uint8_t type_id) {
    if (!g_loaded || g_names[type_id][0] == '\0') return NULL;
    return g_names[type_id];
}
