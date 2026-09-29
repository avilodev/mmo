/**
 * @file
 * Upload the city scene once and draw the cells of it that can be seen.
 */

#include "render/gl_loader.h"
#include "render/city_renderer.h"
#include "render/world_light.h"
#include "model/obj_mesh.h"
#include "camera/cglm_config.h"
#include "core/client_log.h"
#include "utils/stb_image.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

static const char* VERTEX_SRC =
    "#version 330 core\n"
    "layout(location = 0) in vec3 a_pos;\n"
    "layout(location = 1) in vec3 a_normal;\n"
    "layout(location = 2) in vec2 a_uv;\n"
    "uniform mat4 u_mvp;\n"
    "uniform mat4 u_model;\n"
    "out vec3 v_normal;\n"
    "out vec2 v_uv;\n"
    "out vec2 v_world;\n"
    "void main() {\n"
    "    v_normal = mat3(u_model) * a_normal;\n"
    "    v_world = (u_model * vec4(a_pos, 1.0)).xz;\n"
    "    v_uv = a_uv;\n"
    "    gl_Position = u_mvp * vec4(a_pos, 1.0);\n"
    "}\n";

/* The same half-Lambert sun, scale and fog as model_renderer.c, so the city
 * and the people walking through it are lit alike. */
static const char* FRAGMENT_SRC =
    "#version 330 core\n"
    "in vec3 v_normal;\n"
    "in vec2 v_uv;\n"
    "in vec2 v_world;\n"
    "uniform vec2 u_fog_center;\n"
    "uniform vec2 u_fog_range;\n"
    "uniform vec3 u_fog_color;\n"
    "uniform vec3 u_color;\n"
    "uniform vec3 u_sun;\n"
    "uniform float u_ambient;\n"
    "uniform float u_light_scale;\n"
    "uniform int u_use_tex;\n"
    "uniform sampler2D u_tex;\n"
    "out vec4 frag;\n"
    "void main() {\n"
    "    vec3 n = normalize(v_normal);\n"
    "    if (!gl_FrontFacing) n = -n;\n"
    "    float wrap = dot(n, u_sun) * 0.5 + 0.5;\n"
    "    float light = u_ambient + (1.0 - u_ambient) * wrap * wrap;\n"
    "    vec3 c = u_color;\n"
    "    if (u_use_tex == 1) c *= texture(u_tex, v_uv).rgb;\n"
    "    float f = smoothstep(u_fog_range.x, u_fog_range.y, distance(v_world, u_fog_center));\n"
    "    frag = vec4(mix(c * light * u_light_scale, u_fog_color, f), 1.0);\n"
    "}\n";

static GLuint g_program, g_vao, g_vbo, g_ibo;
static GLuint g_textures[OBJ_MAX_MATERIALS];
static float  g_colors[OBJ_MAX_MATERIALS][3];
static int    g_material_count;
static GLint  u_mvp, u_model, u_color, u_sun, u_ambient, u_light_scale, u_use_tex, u_tex;
static GLint  u_fog_center, u_fog_range, u_fog_color;

/* Only the batch table stays on the CPU; the geometry lives on the GPU. */
static ObjBatch* g_batches;
static int       g_batch_count;

static GLuint compile(GLenum type, const char* src) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);

    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(shader, (GLsizei)sizeof(log), NULL, log);
        CLOG_ERROR("[CITY] %s shader failed to compile: %s",
                   type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static int build_program(void) {
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
        CLOG_ERROR("[CITY] Shader program failed to link: %s", log);
        glDeleteProgram(g_program);
        g_program = 0;
        return 0;
    }

    u_mvp         = glGetUniformLocation(g_program, "u_mvp");
    u_model       = glGetUniformLocation(g_program, "u_model");
    u_color       = glGetUniformLocation(g_program, "u_color");
    u_sun         = glGetUniformLocation(g_program, "u_sun");
    u_ambient     = glGetUniformLocation(g_program, "u_ambient");
    u_light_scale = glGetUniformLocation(g_program, "u_light_scale");
    u_use_tex     = glGetUniformLocation(g_program, "u_use_tex");
    u_tex         = glGetUniformLocation(g_program, "u_tex");
    u_fog_center  = glGetUniformLocation(g_program, "u_fog_center");
    u_fog_range   = glGetUniformLocation(g_program, "u_fog_range");
    u_fog_color   = glGetUniformLocation(g_program, "u_fog_color");
    return 1;
}

/** A material's map_Kd, mipmapped. 0 (drawn in its flat colour) if missing. */
static GLuint load_texture(const char* path) {
    if (!path[0]) return 0;
    int w, h, channels;
    unsigned char* rgb = stbi_load(path, &w, &h, &channels, 3);
    if (!rgb) {
        CLOG_WARN("[CITY] Texture %s did not load: %s", path, stbi_failure_reason());
        return 0;
    }
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, rgb);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glGenerateMipmap(GL_TEXTURE_2D);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, 0);
    stbi_image_free(rgb);
    return tex;
}

static void upload(const ObjMesh* mesh) {
    glGenVertexArrays(1, &g_vao);
    glBindVertexArray(g_vao);

    glGenBuffers(1, &g_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 (mmo_GLsizeiptr)((size_t)mesh->vertex_count * sizeof(ObjVertex)),
                 mesh->vertices, GL_STATIC_DRAW);
    const GLsizei stride = (GLsizei)sizeof(ObjVertex);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (const void*)offsetof(ObjVertex, x));
    glVertexAttribPointer(1, 3, GL_BYTE, GL_TRUE, stride, (const void*)offsetof(ObjVertex, nx));
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (const void*)offsetof(ObjVertex, u));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);

    /* The element buffer binding is part of the VAO's state. */
    glGenBuffers(1, &g_ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 (mmo_GLsizeiptr)((size_t)mesh->index_count * sizeof(uint32_t)),
                 mesh->indices, GL_STATIC_DRAW);

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    g_material_count = mesh->material_count;
    for (int i = 0; i < mesh->material_count; i++) {
        g_textures[i] = load_texture(mesh->materials[i].texture);
        memcpy(g_colors[i], mesh->materials[i].diffuse, sizeof(g_colors[i]));
    }
}

int city_renderer_init(void) {
    const float cell = CITY_CELL_UNITS / CITY_UNITS_PER_METRE;
    ObjMesh mesh;
    char error[256] = "";
    int from_cache = 0;
    double start = glfwGetTime();
    if (!obj_mesh_load_cached(&mesh, CITY_OBJ_PATH, CITY_CACHE_PATH, cell, &from_cache,
                              error, (int)sizeof(error))) {
        CLOG_ERROR("[CITY] Could not load %s: %s", CITY_OBJ_PATH, error);
        return 0;
    }
    double loaded = glfwGetTime();

    if (!build_program()) {
        obj_mesh_free(&mesh);
        return 0;
    }
    upload(&mesh);

    /* Keep the batch table; hand the rest back. */
    g_batches = mesh.batches;
    g_batch_count = mesh.batch_count;
    mesh.batches = NULL;
    uint32_t tris = mesh.index_count / 3, verts = mesh.vertex_count;
    obj_mesh_free(&mesh);

    CLOG_INFO("[CITY] Ready: %u triangles, %u vertices, %d cells; %s in %.2f s, "
              "uploaded in %.2f s", tris, verts, g_batch_count,
              from_cache ? "read from cache" : "parsed", loaded - start,
              glfwGetTime() - loaded);
    return 1;
}

void city_renderer_shutdown(void) {
    for (int i = 0; i < g_material_count; i++)
        if (g_textures[i]) glDeleteTextures(1, &g_textures[i]);
    memset(g_textures, 0, sizeof(g_textures));
    g_material_count = 0;
    if (g_ibo)     { glDeleteBuffers(1, &g_ibo);         g_ibo = 0; }
    if (g_vbo)     { glDeleteBuffers(1, &g_vbo);         g_vbo = 0; }
    if (g_vao)     { glDeleteVertexArrays(1, &g_vao);    g_vao = 0; }
    if (g_program) { glDeleteProgram(g_program);         g_program = 0; }
    free(g_batches);
    g_batches = NULL;
    g_batch_count = 0;
}

/** Whether a world-space box is at least partly inside the frustum of a
 *  clip-from-world matrix (column-major), tested against each plane. */
static int box_in_frustum(const float m[16], const float lo[3], const float hi[3]) {
    for (int p = 0; p < 6; p++) {
        int row = p / 2;
        float sign = (p % 2) ? -1.0f : 1.0f;
        /* Plane = row 3 +/- row `row` of the matrix. */
        float a = m[3]  + sign * m[row];
        float b = m[7]  + sign * m[4 + row];
        float c = m[11] + sign * m[8 + row];
        float d = m[15] + sign * m[12 + row];
        /* The box corner furthest along the plane's normal. */
        float x = a >= 0.0f ? hi[0] : lo[0];
        float y = b >= 0.0f ? hi[1] : lo[1];
        float z = c >= 0.0f ? hi[2] : lo[2];
        if (a * x + b * y + c * z + d < 0.0f) return 0;
    }
    return 1;
}

/** Ground distance from (x, z) to the nearest point of a box. */
static float ground_distance(float x, float z, const float lo[3], const float hi[3]) {
    float dx = x < lo[0] ? lo[0] - x : (x > hi[0] ? x - hi[0] : 0.0f);
    float dz = z < lo[2] ? lo[2] - z : (z > hi[2] ? z - hi[2] : 0.0f);
    return sqrtf(dx * dx + dz * dz);
}

void city_renderer_draw(const CameraView* view, float focus_x, float focus_y) {
    if (!g_program || g_batch_count == 0) return;

    const float s = CITY_UNITS_PER_METRE;
    mat4 model, mvp;
    glm_mat4_identity(model);
    glm_translate(model, (vec3){ CITY_ORIGIN_X, 0.0f, CITY_ORIGIN_Y });
    glm_scale_uni(model, s);
    glm_mat4_mul((vec4*)view->viewproj, model, mvp);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    /* Blender scenes are full of single-sided planes; draw both sides. */
    glDisable(GL_CULL_FACE);
    GLboolean blend_was_on = glIsEnabled(GL_BLEND);
    glDisable(GL_BLEND);

    glUseProgram(g_program);
    glBindVertexArray(g_vao);
    glUniformMatrix4fv(u_mvp, 1, GL_FALSE, (const float*)mvp);
    glUniformMatrix4fv(u_model, 1, GL_FALSE, (const float*)model);
    float len = sqrtf(WORLD_SUN_X * WORLD_SUN_X + WORLD_SUN_Y * WORLD_SUN_Y +
                      WORLD_SUN_Z * WORLD_SUN_Z);
    glUniform3f(u_sun, WORLD_SUN_X / len, WORLD_SUN_Y / len, WORLD_SUN_Z / len);
    glUniform1f(u_ambient, WORLD_AMBIENT);
    float up = 0.5f * WORLD_SUN_Y / len + 0.5f;
    glUniform1f(u_light_scale, 1.0f / (WORLD_AMBIENT + (1.0f - WORLD_AMBIENT) * up * up));
    glUniform1i(u_tex, 0);
    glUniform2f(u_fog_center, focus_x, focus_y);
    glUniform2f(u_fog_range, WORLD_FOG_START, WORLD_FOG_END);
    glUniform3f(u_fog_color, WORLD_SKY_R, WORLD_SKY_G, WORLD_SKY_B);
    glActiveTexture(GL_TEXTURE0);

    int bound = -1;
    for (int i = 0; i < g_batch_count; i++) {
        const ObjBatch* b = &g_batches[i];
        const float lo[3] = { CITY_ORIGIN_X + b->min[0] * s, b->min[1] * s,
                              CITY_ORIGIN_Y + b->min[2] * s };
        const float hi[3] = { CITY_ORIGIN_X + b->max[0] * s, b->max[1] * s,
                              CITY_ORIGIN_Y + b->max[2] * s };
        if (ground_distance(focus_x, focus_y, lo, hi) > WORLD_FOG_END) continue;
        if (!box_in_frustum(view->viewproj, lo, hi)) continue;

        if (b->material != bound) {
            bound = b->material;
            GLuint tex = g_textures[bound];
            glUniform1i(u_use_tex, tex ? 1 : 0);
            if (tex) glBindTexture(GL_TEXTURE_2D, tex);
            glUniform3f(u_color, g_colors[bound][0], g_colors[bound][1], g_colors[bound][2]);
        }
        glDrawElements(GL_TRIANGLES, (GLsizei)b->count, GL_UNSIGNED_INT,
                       (const void*)((size_t)b->first * sizeof(uint32_t)));
    }

    glBindVertexArray(0);
    glUseProgram(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_DEPTH_TEST);
    if (blend_was_on) glEnable(GL_BLEND);
}
