/**
 * @file
 * Fetch the post-1.1 OpenGL entry points from the driver through GLFW.
 */

#include "render/gl_loader.h"
#include "core/client_log.h"

#include <stdio.h>
#include <string.h>

#define MMO_GL_DEFINE(ret, name, args) mmo_PFN_##name mmo_gl##name = NULL;
MMO_GL_FUNCTIONS(MMO_GL_DEFINE)
#undef MMO_GL_DEFINE

static float g_max_anisotropy = 1.0f;

void gl_loader_window_hints(void) {
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_COMPAT_PROFILE);
    glfwWindowHint(GLFW_DEPTH_BITS, 24);
}

/** Parse "major.minor" from the start of GL_VERSION. */
static int context_version(int* major, int* minor) {
    const char* version = (const char*)glGetString(GL_VERSION);
    *major = *minor = 0;
    return version && sscanf(version, "%d.%d", major, minor) == 2;
}

int gl_loader_init(void) {
    int major, minor;
    if (!context_version(&major, &minor) || major < 3 || (major == 3 && minor < 3)) {
        CLOG_ERROR("[GL] OpenGL 3.3 is required; this context is %s (%s)",
                   (const char*)glGetString(GL_VERSION),
                   (const char*)glGetString(GL_RENDERER));
        return 0;
    }

    int missing = 0;
#define MMO_GL_LOAD(ret, name, args)                                              \
    mmo_gl##name = (mmo_PFN_##name)glfwGetProcAddress("gl" #name);                \
    if (!mmo_gl##name) {                                                          \
        CLOG_ERROR("[GL] Driver did not provide gl" #name);                       \
        missing++;                                                                \
    }
    MMO_GL_FUNCTIONS(MMO_GL_LOAD)
#undef MMO_GL_LOAD

    if (missing) return 0;

    /* Anisotropy is core in 4.6 and an extension everywhere before it; the
     * query is harmless when neither is present, it just leaves the default. */
    if (glfwExtensionSupported("GL_EXT_texture_filter_anisotropic") ||
        glfwExtensionSupported("GL_ARB_texture_filter_anisotropic")) {
        GLfloat max = 1.0f;
        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &max);
        if (max > 1.0f) g_max_anisotropy = max;
    }

    CLOG_INFO("[GL] %s on %s, anisotropy up to %.0fx",
              (const char*)glGetString(GL_VERSION),
              (const char*)glGetString(GL_RENDERER), g_max_anisotropy);
    return 1;
}

float gl_loader_max_anisotropy(void) {
    return g_max_anisotropy;
}
