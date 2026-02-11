#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "texture.h"
#include <windows.h>
#include <GL/gl.h>
#include <GLFW/glfw3.h>
#include <stdio.h>
#include <stdlib.h>


unsigned int texture_load(const char* filepath) {
    unsigned int texture_id;
    int width, height, channels;
    
    unsigned char* data = stbi_load(filepath, &width, &height, &channels, 0);
    
    if (!data) {
        fprintf(stderr, "Failed to load texture: %s\n", filepath);
        fprintf(stderr, "STB Error: %s\n", stbi_failure_reason());
        return 0;
    }
    
    printf("Loaded texture: %s (%dx%d, %d channels)\n", 
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

void texture_unload(unsigned int texture_id) {
    if (texture_id != 0) {
        glDeleteTextures(1, &texture_id);
        printf("Unloaded texture ID: %u\n", texture_id);
    }
}

void texture_get_size(unsigned int texture_id, int* width, int* height) {
    glBindTexture(GL_TEXTURE_2D, texture_id);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, width);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, height);
}