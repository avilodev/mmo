#ifndef GROUND_RENDERER_H
#define GROUND_RENDERER_H

/**
 * @file
 * Draw the world's chunk geometry from GPU vertex buffers with one small shader.
 *
 * Two kinds of chunk geometry share it: the flat ground (every vertex at
 * height 0) and the raised structures of structure_mesh.h, whose lighting is
 * baked into their vertex colours. Only the depth rules differ.
 */

#include "camera/camera_math.h"
#include "world/chunk_mesh.h"

/** Compile the shader and create the vertex array. Needs gl_loader_init().
 *  @return Nonzero on success; failures are logged. */
int  ground_renderer_init(void);
void ground_renderer_shutdown(void);

/** Upload a built mesh into a layer's vertex buffer, replacing what it held. */
void ground_renderer_upload(ChunkLayerGpu* layer, const ChunkMesh* mesh);

/** Free a layer's vertex buffer. Safe on a layer that was never uploaded. */
void ground_renderer_release(ChunkLayerGpu* layer);

/** Start drawing chunk geometry under a camera. Pair with ground_renderer_end().
 *
 * @param solid  Zero for the ground: drawn in order, no depth test or write
 *               (D12). Nonzero for structures: depth-tested and written, so
 *               what is drawn after is hidden behind them.
 * @param fog_x, fog_y  The point the camera orbits; the world fades into the
 *               sky with distance from it (world_light.h).
 */
void ground_renderer_begin(const CameraView* view, int solid, float fog_x, float fog_y);

/** Draw one uploaded layer, binding each tileset texture its ranges need. */
void ground_renderer_draw(const ChunkLayerGpu* layer, const unsigned int* tileset_textures);

/** Restore the fixed-function state the rest of the client draws with. */
void ground_renderer_end(void);

#endif /* GROUND_RENDERER_H */
