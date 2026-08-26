#pragma once
// Android/GLES build of the melonDS compute renderer. Instead of the desktop
// GL 4.3 glad loader, use the platform GLES 3.2 headers directly (libGLESv3
// provides the entry points). GLES 3.2 supplies compute shaders, image
// load/store, atomics, shared memory and barriers, which is what the renderer
// needs. GLES 3.2 support is verified at runtime in ComputeHost.
#include <GLES3/gl32.h>

// The renderer guards init on GLAD_GL_VERSION_4_3; on GLES that check is
// replaced by an explicit GLES 3.2 version test in ComputeHost, so treat the
// desktop-version flag as satisfied here.
#ifndef GLAD_GL_VERSION_4_3
#define GLAD_GL_VERSION_4_3 1
#endif

// ── Desktop-GL entry points GLES lacks; shimmed to GLES equivalents ──────────
#ifndef GL_READ_ONLY
#define GL_READ_ONLY  0x88B8
#define GL_WRITE_ONLY 0x88B9
#define GL_READ_WRITE 0x88BA
#endif

// GLES has no GL_BGRA_INTEGER. Only the software-render fallback upload uses it
// (the primary compute path uses GL_RGBA_INTEGER); map it so the build works.
// TODO: swizzle R/B if the fallback path's colors need correcting.
#ifndef GL_BGRA_INTEGER
#define GL_BGRA_INTEGER GL_RGBA_INTEGER
#endif

// glMapBuffer(target, access): GLES only has glMapBufferRange, which needs an
// explicit range. Map the whole buffer, translating the access enum to bits.
static inline void* nds_glMapBuffer(GLenum target, GLenum access) {
    GLint size = 0;
    glGetBufferParameteriv(target, GL_BUFFER_SIZE, &size);
    GLbitfield bits = (access == GL_READ_ONLY)  ? GL_MAP_READ_BIT
                    : (access == GL_WRITE_ONLY) ? GL_MAP_WRITE_BIT
                    : (GL_MAP_READ_BIT | GL_MAP_WRITE_BIT);
    return glMapBufferRange(target, 0, size, bits);
}
#define glMapBuffer nds_glMapBuffer

// glBindFragDataLocation: on GLES fragment outputs are bound by an in-shader
// layout(location=N) qualifier, so the host-side call is a no-op.
static inline void nds_glBindFragDataLocation(GLuint, GLuint, const GLchar*) {}
#define glBindFragDataLocation nds_glBindFragDataLocation

// glGetTexImage: GLES has no texture readback. Attach the bound texture to a
// temporary read FBO and glReadPixels into the bound pixel-pack buffer.
static inline void nds_glGetTexImage(GLenum target, GLint level, GLenum format,
                                     GLenum type, void* pixels) {
    GLint tex = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
    GLint w = 0, h = 0;
    glGetTexLevelParameteriv(target, level, GL_TEXTURE_WIDTH, &w);
    glGetTexLevelParameteriv(target, level, GL_TEXTURE_HEIGHT, &h);
    static GLuint fbo = 0;
    if (!fbo) glGenFramebuffers(1, &fbo);
    GLint prev = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, target,
                           static_cast<GLuint>(tex), level);
    glReadPixels(0, 0, w, h, format, type, pixels);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(prev));
}
#define glGetTexImage nds_glGetTexImage
