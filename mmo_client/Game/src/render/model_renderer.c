/**
 * @file
 * Upload skinned models and draw them with a lit GLSL 330 shader.
 */

#include "render/gl_loader.h"
#include "render/model_renderer.h"
#include "render/world_light.h"
#include "world/sky.h"
#include "render/scene_light.h"
#include "render/shadow_map.h"
#include "core/client_log.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>

static const char* VERTEX_SRC =
	"#version 330 core\n"
	"layout(location = 0) in vec3 a_pos;\n"
	"layout(location = 1) in vec3 a_normal;\n"
	"layout(location = 2) in vec2 a_uv;\n"
	"uniform mat4 u_mvp;\n"
	"uniform mat4 u_model;\n"
	"out vec3 v_normal;\n"
	"out vec2 v_uv;\n"
	"out vec3 v_wpos;\n"
	"void main() {\n"
	"    v_normal = mat3(u_model) * a_normal;\n"
	"    v_wpos = (u_model * vec4(a_pos, 1.0)).xyz;\n"
	"    v_uv = a_uv;\n"
	"    gl_Position = u_mvp * vec4(a_pos, 1.0);\n"
	"}\n";

// The light comes from scene_light.h, shared with the city, so a body is lit
// exactly as the walls around it are.
static const char* FRAGMENT_HEAD =
	"#version 330 core\n"
	"in vec3 v_normal;\n"
	"in vec2 v_uv;\n"
	"in vec3 v_wpos;\n"
	"uniform vec3 u_color;\n"
	"uniform int u_use_tex;\n"
	"uniform sampler2D u_tex;\n"
	"out vec4 frag;\n";

static const char* FRAGMENT_MAIN =
	"void main() {\n"
	"    vec3 n = normalize(v_normal);\n"
	"    vec3 c = u_color;\n"
	"    if (u_use_tex == 1) {\n"
	"        vec4 t = texture(u_tex, v_uv);\n"
	"        if (t.a < 0.5) discard;\n"
	"        c *= t.rgb;\n"
	"    }\n"
	"    frag = vec4(scene_fog(c * scene_light(n, v_wpos), v_wpos), 1.0);\n"
	"}\n";

static GLuint    g_program;
static GLuint    g_vao;
static GLboolean g_blend_was_on;   /* restored by model_renderer_end() */
static GLint  u_mvp, u_model, u_color, u_use_tex, u_tex;
static scene_light_uniforms g_light;

static GLuint compile(GLenum type, const char* const* parts, int part_count) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, part_count, parts, NULL);
    glCompileShader(shader);

    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(shader, (GLsizei)sizeof(log), NULL, log);
        CLOG_ERROR("[MODEL] %s shader failed to compile: %s",
                   type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

int model_renderer_init(void) {
    const char* const vertex_parts[] = { VERTEX_SRC };
    const char* const fragment_parts[] = { FRAGMENT_HEAD, scene_light_glsl(), FRAGMENT_MAIN };
    GLuint vs = compile(GL_VERTEX_SHADER, vertex_parts, 1);
    GLuint fs = compile(GL_FRAGMENT_SHADER, fragment_parts, 3);
    if (!vs || !fs) {
        if (vs) 
            glDeleteShader(vs);
        if (fs) 
            glDeleteShader(fs);
        return 0;
    }

    g_program = glCreateProgram();
    glAttachShader(g_program, vs);
    glAttachShader(g_program, fs);
    glLinkProgram(g_program);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(g_program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(g_program, (GLsizei)sizeof(log), NULL, log);
        CLOG_ERROR("[MODEL] Shader program failed to link: %s", log);
        glDeleteProgram(g_program);
        g_program = 0;
        return 0;
    }

    u_mvp     = glGetUniformLocation(g_program, "u_mvp");
    u_model   = glGetUniformLocation(g_program, "u_model");
    u_color   = glGetUniformLocation(g_program, "u_color");
    u_use_tex = glGetUniformLocation(g_program, "u_use_tex");
    u_tex     = glGetUniformLocation(g_program, "u_tex");
    scene_light_locate(g_program, &g_light);

    glGenVertexArrays(1, &g_vao);
    CLOG_INFO("[MODEL] Shader ready");
    return 1;
}

void model_renderer_shutdown(void) {
    if (g_vao) { 
        glDeleteVertexArrays(1, &g_vao); 
        g_vao = 0; 
    }

    if (g_program) { 
        glDeleteProgram(g_program);       
        g_program = 0; 
    }
}

static GLuint upload_static(GLenum target, const void* data, size_t bytes) {
    GLuint name = 0;
    glGenBuffers(1, &name);
    glBindBuffer(target, name);
    glBufferData(target, (mmo_GLsizeiptr)bytes, data, GL_STATIC_DRAW);
    glBindBuffer(target, 0);
    return name;
}

static GLuint upload_texture(const unsigned char* rgba, int w, int h) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glGenerateMipmap(GL_TEXTURE_2D);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

void model_renderer_upload(SkinnedModel* model) {
    for (int i = 0; i < model->mesh_count; i++) {
        SkinnedMesh* mesh = &model->meshes[i];
        if (!mesh->index_buffer && mesh->indices && mesh->index_count > 0)
            mesh->index_buffer = upload_static(GL_ELEMENT_ARRAY_BUFFER, mesh->indices,
                                               (size_t)mesh->index_count * sizeof(*mesh->indices));
        if (!mesh->uv_buffer && mesh->uvs)
            mesh->uv_buffer = upload_static(GL_ARRAY_BUFFER, mesh->uvs,
                                            (size_t)mesh->vertex_count * 2 * sizeof(float));
        if (!mesh->texture && mesh->image) {
            mesh->texture = upload_texture(mesh->image, mesh->image_width, mesh->image_height);
            /* On the GPU now; the decoded copy would be dead weight. */
            free(mesh->image);
            mesh->image = NULL;
        }
    }
}

void model_renderer_release_model(SkinnedModel* model) {
    for (int i = 0; i < model->mesh_count; i++) {
        SkinnedMesh* mesh = &model->meshes[i];
        if (mesh->index_buffer) 
            glDeleteBuffers(1, &mesh->index_buffer);
        if (mesh->uv_buffer)    
            glDeleteBuffers(1, &mesh->uv_buffer);
        if (mesh->texture)      
            glDeleteTextures(1, &mesh->texture);
        mesh->index_buffer = mesh->uv_buffer = mesh->texture = 0;
    }
}

void model_renderer_release_instance(SkinnedInstance* instance) {
    for (int i = 0; i < instance->mesh_count; i++) {
        if (instance->meshes[i].vertex_buffer)
            glDeleteBuffers(1, &instance->meshes[i].vertex_buffer);
        instance->meshes[i].vertex_buffer = 0;
    }
}

void model_renderer_begin(void) {
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    g_blend_was_on = glIsEnabled(GL_BLEND);
    glDisable(GL_BLEND);

    glUseProgram(g_program);
    glBindVertexArray(g_vao);
    scene_light_apply(&g_light);
    glUniform1i(u_tex, 0);
    glActiveTexture(GL_TEXTURE0);
}

void model_renderer_draw(const SkinnedModel* model, SkinnedInstance* instance,
                         const float mvp[16], const float model_mat[16],
                         const float tint[3]) {
    if (!g_program) 
        return;

    glUniformMatrix4fv(u_mvp, 1, GL_FALSE, mvp);
    glUniformMatrix4fv(u_model, 1, GL_FALSE, model_mat);

    const GLsizei stride = (GLsizei)(SKIN_VERTEX_FLOATS * sizeof(float));
    int meshes = model->mesh_count < instance->mesh_count ? model->mesh_count
                                                          : instance->mesh_count;
    for (int i = 0; i < meshes; i++) {
        const SkinnedMesh* mesh = &model->meshes[i];
        SkinnedMeshVertices* verts = &instance->meshes[i];
        if (!mesh->index_buffer || !verts->vertices || verts->vertex_count <= 0) 
            continue;

        /* Orphan-and-refill: the driver hands back fresh storage instead of
         * waiting for last frame's draw to finish reading the old. */
        if (!verts->vertex_buffer) 
            glGenBuffers(1, &verts->vertex_buffer);
        glBindBuffer(GL_ARRAY_BUFFER, verts->vertex_buffer);
        glBufferData(GL_ARRAY_BUFFER,
                     (mmo_GLsizeiptr)((size_t)verts->vertex_count * SKIN_VERTEX_FLOATS * sizeof(float)),
                     verts->vertices, GL_STREAM_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (const void*)0);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (const void*)(3 * sizeof(float)));
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);

        int textured = mesh->texture && mesh->uv_buffer;
        if (textured) {
            glBindBuffer(GL_ARRAY_BUFFER, mesh->uv_buffer);
            glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 0, (const void*)0);
            glEnableVertexAttribArray(2);
            glBindTexture(GL_TEXTURE_2D, mesh->texture);
        } else {
            glDisableVertexAttribArray(2);
        }
        glUniform1i(u_use_tex, textured ? 1 : 0);

		// A thin part (hair, cloth) is seen from both sides
		if(mesh->double_sided)
			glDisable(GL_CULL_FACE);
		else
			glEnable(GL_CULL_FACE);

		// With a tint: the tint, a darker shade of it, or the material's own
		// colour, by the loader's rule. With none: the material's own colour
		if(tint && mesh->tint_shade > 0.0f)
			glUniform3f(u_color, tint[0] * mesh->tint_shade, tint[1] * mesh->tint_shade,
			            tint[2] * mesh->tint_shade);
		else
			glUniform3f(u_color, mesh->base_color[0], mesh->base_color[1],
			            mesh->base_color[2]);

        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh->index_buffer);
        glDrawElements(GL_TRIANGLES, mesh->index_count, GL_UNSIGNED_INT, (const void*)0);
    }
}

void model_renderer_depth_begin(void) {
	glBindVertexArray(g_vao);
	glDisableVertexAttribArray(1);
	glDisableVertexAttribArray(2);
	glEnableVertexAttribArray(0);
}

void model_renderer_draw_depth(const SkinnedModel* model, const SkinnedInstance* instance,
                               const float mvp[16]) {
	const GLsizei stride = (GLsizei)(SKIN_VERTEX_FLOATS * sizeof(float));
	int meshes = model->mesh_count < instance->mesh_count ? model->mesh_count
	                                                      : instance->mesh_count;

	shadow_map_set_mvp(mvp);
	for(int i = 0; i < meshes; i++) {
		const SkinnedMesh* mesh = &model->meshes[i];
		const SkinnedMeshVertices* verts = &instance->meshes[i];

		// The vertices the last real draw streamed; none yet means nothing to cast
		if(!mesh->index_buffer || !verts->vertex_buffer || verts->vertex_count <= 0)
			continue;
		glBindBuffer(GL_ARRAY_BUFFER, verts->vertex_buffer);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (const void*)0);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh->index_buffer);
		glDrawElements(GL_TRIANGLES, mesh->index_count, GL_UNSIGNED_INT, (const void*)0);
	}
}

void model_renderer_depth_end(void) {
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindVertexArray(0);
}

void model_renderer_end(void) {
    glDisableVertexAttribArray(2);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
    glUseProgram(0);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    if (g_blend_was_on) 
        glEnable(GL_BLEND);
}
