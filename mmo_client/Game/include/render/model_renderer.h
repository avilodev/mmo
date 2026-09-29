#ifndef MODEL_RENDERER_H
#define MODEL_RENDERER_H

/**
 * @file
 * Draw skinned models with one small lit shader.
 *
 * The GL half of model/skinned_model.h: it owns every GL name a SkinnedModel
 * or SkinnedInstance carries. Loading and skinning stay GL-free so they are
 * tested headless; this only uploads and draws what they produced.
 */

#include "model/skinned_model.h"

/** Compile the shader. Needs gl_loader_init().
 *  @return Nonzero on success; failures are logged. */
int  model_renderer_init(void);
void model_renderer_shutdown(void);

/** Put a model's shared geometry (indices, UVs, textures) on the GPU. */
void model_renderer_upload(SkinnedModel* model);

/** Delete the GL names a model holds; call before skinned_model_free(). */
void model_renderer_release_model(SkinnedModel* model);

/** Delete the GL names an instance holds; call before skinned_instance_free(). */
void model_renderer_release_instance(SkinnedInstance* instance);

/** Set up depth, culling and the shader for a run of model draws. Bodies fade
 *  into the sky with distance from (fog_x, fog_y), as the world does. */
void model_renderer_begin(float fog_x, float fog_y);

/** Draw one body. Streams this frame's skinned vertices to the GPU first.
 *
 * @param mvp       Column-major clip-from-model matrix.
 * @param model_mat Column-major world-from-model matrix (rotation and uniform
 *                  scale only matter; it orients the normals).
 * @param tint      Colour of the model's first material; later materials take
 *                  a darker shade of it.
 */
void model_renderer_draw(const SkinnedModel* model, SkinnedInstance* instance,
                         const float mvp[16], const float model_mat[16],
                         const float tint[3]);

/** Restore the fixed-function state the rest of the client draws with. */
void model_renderer_end(void);

#endif /* MODEL_RENDERER_H */
