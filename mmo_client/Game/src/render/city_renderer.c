/**
 * @file
 * Upload the city scene once and draw the cells of it that can be seen.
 */

#include "render/gl_loader.h"
#include "render/city_renderer.h"
#include "render/world_light.h"
#include "world/sky.h"
#include "world/city_lamps.h"
#include "render/scene_light.h"
#include "render/shadow_map.h"
#include "render/cover_map.h"
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
	"out vec3 v_wpos;\n"
	"void main() {\n"
	"    vec4 w = u_model * vec4(a_pos, 1.0);\n"
	"    v_normal = mat3(u_model) * a_normal;\n"
	"    v_wpos = w.xyz;\n"
	"    v_uv = a_uv;\n"
	"    gl_Position = u_mvp * vec4(a_pos, 1.0);\n"
	"}\n";

// The light comes from scene_light.h, shared with the characters, so the city
// and the people walking through it are lit alike.
static const char* FRAGMENT_HEAD =
	"#version 330 core\n"
	"in vec3 v_normal;\n"
	"in vec2 v_uv;\n"
	"in vec3 v_wpos;\n"
	"uniform float u_clip_y;\n"
	"uniform vec3 u_color;\n"
	"uniform int u_use_tex;\n"
	"uniform sampler2D u_tex;\n"
	"out vec4 frag;\n";

static const char* FRAGMENT_MAIN =
	"void main() {\n"
	"    if (v_wpos.y > u_clip_y) discard;\n"
	"    vec3 n = normalize(v_normal);\n"
	"    if (!gl_FrontFacing) n = -n;\n"
	"    vec3 c = u_color;\n"
	"    if (u_use_tex == 1) c *= texture(u_tex, v_uv).rgb;\n"
	"    c = scene_surface(c, n, v_wpos);\n"
	"    vec3 lit = c * scene_light(n, v_wpos) + scene_sheen(n, v_wpos);\n"
	"    frag = vec4(scene_fog(lit, v_wpos), 1.0);\n"
	"}\n";

static GLuint g_program, g_vao, g_vbo, g_ibo;
static GLuint g_textures[OBJ_MAX_MATERIALS];
static float  g_colors[OBJ_MAX_MATERIALS][3];
static int    g_material_count;
static GLint  u_mvp, u_model, u_color, u_use_tex, u_tex;
static scene_light_uniforms g_light;
static GLint  u_clip_y;

// The scene's footprint in world units, from the batch boxes.
static float g_bounds[4];
static float g_heights[2];
static int   g_have_bounds;

/* Only the batch table stays on the CPU; the geometry lives on the GPU. */
static ObjBatch* g_batches;
static int       g_batch_count;

static GLuint compile(GLenum type, const char* const* parts, int part_count) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, part_count, parts, NULL);
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
        CLOG_ERROR("[CITY] Shader program failed to link: %s", log);
        glDeleteProgram(g_program);
        g_program = 0;
        return 0;
    }

    u_mvp         = glGetUniformLocation(g_program, "u_mvp");
    u_model       = glGetUniformLocation(g_program, "u_model");
    u_color       = glGetUniformLocation(g_program, "u_color");
    u_use_tex     = glGetUniformLocation(g_program, "u_use_tex");
    u_tex         = glGetUniformLocation(g_program, "u_tex");
    u_clip_y      = glGetUniformLocation(g_program, "u_clip_y");
    scene_light_locate(g_program, &g_light);
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

// Find the world-space rectangle every batch lies inside.
static void measure_bounds(void) {
	const float s = CITY_UNITS_PER_METRE;

	g_have_bounds = g_batch_count > 0;
	for(int i = 0; i < g_batch_count; i++) {
		const ObjBatch* b = &g_batches[i];
		float lo_x = CITY_ORIGIN_X + b->min[0] * s;
		float lo_y = CITY_ORIGIN_Y + b->min[2] * s;
		float hi_x = CITY_ORIGIN_X + b->max[0] * s;
		float hi_y = CITY_ORIGIN_Y + b->max[2] * s;

		if(i == 0 || lo_x < g_bounds[0])
			g_bounds[0] = lo_x;
		if(i == 0 || lo_y < g_bounds[1])
			g_bounds[1] = lo_y;
		if(i == 0 || hi_x > g_bounds[2])
			g_bounds[2] = hi_x;
		if(i == 0 || hi_y > g_bounds[3])
			g_bounds[3] = hi_y;
		if(i == 0 || b->min[1] * s < g_heights[0])
			g_heights[0] = b->min[1] * s;
		if(i == 0 || b->max[1] * s > g_heights[1])
			g_heights[1] = b->max[1] * s;
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
    measure_bounds();
    uint32_t tris = mesh.index_count / 3, verts = mesh.vertex_count;
    obj_mesh_free(&mesh);

    int lamps = city_lamps_load(CITY_LAMPS_PATH, CITY_ORIGIN_X, CITY_ORIGIN_Y,
                                CITY_UNITS_PER_METRE);
    if (lamps < 0)
        CLOG_WARN("[CITY] Could not read the lamps from %s; the city has none at night",
                  CITY_LAMPS_PATH);
    else
        CLOG_INFO("[CITY] %d lamps", lamps);

    // What the roofs keep the rain off: drawn from above once, now the scene is here
    cover_map_build();

    CLOG_INFO("[CITY] Ready: %u triangles, %u vertices, %d cells; %s in %.2f s, "
              "uploaded in %.2f s", tris, verts, g_batch_count,
              from_cache ? "read from cache" : "parsed", loaded - start,
              glfwGetTime() - loaded);
    return 1;
}

void city_renderer_shutdown(void) {
    cover_map_shutdown();
    for (int i = 0; i < g_material_count; i++)
        if (g_textures[i]) glDeleteTextures(1, &g_textures[i]);
    memset(g_textures, 0, sizeof(g_textures));
    g_material_count = 0;
    if (g_ibo) { 
        glDeleteBuffers(1, &g_ibo);         
        g_ibo = 0; 
    }

    if (g_vbo) { 
        glDeleteBuffers(1, &g_vbo);         
        g_vbo = 0; 
    }

    if (g_vao) { 
        glDeleteVertexArrays(1, &g_vao);    
        g_vao = 0; 
    }

    if (g_program) { 
        glDeleteProgram(g_program);         
        g_program = 0; 
    }

    free(g_batches);
    g_batches = NULL;
    g_batch_count = 0;
    g_have_bounds = 0;
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

// Draw every cell the frustum holds, but nothing above clip_y
static void draw_scene(const float* viewproj, float clip_y) {
    if (!g_program || g_batch_count == 0) return;

    const float s = CITY_UNITS_PER_METRE;
    mat4 model, mvp;
    glm_mat4_identity(model);
    glm_translate(model, (vec3){ CITY_ORIGIN_X, 0.0f, CITY_ORIGIN_Y });
    glm_scale_uni(model, s);
    glm_mat4_mul((vec4*)viewproj, model, mvp);

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
    scene_light_apply(&g_light);
    glUniform1i(u_tex, 0);
    glUniform1f(u_clip_y, clip_y);
    glActiveTexture(GL_TEXTURE0);

    int bound = -1;
    for (int i = 0; i < g_batch_count; i++) {
        const ObjBatch* b = &g_batches[i];
        const float lo[3] = { CITY_ORIGIN_X + b->min[0] * s, b->min[1] * s,
                              CITY_ORIGIN_Y + b->min[2] * s };
        const float hi[3] = { CITY_ORIGIN_X + b->max[0] * s, b->max[1] * s,
                              CITY_ORIGIN_Y + b->max[2] * s };
        if (!box_in_frustum(viewproj, lo, hi))
            continue;

        if (b->material != bound) {
            bound = b->material;
            GLuint tex = g_textures[bound];
            glUniform1i(u_use_tex, tex ? 1 : 0);
            if (tex) 
                glBindTexture(GL_TEXTURE_2D, tex);
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

void city_renderer_draw(const CameraView* view) {
	draw_scene(view->viewproj, 1.0e9f);
}

void city_renderer_draw_map(const float* viewproj, float clip_y) {
	// A map shows the city in daylight, whatever hour it is in the world
	scene_light_set_flat(1);
	draw_scene(viewproj, clip_y);
	scene_light_set_flat(0);
}

void city_renderer_draw_shadow(const float* light_viewproj) {
	const float s = CITY_UNITS_PER_METRE;
	mat4 model, mvp;

	if(!g_vao || g_batch_count == 0)
		return;

	glm_mat4_identity(model);
	glm_translate(model, (vec3){ CITY_ORIGIN_X, 0.0f, CITY_ORIGIN_Y });
	glm_scale_uni(model, s);
	glm_mat4_mul((vec4*)light_viewproj, model, mvp);
	shadow_map_set_mvp((const float*)mvp);

	glBindVertexArray(g_vao);
	for(int i = 0; i < g_batch_count; i++) {
		const ObjBatch* b = &g_batches[i];
		const float lo[3] = { CITY_ORIGIN_X + b->min[0] * s, b->min[1] * s,
		                      CITY_ORIGIN_Y + b->min[2] * s };
		const float hi[3] = { CITY_ORIGIN_X + b->max[0] * s, b->max[1] * s,
		                      CITY_ORIGIN_Y + b->max[2] * s };

		if(!box_in_frustum(light_viewproj, lo, hi))
			continue;
		glDrawElements(GL_TRIANGLES, (GLsizei)b->count, GL_UNSIGNED_INT,
		               (const void*)((size_t)b->first * sizeof(uint32_t)));
	}
	glBindVertexArray(0);
}

int city_renderer_height_range(float* low, float* high) {
	if(!g_have_bounds)
		return 0;
	if(low)
		*low = g_heights[0];
	if(high)
		*high = g_heights[1];

	return 1;
}

int city_renderer_bounds(float* min_x, float* min_y, float* max_x, float* max_y) {
	if(!g_have_bounds)
		return 0;
	if(min_x)
		*min_x = g_bounds[0];
	if(min_y)
		*min_y = g_bounds[1];
	if(max_x)
		*max_x = g_bounds[2];
	if(max_y)
		*max_y = g_bounds[3];

	return 1;
}
