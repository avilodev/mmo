#ifndef GROUND_RENDERER_H
#define GROUND_RENDERER_H

/**
 * @file
 * Draw chunk tile layers from GPU vertex buffers with one small shader.
 *
 * This replaced per-chunk display lists (Next_steps/3d_refactor.md, step 7).
 * The shader also does the two things the layers need per pixel: fading the
 * roof layer around an indoor player, and splitting the roof layer at the
 * player's row for the top-down camera.
 */

#include "camera/camera_math.h"
#include "world/chunk_mesh.h"

/** Which part of a layer a draw keeps, by world y. */
typedef enum {
    GROUND_SPLIT_NONE = 0,
    GROUND_SPLIT_NORTH,    /**< Keep y < split_y. */
    GROUND_SPLIT_SOUTH     /**< Keep y >= split_y. */
} GroundSplit;

/** Per-draw options; zero-initialized means "draw everything, opaque". */
typedef struct {
    GroundSplit split;
    float       split_y;
    int         fade;                    /**< Fade out around (fade_x, fade_y). */
    float       fade_x, fade_y;
    float       fade_inner, fade_outer;  /**< Hidden inside inner, full past outer. */
} GroundPassOptions;

/** Compile the shader and create the vertex array. Needs gl_loader_init().
 *  @return Nonzero on success; failures are logged. */
int  ground_renderer_init(void);
void ground_renderer_shutdown(void);

/** Upload a built mesh into a layer's vertex buffer, replacing what it held. */
void ground_renderer_upload(ChunkLayerGpu* layer, const ChunkMesh* mesh);

/** Free a layer's vertex buffer. Safe on a layer that was never uploaded. */
void ground_renderer_release(ChunkLayerGpu* layer);

/** Start drawing ground under a camera. Pair with ground_renderer_end(). */
void ground_renderer_begin(const CameraView* view);

void ground_renderer_set_options(const GroundPassOptions* options);

/** Draw one uploaded layer, binding each tileset texture its ranges need. */
void ground_renderer_draw(const ChunkLayerGpu* layer, const unsigned int* tileset_textures);

/** Restore the fixed-function state the rest of the client draws with. */
void ground_renderer_end(void);

#endif /* GROUND_RENDERER_H */
