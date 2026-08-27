/**
 * @file
 * Load the generator's downscaled world overview and hold it as a texture.
 */
#include "world/world_overview.h"
#include "world/tile_palette.h"

#include <GLFW/glfw3.h>
// Windows/MinGW ships OpenGL 1.1 headers, which predate GL_CLAMP_TO_EDGE (1.2).
// texture.h already carries the guarded fallback this needs.
#include "texture/texture.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/client_log.h"

/** Refuse absurd headers so a corrupt file cannot drive a huge allocation. */
#define OVERVIEW_MAX_DIM 8192

static struct {
    unsigned int texture;
    int width;
    int height;
    int scale;
} g_overview;

/**
 * Read the overview file and upload it as an RGB texture.
 *
 * @param world_file_path  Path passed to world_init; ".overview" is appended.
 * @return                 1 if an overview is now available, otherwise 0.
 */
int world_overview_load(const char* world_file_path) {
    world_overview_unload();
    if (!world_file_path) return 0;

    char path[512];
    int n = snprintf(path, sizeof(path), "%s.overview", world_file_path);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        CLOG_WARN("[OVERVIEW] World path too long; map overview disabled");
        return 0;
    }

    FILE* f = fopen(path, "rb");
    if (!f) {
        CLOG_INFO("[OVERVIEW] No overview at %s; map shows grid only", path);
        return 0;
    }

    int32_t ow = 0, oh = 0, scale = 0;
    if (fread(&ow, sizeof(int32_t), 1, f) != 1 ||
        fread(&oh, sizeof(int32_t), 1, f) != 1 ||
        fread(&scale, sizeof(int32_t), 1, f) != 1) {
        CLOG_WARN("[OVERVIEW] %s is truncated; map shows grid only", path);
        fclose(f);
        return 0;
    }

    if (ow <= 0 || oh <= 0 || scale <= 0 ||
        ow > OVERVIEW_MAX_DIM || oh > OVERVIEW_MAX_DIM) {
        CLOG_WARN("[OVERVIEW] %s has invalid header %dx%d scale %d",
               path, ow, oh, scale);
        fclose(f);
        return 0;
    }

    size_t cells = (size_t)ow * (size_t)oh;
    uint8_t* indices = malloc(cells);
    if (!indices) { fclose(f); return 0; }

    if (fread(indices, 1, cells, f) != cells) {
        CLOG_WARN("[OVERVIEW] %s payload is short; map shows grid only", path);
        free(indices);
        fclose(f);
        return 0;
    }
    fclose(f);

    // Expand palette indices to RGB for the GPU.
    uint8_t* rgb = malloc(cells * 3);
    if (!rgb) { free(indices); return 0; }

    for (size_t i = 0; i < cells; i++) {
        const float* c = tile_palette_rgb(indices[i]);
        if (!c) {
            // PAL_EMPTY or out of range: draw as the map's void colour.
            rgb[i * 3 + 0] = 10;
            rgb[i * 3 + 1] = 12;
            rgb[i * 3 + 2] = 18;
            continue;
        }
        rgb[i * 3 + 0] = (uint8_t)(c[0] * 255.0f);
        rgb[i * 3 + 1] = (uint8_t)(c[1] * 255.0f);
        rgb[i * 3 + 2] = (uint8_t)(c[2] * 255.0f);
    }
    free(indices);

    unsigned int tex = 0;
    glGenTextures(1, &tex);
    if (!tex) { free(rgb); return 0; }

    glBindTexture(GL_TEXTURE_2D, tex);
    // Rows are byte-packed and the width is rarely a multiple of four.
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, ow, oh, 0,
                 GL_RGB, GL_UNSIGNED_BYTE, rgb);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    free(rgb);

    g_overview.texture = tex;
    g_overview.width   = ow;
    g_overview.height  = oh;
    g_overview.scale   = scale;

    CLOG_INFO("[OVERVIEW] Loaded %dx%d cells at 1/%d scale from %s",
           ow, oh, scale, path);
    return 1;
}

/** Release the overview texture and reset state. */
void world_overview_unload(void) {
    if (g_overview.texture) {
        glDeleteTextures(1, &g_overview.texture);
    }
    memset(&g_overview, 0, sizeof(g_overview));
}

/** @return Nonzero once an overview texture is ready to draw. */
int world_overview_ready(void) {
    return g_overview.texture != 0;
}

/** @return The overview's OpenGL texture, or 0 when none is loaded. */
unsigned int world_overview_texture(void) {
    return g_overview.texture;
}

/** Report overview geometry; any output pointer may be NULL. */
void world_overview_dims(int* out_width, int* out_height, int* out_scale) {
    if (out_width)  *out_width  = g_overview.width;
    if (out_height) *out_height = g_overview.height;
    if (out_scale)  *out_scale  = g_overview.scale;
}
