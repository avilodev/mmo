/**
 * @file
 * Compile the ground shader and draw chunk layers from vertex buffers.
 */

#include "render/gl_loader.h"
#include "render/ground_renderer.h"
#include "render/world_light.h"
#include "core/client_log.h"

#include <stddef.h>

static const char* VERTEX_SRC =
    "#version 330 core\n"
    "layout(location = 0) in vec3 a_pos;\n"
    "layout(location = 1) in vec2 a_uv;\n"
    "layout(location = 2) in vec4 a_color;\n"
    "uniform mat4 u_mvp;\n"
    "out vec2 v_uv;\n"
    "out vec4 v_color;\n"
    "out vec2 v_world;\n"
    "void main() {\n"
    "    v_uv = a_uv;\n"
    "    v_color = a_color;\n"
    "    v_world = a_pos.xy;\n"
    "    gl_Position = u_mvp * vec4(a_pos, 1.0);\n"
    "}\n";

static const char* FRAGMENT_SRC =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "in vec4 v_color;\n"
    "in vec2 v_world;\n"
    "uniform sampler2D u_tex;\n"
    "uniform int   u_use_tex;\n"
    "uniform vec2  u_fog_center;\n"
    "uniform vec2  u_fog_range;\n"
    "uniform vec3  u_fog_color;\n"
    "out vec4 frag;\n"
    "void main() {\n"
    "    vec4 c = v_color;\n"
    "    if (u_use_tex == 1) c *= texture(u_tex, v_uv);\n"
    "    if (c.a <= 0.0) discard;\n"
    "    float f = smoothstep(u_fog_range.x, u_fog_range.y, distance(v_world, u_fog_center));\n"
    "    c.rgb = mix(c.rgb, u_fog_color, f);\n"
    "    frag = c;\n"
    "}\n";

static GLuint g_program;
static GLuint g_vao;
static GLint  u_mvp, u_tex, u_use_tex, u_fog_center, u_fog_range, u_fog_color;

static GLuint compile(GLenum type, const char* src) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);

    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(shader, (GLsizei)sizeof(log), NULL, log);
        CLOG_ERROR("[GROUND] %s shader failed to compile: %s",
                   type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

int ground_renderer_init(void) {
    GLuint vs = compile(GL_VERTEX_SHADER, VERTEX_SRC);
    GLuint fs = compile(GL_FRAGMENT_SHADER, FRAGMENT_SRC);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
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
        CLOG_ERROR("[GROUND] Shader program failed to link: %s", log);
        glDeleteProgram(g_program);
        g_program = 0;
        return 0;
    }

    u_mvp         = glGetUniformLocation(g_program, "u_mvp");
    u_tex         = glGetUniformLocation(g_program, "u_tex");
    u_use_tex     = glGetUniformLocation(g_program, "u_use_tex");
    u_fog_center  = glGetUniformLocation(g_program, "u_fog_center");
    u_fog_range   = glGetUniformLocation(g_program, "u_fog_range");
    u_fog_color   = glGetUniformLocation(g_program, "u_fog_color");

    glGenVertexArrays(1, &g_vao);
    CLOG_INFO("[GROUND] Shader ready");
    return 1;
}

void ground_renderer_shutdown(void) {
    if (g_vao)     { glDeleteVertexArrays(1, &g_vao); g_vao = 0; }
    if (g_program) { glDeleteProgram(g_program);       g_program = 0; }
}

void ground_renderer_upload(ChunkLayerGpu* layer, const ChunkMesh* mesh) {
    layer->vert_count  = mesh->vert_count;
    layer->range_count = mesh->range_count;
    for (int i = 0; i < mesh->range_count; i++) layer->ranges[i] = mesh->ranges[i];

    if (mesh->vert_count == 0) return;   /* keep any buffer for reuse; nothing draws */
    if (!layer->vbo) glGenBuffers(1, &layer->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, layer->vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 (mmo_GLsizeiptr)((size_t)mesh->vert_count * sizeof(ChunkVertex)),
                 mesh->verts, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void ground_renderer_release(ChunkLayerGpu* layer) {
    if (layer->vbo) glDeleteBuffers(1, &layer->vbo);
    layer->vbo         = 0;
    layer->vert_count  = 0;
    layer->range_count = 0;
}

void ground_renderer_begin(const CameraView* view, int solid, float fog_x, float fog_y) {
    if (solid) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        glDepthMask(GL_TRUE);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    glUseProgram(g_program);
    glBindVertexArray(g_vao);
    glUniformMatrix4fv(u_mvp, 1, GL_FALSE, view->ground_mvp);
    glUniform1i(u_tex, 0);
    glUniform2f(u_fog_center, fog_x, fog_y);
    glUniform2f(u_fog_range, WORLD_FOG_START, WORLD_FOG_END);
    glUniform3f(u_fog_color, WORLD_SKY_R, WORLD_SKY_G, WORLD_SKY_B);
    glActiveTexture(GL_TEXTURE0);
}

void ground_renderer_draw(const ChunkLayerGpu* layer, const unsigned int* tileset_textures) {
    if (!layer->vbo || layer->vert_count == 0) return;

    glBindBuffer(GL_ARRAY_BUFFER, layer->vbo);
    const GLsizei stride = (GLsizei)sizeof(ChunkVertex);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (const void*)offsetof(ChunkVertex, x));
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (const void*)offsetof(ChunkVertex, u));
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, (const void*)offsetof(ChunkVertex, r));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);

    for (int i = 0; i < layer->range_count; i++) {
        const ChunkDrawRange* range = &layer->ranges[i];
        unsigned int tex = range->tileset > 0 ? tileset_textures[range->tileset] : 0;
        glUniform1i(u_use_tex, tex ? 1 : 0);
        if (tex) glBindTexture(GL_TEXTURE_2D, tex);
        glDrawArrays(GL_TRIANGLES, range->first, range->count);
    }
}

void ground_renderer_end(void) {
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
    glUseProgram(0);
    glDisable(GL_DEPTH_TEST);
}
