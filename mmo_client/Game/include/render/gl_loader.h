#ifndef GL_LOADER_H
#define GL_LOADER_H

/**
 * @file
 * Load the OpenGL 2.0-3.3 entry points the renderer uses beyond GL 1.1.
 *
 * Windows' opengl32.dll exports only OpenGL 1.1; everything newer has to be
 * fetched from the driver at runtime. This is the short list the client
 * actually calls -- buffers, vertex arrays, shaders, mipmaps -- loaded through
 * GLFW, so no generated loader has to be vendored (Next_steps/3d_refactor.md,
 * D26).
 *
 * Each function is called by its normal GL name: the macros below redirect
 * the name to the loaded pointer. Include this header, not the GL headers
 * alone, in any file that calls one of them.
 */

#include <GLFW/glfw3.h>
#include <stddef.h>

#ifndef APIENTRY
#define APIENTRY
#endif

typedef char      mmo_GLchar;
typedef ptrdiff_t mmo_GLsizeiptr;

/* Enums past GL 1.1. Guarded: a platform header may already define them. */
#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER                 0x8892
#endif
#ifndef GL_STATIC_DRAW
#define GL_STATIC_DRAW                  0x88E4
#endif
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER              0x8B30
#endif
#ifndef GL_VERTEX_SHADER
#define GL_VERTEX_SHADER                0x8B31
#endif
#ifndef GL_COMPILE_STATUS
#define GL_COMPILE_STATUS               0x8B81
#endif
#ifndef GL_LINK_STATUS
#define GL_LINK_STATUS                  0x8B82
#endif
#ifndef GL_INFO_LOG_LENGTH
#define GL_INFO_LOG_LENGTH              0x8B84
#endif
#ifndef GL_TEXTURE0
#define GL_TEXTURE0                     0x84C0
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE                0x812F
#endif
#ifndef GL_TEXTURE_MAX_LEVEL
#define GL_TEXTURE_MAX_LEVEL            0x813D
#endif
#ifndef GL_TEXTURE_MAX_ANISOTROPY
#define GL_TEXTURE_MAX_ANISOTROPY       0x84FE
#endif
#ifndef GL_MAX_TEXTURE_MAX_ANISOTROPY
#define GL_MAX_TEXTURE_MAX_ANISOTROPY   0x84FF
#endif

#define MMO_GL_FUNCTIONS(X)                                                                  \
    X(void,   GenBuffers,              (GLsizei n, GLuint* buffers))                         \
    X(void,   DeleteBuffers,           (GLsizei n, const GLuint* buffers))                   \
    X(void,   BindBuffer,              (GLenum target, GLuint buffer))                       \
    X(void,   BufferData,              (GLenum target, mmo_GLsizeiptr size,                  \
                                        const void* data, GLenum usage))                     \
    X(void,   GenVertexArrays,         (GLsizei n, GLuint* arrays))                          \
    X(void,   DeleteVertexArrays,      (GLsizei n, const GLuint* arrays))                    \
    X(void,   BindVertexArray,         (GLuint array))                                       \
    X(void,   VertexAttribPointer,     (GLuint index, GLint size, GLenum type,               \
                                        GLboolean normalized, GLsizei stride,                \
                                        const void* pointer))                                \
    X(void,   EnableVertexAttribArray, (GLuint index))                                       \
    X(GLuint, CreateShader,            (GLenum type))                                        \
    X(void,   ShaderSource,            (GLuint shader, GLsizei count,                        \
                                        const mmo_GLchar* const* string,                     \
                                        const GLint* length))                                \
    X(void,   CompileShader,           (GLuint shader))                                      \
    X(void,   GetShaderiv,             (GLuint shader, GLenum pname, GLint* params))         \
    X(void,   GetShaderInfoLog,        (GLuint shader, GLsizei max, GLsizei* length,         \
                                        mmo_GLchar* log))                                    \
    X(void,   DeleteShader,            (GLuint shader))                                      \
    X(GLuint, CreateProgram,           (void))                                               \
    X(void,   AttachShader,            (GLuint program, GLuint shader))                      \
    X(void,   BindAttribLocation,      (GLuint program, GLuint index,                        \
                                        const mmo_GLchar* name))                             \
    X(void,   LinkProgram,             (GLuint program))                                     \
    X(void,   GetProgramiv,            (GLuint program, GLenum pname, GLint* params))        \
    X(void,   GetProgramInfoLog,       (GLuint program, GLsizei max, GLsizei* length,        \
                                        mmo_GLchar* log))                                    \
    X(void,   UseProgram,              (GLuint program))                                     \
    X(void,   DeleteProgram,           (GLuint program))                                     \
    X(GLint,  GetUniformLocation,      (GLuint program, const mmo_GLchar* name))             \
    X(void,   UniformMatrix4fv,        (GLint location, GLsizei count,                       \
                                        GLboolean transpose, const GLfloat* value))          \
    X(void,   Uniform1i,               (GLint location, GLint v0))                           \
    X(void,   Uniform1f,               (GLint location, GLfloat v0))                         \
    X(void,   Uniform2f,               (GLint location, GLfloat v0, GLfloat v1))             \
    X(void,   ActiveTexture,           (GLenum texture))                                     \
    X(void,   GenerateMipmap,          (GLenum target))

#define MMO_GL_DECLARE(ret, name, args)                  \
    typedef ret (APIENTRY* mmo_PFN_##name) args;         \
    extern mmo_PFN_##name mmo_gl##name;
MMO_GL_FUNCTIONS(MMO_GL_DECLARE)
#undef MMO_GL_DECLARE

#define glGenBuffers              mmo_glGenBuffers
#define glDeleteBuffers           mmo_glDeleteBuffers
#define glBindBuffer              mmo_glBindBuffer
#define glBufferData              mmo_glBufferData
#define glGenVertexArrays         mmo_glGenVertexArrays
#define glDeleteVertexArrays      mmo_glDeleteVertexArrays
#define glBindVertexArray         mmo_glBindVertexArray
#define glVertexAttribPointer     mmo_glVertexAttribPointer
#define glEnableVertexAttribArray mmo_glEnableVertexAttribArray
#define glCreateShader            mmo_glCreateShader
#define glShaderSource            mmo_glShaderSource
#define glCompileShader           mmo_glCompileShader
#define glGetShaderiv             mmo_glGetShaderiv
#define glGetShaderInfoLog        mmo_glGetShaderInfoLog
#define glDeleteShader            mmo_glDeleteShader
#define glCreateProgram           mmo_glCreateProgram
#define glAttachShader            mmo_glAttachShader
#define glBindAttribLocation      mmo_glBindAttribLocation
#define glLinkProgram             mmo_glLinkProgram
#define glGetProgramiv            mmo_glGetProgramiv
#define glGetProgramInfoLog       mmo_glGetProgramInfoLog
#define glUseProgram              mmo_glUseProgram
#define glDeleteProgram           mmo_glDeleteProgram
#define glGetUniformLocation      mmo_glGetUniformLocation
#define glUniformMatrix4fv        mmo_glUniformMatrix4fv
#define glUniform1i               mmo_glUniform1i
#define glUniform1f               mmo_glUniform1f
#define glUniform2f               mmo_glUniform2f
#define glActiveTexture           mmo_glActiveTexture
#define glGenerateMipmap          mmo_glGenerateMipmap

/** Request a context this loader can serve. Call before glfwCreateWindow.
 *
 * A 3.3 *compatibility* context: the shader path needs 3.3, and the rest of
 * the client still draws with GL 1.1 immediate mode, which a core context
 * refuses. Asking for 3.3 without naming the profile gets a core context on
 * Mesa, where every glBegin would silently fail.
 */
void gl_loader_window_hints(void);

/** Load every entry point, once a context is current.
 *
 * @return Nonzero on success. On failure the missing names and the context's
 *         version are logged, and nothing past GL 1.1 may be called.
 */
int gl_loader_init(void);

/** Largest anisotropy the driver allows, or 1.0 when it has none. */
float gl_loader_max_anisotropy(void);

#endif /* GL_LOADER_H */
