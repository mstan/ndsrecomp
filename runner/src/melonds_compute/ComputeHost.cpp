// GPL-3.0-or-later; see ComputeHost.h and THIRD_PARTY_ATTRIBUTION.md.
#include "ComputeHost.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "glad/glad.h"
#include "gpu2d.h"
#include "gpu3d.h"

namespace
{
SDL_Window* g_window = nullptr;
SDL_GLContext g_context = nullptr;
bool g_owns_video = false;
bool g_owns_window = false;
bool g_active = false;
bool g_visible = false;
GLuint g_present_program = 0;
GLuint g_present_vao = 0;
GLuint g_fallback_texture[2] = {};
GLuint g_object_texture[2] = {};
int g_texture_width = 0;
unsigned g_present_buffer = 0;

const char* kPresentVertex = R"GLSL(
#version 430 core
out vec2 uv;
void main()
{
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    uv = vec2(p.x, 1.0 - p.y);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

const char* kPresentFragment = R"GLSL(
#version 430 core
layout(binding = 0) uniform usampler2D tex3d;
layout(binding = 1) uniform usampler2D texObject;
layout(binding = 2) uniform usampler2D texFallback;
layout(location = 0) uniform uint directMode;
layout(location = 1) uniform uint outputWidth;
layout(location = 2) uniform uint renderXPos;
layout(location = 3) uniform uvec3 backdrop;
layout(location = 4) uniform uint bldcnt;
layout(location = 5) uniform uint eva;
layout(location = 6) uniform uint evb;
layout(location = 7) uniform uint evy;
layout(location = 8) uniform uint masterBright;
layout(location = 9) uniform uint priority3d;
in vec2 uv;
out vec4 outColor;

uvec3 blend16(uvec3 a, uvec3 b, uint ca, uint cb)
{
    return min((a * ca + b * cb + uvec3(8u)) >> 4, uvec3(63u));
}

uvec3 blend5(uvec3 a, uvec3 b, uint alpha)
{
    uint ca = alpha + 1u;
    if (ca == 32u) return a;
    return min((a * ca + b * (32u - ca) + uvec3(16u)) >> 5,
               uvec3(63u));
}

uvec3 brighten(uvec3 c, uint factor, uint bias)
{
    return min(c + (((uvec3(63u) - c) * factor + uvec3(bias)) >> 4),
               uvec3(63u));
}

uvec3 darken(uvec3 c, uint factor, uint bias)
{
    return c - ((c * factor + uvec3(bias)) >> 4);
}

uvec3 effect(uvec3 top, uvec3 below, uint topTarget,
             uint belowTarget, uint alphaCode, uint alpha5)
{
    uint target2 = belowTarget << 8;
    if (alpha5 != 0u) {
        if ((bldcnt & target2) != 0u)
            return blend5(top, below, alpha5);
        if ((bldcnt & topTarget) != 0u) {
            uint mode = (bldcnt >> 6) & 3u;
            if (mode == 2u) return brighten(top, evy, 8u);
            if (mode == 3u) return darken(top, evy, 7u);
        }
        return top;
    }
    if (alphaCode != 0u && (bldcnt & target2) != 0u) {
        if (alphaCode == 255u) return blend16(top, below, eva, evb);
        return blend16(top, below, alphaCode, 16u - alphaCode);
    }
    if ((bldcnt & topTarget) != 0u) {
        uint mode = (bldcnt >> 6) & 3u;
        if (mode == 1u && (bldcnt & target2) != 0u)
            return blend16(top, below, eva, evb);
        if (mode == 2u) return brighten(top, evy, 8u);
        if (mode == 3u) return darken(top, evy, 7u);
    }
    return top;
}

void main()
{
    ivec2 size = textureSize(texFallback, 0);
    ivec2 dst = ivec2(clamp(uv * vec2(size), vec2(0.0),
                            vec2(size) - vec2(1.0)));
    if (directMode == 0u) {
        uvec4 pixel = texelFetch(texFallback, dst, 0);
        outColor = vec4(pixel) / 255.0;
        return;
    }

    int x = dst.x;
    int sourceX = x;
    bool outside = false;
    if (renderXPos != 0u) {
        if ((renderXPos & 0x100u) != 0u) {
            int shift = min(int(outputWidth), 512 - int(renderXPos));
            outside = x < shift;
            sourceX = x - shift;
        } else {
            sourceX = x + int(renderXPos);
            outside = sourceX >= int(outputWidth);
        }
    }
    uvec4 pixel3d = outside
        ? uvec4(0u)
        : texelFetch(tex3d, ivec2(sourceX, dst.y), 0);
    uvec3 color3d = pixel3d.rgb & uvec3(63u);
    uint alpha3d = pixel3d.a & 31u;
    uvec4 object = texelFetch(texObject, dst, 0);
    uint objectPriority = object.b >> 6;
    uint objectAlphaCode = object.a;
    bool objectValid = objectAlphaCode != 0u;
    bool objectWins = objectValid &&
        (alpha3d == 0u || objectPriority <= priority3d);

    uvec3 color;
    if (objectWins) {
        uvec3 below = alpha3d != 0u ? color3d : backdrop;
        uint belowTarget = alpha3d != 0u ? 1u : 32u;
        uint alphaCode = objectAlphaCode == 17u ? 0u : objectAlphaCode;
        color = effect(object.rgb & uvec3(63u), below, 16u,
                       belowTarget, alphaCode, 0u);
    } else if (alpha3d != 0u) {
        color = effect(color3d, backdrop, 1u, 32u, 0u, alpha3d);
    } else {
        color = effect(backdrop, backdrop, 32u, 32u, 0u, 0u);
    }

    uint brightMode = masterBright >> 14;
    uint brightFactor = min(masterBright & 31u, 16u);
    if (brightMode == 1u)
        color = brighten(color, brightFactor, 0u);
    else if (brightMode == 2u)
        color = darken(color, brightFactor, 15u);
    outColor = vec4(vec3(color) / 63.0, 1.0);
}
)GLSL";

GLuint compile_shader(GLenum type, const char* source)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok == GL_TRUE) return shader;
    char log[2048] = {};
    glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
    std::fprintf(stderr, "[gpu3d] direct-present shader failed: %s\n", log);
    glDeleteShader(shader);
    return 0;
}

bool start_presenter()
{
    GLuint vertex = compile_shader(GL_VERTEX_SHADER, kPresentVertex);
    GLuint fragment = compile_shader(GL_FRAGMENT_SHADER, kPresentFragment);
    if (!vertex || !fragment) {
        if (vertex) glDeleteShader(vertex);
        if (fragment) glDeleteShader(fragment);
        return false;
    }
    g_present_program = glCreateProgram();
    glAttachShader(g_present_program, vertex);
    glAttachShader(g_present_program, fragment);
    glLinkProgram(g_present_program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    GLint ok = GL_FALSE;
    glGetProgramiv(g_present_program, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE) {
        char log[2048] = {};
        glGetProgramInfoLog(g_present_program, sizeof(log), nullptr, log);
        std::fprintf(stderr, "[gpu3d] direct-present link failed: %s\n", log);
        return false;
    }
    glGenVertexArrays(1, &g_present_vao);
    glGenTextures(2, g_fallback_texture);
    glGenTextures(2, g_object_texture);
    return true;
}

void stop_presenter()
{
    glDeleteTextures(2, g_object_texture);
    glDeleteTextures(2, g_fallback_texture);
    if (g_present_vao) glDeleteVertexArrays(1, &g_present_vao);
    if (g_present_program) glDeleteProgram(g_present_program);
    g_object_texture[0] = g_object_texture[1] = 0;
    g_fallback_texture[0] = g_fallback_texture[1] = 0;
    g_present_vao = 0;
    g_present_program = 0;
    g_texture_width = 0;
    g_present_buffer = 0;
}

void configure_integer_texture(GLuint texture, int width)
{
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8UI, width, 192, 0,
                 GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, nullptr);
}

void release_host_objects()
{
    if (g_context && g_window) {
        SDL_GL_MakeCurrent(g_window, g_context);
        stop_presenter();
    }
    if (g_context) SDL_GL_DeleteContext(g_context);
    if (g_window && g_owns_window) SDL_DestroyWindow(g_window);
    if (g_owns_video) SDL_QuitSubSystem(SDL_INIT_VIDEO);
    g_window = nullptr;
    g_context = nullptr;
    g_owns_video = false;
    g_owns_window = false;
    g_active = false;
    g_visible = false;
}
}

bool nds_compute_host_start(SDL_Window* presentation_window)
{
    const NdsGpu3dRendererPolicy policy = nds_gpu3d_renderer_policy();
    if (policy == NdsGpu3dRendererPolicy::Soft) return true;
    if (g_active) return true;
    const bool required = policy == NdsGpu3dRendererPolicy::Compute;
    auto fail_or_fallback = [&](const char* reason) {
        nds_gpu3d_restore_soft_renderer();
        release_host_objects();
        if (required) return false;
        std::fprintf(stderr,
            "[gpu3d] OpenGL auto-selection failed (%s); "
            "using threaded software\n", reason);
        return true;
    };

    SDL_SetMainReady();
    g_owns_video = (SDL_WasInit(SDL_INIT_VIDEO) == 0);
    if (g_owns_video && SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "[gpu3d] SDL video init failed: %s\n",
                     SDL_GetError());
        return fail_or_fallback("SDL video initialization");
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                        SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, presentation_window ? 1 : 0);
    g_window = presentation_window;
    g_visible = presentation_window != nullptr;
    if (!g_window) {
        g_window = SDL_CreateWindow(
            "ndsrecomp compute context", SDL_WINDOWPOS_UNDEFINED,
            SDL_WINDOWPOS_UNDEFINED, 1, 1,
            SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
        g_owns_window = true;
        if (!g_window) {
            std::fprintf(stderr, "[gpu3d] compute GL window failed: %s\n",
                         SDL_GetError());
            SDL_GL_ResetAttributes();
            return fail_or_fallback("OpenGL window creation");
        }
    }
    g_context = SDL_GL_CreateContext(g_window);
    SDL_GL_ResetAttributes();
    if (!g_context || SDL_GL_MakeCurrent(g_window, g_context) != 0) {
        std::fprintf(stderr, "[gpu3d] compute GL 4.3 context failed: %s\n",
                     SDL_GetError());
        return fail_or_fallback("OpenGL 4.3 context creation");
    }
    if (!gladLoadGLLoader(
            reinterpret_cast<GLADloadproc>(SDL_GL_GetProcAddress)) ||
        !GLAD_GL_VERSION_4_3) {
        std::fprintf(stderr, "[gpu3d] OpenGL 4.3 unavailable\n");
        return fail_or_fallback("OpenGL 4.3 unavailable");
    }
    std::fprintf(stderr, "[gpu3d] OpenGL %s / %s\n",
                 reinterpret_cast<const char*>(glGetString(GL_VERSION)),
                 reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
    if (!nds_gpu3d_use_compute_renderer()) {
        std::fprintf(stderr, "[gpu3d] compute renderer init failed\n");
        return fail_or_fallback("compute renderer initialization");
    }
    if (g_visible && !start_presenter()) {
        std::fprintf(stderr, "[gpu3d] direct presenter init failed\n");
        return fail_or_fallback("direct presenter initialization");
    }
    if (g_visible) SDL_GL_SetSwapInterval(0);
    g_active = true;
    std::fprintf(stderr, "[gpu3d] renderer: OpenGL 4.3 compute\n");
    return true;
}

bool nds_compute_host_make_current()
{
    return !g_active || SDL_GL_MakeCurrent(g_window, g_context) == 0;
}

bool nds_compute_host_has_visible_context()
{
    return g_active && g_visible;
}

bool nds_compute_host_present_top(const unsigned int* fallback_pixels,
                                  unsigned short fallback_width,
                                  const NdsGpu2dDirectFrame* direct_frame,
                                  NdsComputePresentTicks* ticks)
{
    if (!g_active || !g_visible || !g_present_program || !ticks)
        return false;
    if (SDL_GL_MakeCurrent(g_window, g_context) != 0) return false;
    const int width = direct_frame ? direct_frame->width : fallback_width;
    if (width <= 0 || width > 448) return false;
    if (g_texture_width != width) {
        for (int i = 0; i < 2; ++i) {
            configure_integer_texture(g_fallback_texture[i], width);
            configure_integer_texture(g_object_texture[i], width);
        }
        g_texture_width = width;
    }
    const unsigned buffer = g_present_buffer;
    g_present_buffer ^= 1u;

    unsigned long long start = SDL_GetPerformanceCounter();
    if (direct_frame) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, g_object_texture[buffer]);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, 192,
                        GL_RGBA_INTEGER, GL_UNSIGNED_BYTE,
                        direct_frame->object_pixels);
        glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
    } else {
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, g_fallback_texture[buffer]);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, 192,
                        GL_BGRA_INTEGER, GL_UNSIGNED_BYTE, fallback_pixels);
    }
    ticks->upload += SDL_GetPerformanceCounter() - start;

    start = SDL_GetPerformanceCounter();
    int drawable_width = 0;
    int drawable_height = 0;
    SDL_GL_GetDrawableSize(g_window, &drawable_width, &drawable_height);
    glViewport(0, 0, drawable_width, drawable_height);
    glUseProgram(g_present_program);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, g_object_texture[buffer]);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, g_fallback_texture[buffer]);
    glUniform1ui(0, direct_frame ? 1u : 0u);
    glUniform1ui(1, static_cast<GLuint>(width));
    if (direct_frame) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, nds_gpu3d_compute_output_texture());
        glUniform1ui(2, direct_frame->render_xpos);
        glUniform3ui(3,
            direct_frame->backdrop_color & 0x3Fu,
            (direct_frame->backdrop_color >> 8) & 0x3Fu,
            (direct_frame->backdrop_color >> 16) & 0x3Fu);
        glUniform1ui(4, direct_frame->bldcnt);
        glUniform1ui(5, direct_frame->eva);
        glUniform1ui(6, direct_frame->evb);
        glUniform1ui(7, direct_frame->evy);
        glUniform1ui(8, direct_frame->master_bright);
        glUniform1ui(9, direct_frame->priority_3d);
        glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
    }
    glBindVertexArray(g_present_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    ticks->draw += SDL_GetPerformanceCounter() - start;

    start = SDL_GetPerformanceCounter();
    SDL_GL_SwapWindow(g_window);
    ticks->swap += SDL_GetPerformanceCounter() - start;
    return glGetError() == GL_NO_ERROR;
}

void nds_compute_host_stop()
{
    if (!g_active) {
        release_host_objects();
        return;
    }
    SDL_GL_MakeCurrent(g_window, g_context);
    nds_gpu3d_restore_soft_renderer();
    release_host_objects();
}

bool nds_compute_host_active()
{
    return g_active;
}
