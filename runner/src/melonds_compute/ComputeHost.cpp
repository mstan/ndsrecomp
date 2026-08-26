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
#include "TextureUpscale.h"

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
GLuint g_hd_top_texture[2] = {};
GLuint g_hd_below_texture[2] = {};
int g_texture_width = 0;
unsigned g_present_buffer = 0;

struct PresentViewport {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

PresentViewport fit_present_viewport(int drawable_width, int drawable_height,
                                     int content_width, int content_height)
{
    PresentViewport viewport{};
    if (drawable_width <= 0 || drawable_height <= 0 ||
        content_width <= 0 || content_height <= 0) {
        return viewport;
    }

    viewport.width = drawable_width;
    viewport.height = static_cast<int>(
        static_cast<long long>(drawable_width) * content_height /
        content_width);
    if (viewport.height > drawable_height) {
        viewport.height = drawable_height;
        viewport.width = static_cast<int>(
            static_cast<long long>(drawable_height) * content_width /
            content_height);
    }
    if (viewport.width < 1) viewport.width = 1;
    if (viewport.height < 1) viewport.height = 1;
    viewport.x = (drawable_width - viewport.width) / 2;
    viewport.y = (drawable_height - viewport.height) / 2;
    return viewport;
}

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
// Internal-resolution (HD) 3D surface. Same RGB6/alpha5 values as tex3d,
// stored normalized by 63 and 31 in UNORM8. Both quantizations are injective
// into 8 bits, so the inverse round below recovers the exact integers and the
// DS blend math downstream stays integer-identical to the native path.
layout(binding = 3) uniform sampler2D tex3dHi;
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
layout(location = 10) uniform uint scale3d;
// HD composite: the 2D stack resolved with the 3D layer removed. See
// NdsGpu2dHdFrame. Non-zero hdMode selects this path over the OBJ-only one.
layout(binding = 4) uniform usampler2D texHdTop;
layout(binding = 5) uniform usampler2D texHdBelow;
layout(location = 11) uniform uint hdMode;
layout(location = 12) uniform uint order3d;
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

// A resolved layer: colour plus the BLDCNT target, alpha, and the
// (priority, order) key the DS sorts by.
struct Layer {
    uvec3 color;
    uint target;
    bool effects;
    uint alpha;    // 0 = no alpha blend, 1..16 = OBJ alpha, 255 = EVA/EVB
    uint alpha5;   // non-zero only on the 3D layer: its own 5-bit coverage
    uint priority;
    uint order;
};

// The emitted 2D surfaces never contain the 3D layer, so alpha5 is always
// zero here; the presenter constructs the 3D layer itself.
Layer unpackLayer(uvec2 texel)
{
    Layer l;
    l.color = uvec3(texel.x & 63u, (texel.x >> 8) & 63u,
                    (texel.x >> 16) & 63u);
    l.target = texel.y & 63u;
    l.effects = (texel.y & 128u) != 0u;
    l.alpha = (texel.y >> 8) & 255u;
    l.alpha5 = 0u;
    l.priority = (texel.y >> 16) & 255u;
    l.order = (texel.y >> 24) & 255u;
    return l;
}

bool ahead(Layer a, Layer b)
{
    return a.priority < b.priority ||
           (a.priority == b.priority && a.order < b.order);
}

// Mirrors compose() in gpu2d.cpp exactly, including the 3D layer's forced
// per-pixel 5-bit blend whenever the pixel behind it is a second target.
uvec3 composeLayers(Layer top, Layer below)
{
    uint target2 = below.target << 8;
    if (top.alpha5 != 0u) {
        // 3D layer on top: whenever the pixel behind is a second target the
        // per-pixel 5-bit blend is forced regardless of the colour effect.
        if ((bldcnt & target2) != 0u)
            return blend5(top.color, below.color, top.alpha5);
        if (top.effects && (bldcnt & top.target) != 0u) {
            uint mode = (bldcnt >> 6) & 3u;
            if (mode == 2u) return brighten(top.color, evy, 8u);
            if (mode == 3u) return darken(top.color, evy, 7u);
        }
        return top.color;
    }
    if (top.alpha != 0u && (bldcnt & target2) != 0u) {
        uint eva1 = top.alpha == 255u ? eva : top.alpha;
        uint evb1 = top.alpha == 255u ? evb : 16u - eva1;
        return blend16(top.color, below.color, eva1, evb1);
    }
    if (top.effects && (bldcnt & top.target) != 0u) {
        uint mode = (bldcnt >> 6) & 3u;
        if (mode == 1u && (bldcnt & target2) != 0u)
            return blend16(top.color, below.color, eva, evb);
        if (mode == 2u) return brighten(top.color, evy, 8u);
        if (mode == 3u) return darken(top.color, evy, 7u);
    }
    return top.color;
}

void main()
{
    ivec2 size = textureSize(texFallback, 0);
    ivec2 dst = ivec2(clamp(uv * vec2(size), vec2(0.0),
                            vec2(size) - vec2(1.0)));
    if (hdMode != 0u) {
        int s = int(max(scale3d, 1u));
        ivec2 hiSize = size * s;
        ivec2 dstHi = ivec2(clamp(uv * vec2(hiSize), vec2(0.0),
                                  vec2(hiSize) - vec2(1.0)));
        // RenderXPos is a native-pixel scroll and the hi-res 3D surface is
        // unscrolled, so it is applied here scaled to that raster.
        int sx = dstHi.x;
        bool off = false;
        if (renderXPos != 0u) {
            if ((renderXPos & 0x100u) != 0u) {
                int shift = min(int(outputWidth), 512 - int(renderXPos)) * s;
                off = dstHi.x < shift;
                sx = dstHi.x - shift;
            } else {
                sx = dstHi.x + int(renderXPos) * s;
                off = sx >= int(outputWidth) * s;
            }
        }
        Layer top = unpackLayer(texelFetch(texHdTop, dst, 0).xy);
        Layer below = unpackLayer(texelFetch(texHdBelow, dst, 0).xy);
        if (!off) {
            vec4 hi = texelFetch(tex3dHi, ivec2(sx, dstHi.y), 0);
            uint a3 = uint(round(hi.a * 31.0));
            if (a3 != 0u) {
                Layer l3;
                // FinalFB stores red where LowResFB stores blue; swizzle to
                // the DS channel order the rest of this shader uses.
                l3.color = uvec3(round(hi.bgr * 63.0));
                l3.target = 1u;       // the 3D layer is BLDCNT first target 0
                l3.effects = top.effects;
                l3.alpha = 0u;
                l3.alpha5 = a3;
                l3.priority = priority3d;
                l3.order = order3d;
                // Insert into the top-two. (priority, order) is a total
                // order, so this yields the same pair the CPU path picks.
                if (ahead(l3, top)) { below = top; top = l3; }
                else if (ahead(l3, below)) { below = l3; }
            }
        }
        uvec3 c = composeLayers(top, below);
        uint bm = masterBright >> 14;
        uint bf = min(masterBright & 31u, 16u);
        if (bm == 1u) c = brighten(c, bf, 0u);
        else if (bm == 2u) c = darken(c, bf, 15u);
        outColor = vec4(vec3(c) / 63.0, 1.0);
        return;
    }
    if (directMode == 0u) {
        uvec4 pixel = texelFetch(texFallback, dst, 0);
        outColor = vec4(pixel) / 255.0;
        return;
    }

    // The 3D layer is sampled on its own grid: identical to the 2D grid at
    // scale 1, denser above it. The 2D object layer is a native surface and
    // is always fetched at native coordinates, which is exactly what the DS
    // does -- only the 3D engine gains sample density here.
    int scale = int(max(scale3d, 1u));
    ivec2 hiSize = size * scale;
    ivec2 dstHi = ivec2(clamp(uv * vec2(hiSize), vec2(0.0),
                              vec2(hiSize) - vec2(1.0)));
    int sourceX = dstHi.x;
    bool outside = false;
    // RenderXPos is a native-pixel scroll, so its shift scales with the
    // raster it is being applied to.
    if (renderXPos != 0u) {
        if ((renderXPos & 0x100u) != 0u) {
            int shift = min(int(outputWidth), 512 - int(renderXPos)) * scale;
            outside = dstHi.x < shift;
            sourceX = dstHi.x - shift;
        } else {
            sourceX = dstHi.x + int(renderXPos) * scale;
            outside = sourceX >= int(outputWidth) * scale;
        }
    }
    uvec3 color3d = uvec3(0u);
    uint alpha3d = 0u;
    if (!outside) {
        if (scale > 1) {
            vec4 hi = texelFetch(tex3dHi, ivec2(sourceX, dstHi.y), 0);
            // The final pass writes the two surfaces in opposite channel
            // order: FinalFB takes red from the packed colour's bits 16..23
            // and blue from bits 0..5, while LowResFB (which this presenter
            // was written against) takes red from bits 0..7. Swizzle back
            // rather than reordering either write, so the native readback
            // every faithful consumer reads stays exactly as it is.
            color3d = uvec3(round(hi.bgr * 63.0));
            alpha3d = uint(round(hi.a * 31.0));
        } else {
            uvec4 pixel3d = texelFetch(tex3d, ivec2(sourceX, dstHi.y), 0);
            color3d = pixel3d.rgb & uvec3(63u);
            alpha3d = pixel3d.a & 31u;
        }
    }
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
    glGenTextures(2, g_hd_top_texture);
    glGenTextures(2, g_hd_below_texture);
    return true;
}

void stop_presenter()
{
    glDeleteTextures(2, g_hd_below_texture);
    glDeleteTextures(2, g_hd_top_texture);
    glDeleteTextures(2, g_object_texture);
    glDeleteTextures(2, g_fallback_texture);
    if (g_present_vao) glDeleteVertexArrays(1, &g_present_vao);
    if (g_present_program) glDeleteProgram(g_present_program);
    g_object_texture[0] = g_object_texture[1] = 0;
    g_fallback_texture[0] = g_fallback_texture[1] = 0;
    g_hd_top_texture[0] = g_hd_top_texture[1] = 0;
    g_hd_below_texture[0] = g_hd_below_texture[1] = 0;
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

// The HD layer surfaces carry a 24-bit colour plus a packed 32-bit key, so
// they need a wider integer format than the RGBA8UI OBJ surface.
void configure_layer_texture(GLuint texture, int width)
{
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG32UI, width, 192, 0,
                 GL_RG_INTEGER, GL_UNSIGNED_INT, nullptr);
}

void release_host_objects()
{
    if (g_context && g_window) {
        SDL_GL_MakeCurrent(g_window, g_context);
        stop_presenter();
    }
    if (g_context) nds_texture_upscale_shutdown();
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
    // Must precede the renderer: the factor sizes every texture-array
    // allocation and the cache's free-list is derived from it. A failure here
    // resets the factor to 1 rather than aborting, so the renderer still
    // starts with native textures.
    nds_texture_upscale_init();
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
            configure_layer_texture(g_hd_top_texture[i], width);
            configure_layer_texture(g_hd_below_texture[i], width);
        }
        g_texture_width = width;
    }
    const unsigned buffer = g_present_buffer;
    g_present_buffer ^= 1u;

    // HD outranks the OBJ-only direct path: it covers the same scenes plus
    // every one carrying BG layers, and it is the only path that can consume
    // the high-resolution 3D surface. It stands down whenever the compositor
    // did not emit surfaces for this frame (native width, engine B on top,
    // unsupported scene, or skybox repair).
    NdsGpu2dHdFrame hd{};
    const unsigned int hires_texture = nds_gpu3d_compute_output_texture_hires();
    const bool hd_active =
        hires_texture != 0u && nds_gpu2d_hd_frame(&hd) &&
        hd.width == width && hd.top_pixels && hd.below_pixels;

    unsigned long long start = SDL_GetPerformanceCounter();
    if (hd_active) {
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D, g_hd_top_texture[buffer]);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, 192,
                        GL_RG_INTEGER, GL_UNSIGNED_INT, hd.top_pixels);
        glActiveTexture(GL_TEXTURE5);
        glBindTexture(GL_TEXTURE_2D, g_hd_below_texture[buffer]);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, 192,
                        GL_RG_INTEGER, GL_UNSIGNED_INT, hd.below_pixels);
        glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
    } else if (direct_frame) {
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
    const PresentViewport viewport =
        fit_present_viewport(drawable_width, drawable_height, width, 192);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    glViewport(viewport.x, viewport.y, viewport.width, viewport.height);
    glUseProgram(g_present_program);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, g_object_texture[buffer]);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, g_fallback_texture[buffer]);
    glUniform1ui(0, direct_frame ? 1u : 0u);
    glUniform1ui(1, static_cast<GLuint>(width));
    glUniform1ui(11, hd_active ? 1u : 0u);
    if (hd_active) {
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, hires_texture);
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D, g_hd_top_texture[buffer]);
        glActiveTexture(GL_TEXTURE5);
        glBindTexture(GL_TEXTURE_2D, g_hd_below_texture[buffer]);
        glUniform1ui(10, nds_gpu3d_internal_scale());
        glUniform1ui(12, hd.order_3d);
        glUniform1ui(2, hd.render_xpos);
        glUniform1ui(4, hd.bldcnt);
        glUniform1ui(5, hd.eva);
        glUniform1ui(6, hd.evb);
        glUniform1ui(7, hd.evy);
        glUniform1ui(8, hd.master_bright);
        glUniform1ui(9, hd.priority_3d);
        glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
    } else if (direct_frame) {
        // Sample the high-resolution surface only when one actually exists;
        // the accessor returns zero at scale 1 or under the soft renderer, in
        // which case the presenter stays on the native integer path.
        const unsigned int hires = nds_gpu3d_compute_output_texture_hires();
        const unsigned scale =
            hires ? nds_gpu3d_internal_scale() : 1u;
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, nds_gpu3d_compute_output_texture());
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, hires);
        glUniform1ui(10, static_cast<GLuint>(scale));
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
