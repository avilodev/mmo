#ifndef TEXTURE_H
#define TEXTURE_H

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

// Load a texture from file and return OpenGL texture ID
// Returns 0 if loading fails
unsigned int texture_load(const char* filepath);

// Unload a texture and free GPU memory
void texture_unload(unsigned int texture_id);

// Get texture dimensions (optional, for checking size)
void texture_get_size(unsigned int texture_id, int* width, int* height);

#endif // TEXTURE_H