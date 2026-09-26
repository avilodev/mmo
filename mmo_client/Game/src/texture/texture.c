/**
 * @file
 * Load image files into OpenGL textures and query or release texture objects.
 */

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "texture.h"
#include "texture/texture_atlas.h"
#include <windows.h>
#include <GL/gl.h>
#include <GLFW/glfw3.h>
#include "render/gl_loader.h"
#include <stdio.h>
#include <stdlib.h>
#include "core/client_log.h"


/**
 * Load an image file into a nearest-filtered OpenGL texture.
 *
 * A current OpenGL context must exist before this call.
 *
 * @param filepath  Path to an image format supported by stb_image.
 * @return      OpenGL texture object, or zero when image loading fails.
 */
unsigned int texture_load(const char* filepath) {
    unsigned int texture_id;
    int width, height, channels;
    
    unsigned char* data = stbi_load(filepath, &width, &height, &channels, 0);
    
    if (!data) {
        CLOG_ERROR("Failed to load texture: %s", filepath);
        CLOG_ERROR("STB Error: %s", stbi_failure_reason());
        return 0;
    }
    
    CLOG_DEBUG("Loaded texture: %s (%dx%d, %d channels)", 
           filepath, width, height, channels);
    
    glGenTextures(1, &texture_id);
    glBindTexture(GL_TEXTURE_2D, texture_id);
    
    // Use GL_CLAMP instead of GL_CLAMP_TO_EDGE for OpenGL 1.1 compatibility
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    
    GLenum format = GL_RGB;
    if (channels == 4) {
        format = GL_RGBA;
    } else if (channels == 3) {
        format = GL_RGB;
    } else if (channels == 1) {
        format = GL_LUMINANCE;  // GL_RED might not be available either
    }
    
    glTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 
                 0, format, GL_UNSIGNED_BYTE, data);
    
    stbi_image_free(data);
    
    return texture_id;
}

/** Gutter width for ground tilesets: two mip levels before it drops under a
 *  texel, which is as far as a 55-degree view minifies nearby ground. */
#define TILESET_PAD 4

/** Anisotropy asked for on ground textures, capped by what the driver allows. */
#define TILESET_ANISOTROPY 8.0f

unsigned int texture_load_tileset(const char* filepath, int cols, int rows,
                                  TilesetLayout* layout) {
    int width, height, channels;
    unsigned char* data = stbi_load(filepath, &width, &height, &channels, 4);
    if (!data) {
        CLOG_ERROR("Failed to load tileset: %s (%s)", filepath, stbi_failure_reason());
        return 0;
    }

    int pad = TILESET_PAD;
    int out_w = width, out_h = height;
    unsigned char* padded = NULL;
    if (cols > 0 && rows > 0)
        padded = texture_atlas_pad(data, width, height, 4, cols, rows, pad, &out_w, &out_h);
    if (!padded) {
        /* Not a whole number of tiles: draw it as it is rather than not at
         * all, and without mips, since there is no gutter to protect. */
        CLOG_WARN("[TEXTURE] %s (%dx%d) is not %dx%d whole tiles; loading unpadded",
                  filepath, width, height, cols, rows);
        pad = 0;
    }

    layout->tile_w = (cols > 0) ? width / cols : width;
    layout->tile_h = (rows > 0) ? height / rows : height;
    layout->pad    = pad;

    unsigned int texture_id;
    glGenTextures(1, &texture_id);
    glBindTexture(GL_TEXTURE_2D, texture_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, out_w, out_h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 padded ? padded : data);

    int levels = texture_atlas_mip_levels(pad);
    if (levels > 0) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, levels);
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_LINEAR);
        float aniso = gl_loader_max_anisotropy();
        if (aniso > TILESET_ANISOTROPY) aniso = TILESET_ANISOTROPY;
        if (aniso > 1.0f)
            glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY, aniso);
    } else {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    }

    free(padded);
    stbi_image_free(data);
    CLOG_DEBUG("Loaded tileset: %s (%dx%d tiles of %dx%d, pad %d, %d mips)",
               filepath, cols, rows, layout->tile_w, layout->tile_h, pad, levels);
    return texture_id;
}

/**
 * Delete an OpenGL texture object when nonzero.
 *
 * A current OpenGL context must exist before this call.
 */
void texture_unload(unsigned int texture_id) {
    if (texture_id != 0) {
        glDeleteTextures(1, &texture_id);
        CLOG_DEBUG("Unloaded texture ID: %u", texture_id);
    }
}

/**
 * Query the level-zero dimensions of an OpenGL texture.
 *
 * A current OpenGL context must exist before this call.
 *
 * @param width  Destination for the texture width in pixels.
 * @param height  Destination for the texture height in pixels.
 */
void texture_get_size(unsigned int texture_id, int* width, int* height) {
    glBindTexture(GL_TEXTURE_2D, texture_id);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, width);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, height);
}
