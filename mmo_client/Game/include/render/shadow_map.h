#ifndef SHADOW_MAP_H
#define SHADOW_MAP_H

// The depth textures the sun's shadows are drawn into, and the small shader
// that draws into them. Two maps: a close one that is sharp near the player
// and a wide one that reaches far across the view.

// GL names the depth textures need, which the loader does not carry
#ifndef GL_TEXTURE_COMPARE_MODE
#define GL_TEXTURE_COMPARE_MODE 0x884C
#endif
#ifndef GL_TEXTURE_COMPARE_FUNC
#define GL_TEXTURE_COMPARE_FUNC 0x884D
#endif
#ifndef GL_COMPARE_REF_TO_TEXTURE
#define GL_COMPARE_REF_TO_TEXTURE 0x884E
#endif
#ifndef GL_POLYGON_OFFSET_FILL
#define GL_POLYGON_OFFSET_FILL 0x8037
#endif
#ifndef GL_NONE
#define GL_NONE 0
#endif

#define SHADOW_CASCADES 2
#define SHADOW_MAP_SIZE 2048

// Make the depth program, textures and framebuffers. Needs gl_loader_init().
// Returns nonzero on success; failures are logged, and the world then has
// no shadows.
int shadow_map_init(void);
void shadow_map_shutdown(void);

// Whether the maps can be drawn into
int shadow_map_ready(void);

// Start drawing casters into a map: its framebuffer is bound and cleared, and
// the depth program is in use. Pair with shadow_map_end().
void shadow_map_begin(int cascade);

// The same for any depth-only framebuffer, drawing into the rectangle
// (x, y, w, h) of it, which is cleared first and is all that can be drawn on.
void shadow_map_begin_target(unsigned int fbo, int x, int y, int w, int h);

// The light-space matrix (column-major) for the next caster draw
void shadow_map_set_mvp(const float mvp[16]);

// Put the framebuffer, viewport and state back as shadow_map_begin found them
void shadow_map_end(void);

// The depth texture of a map, to sample from
unsigned int shadow_map_texture(int cascade);

#endif /* SHADOW_MAP_H */
