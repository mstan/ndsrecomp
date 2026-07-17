// GPL-3.0-or-later; see ComputeHost.h and THIRD_PARTY_ATTRIBUTION.md.
#include "ComputeHost.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "glad/glad.h"
#include "gpu3d.h"

namespace
{
SDL_Window* g_window = nullptr;
SDL_GLContext g_context = nullptr;
bool g_owns_video = false;
bool g_active = false;

void release_host_objects()
{
    if (g_context) SDL_GL_DeleteContext(g_context);
    if (g_window) SDL_DestroyWindow(g_window);
    if (g_owns_video) SDL_QuitSubSystem(SDL_INIT_VIDEO);
    g_window = nullptr;
    g_context = nullptr;
    g_owns_video = false;
    g_active = false;
}
}

bool nds_compute_host_start()
{
    const char* selection = std::getenv("NDS_3D_RENDERER");
    if (!selection || std::strcmp(selection, "compute") != 0) return true;
    if (g_active) return true;

    SDL_SetMainReady();
    g_owns_video = (SDL_WasInit(SDL_INIT_VIDEO) == 0);
    if (g_owns_video && SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "[gpu3d] SDL video init failed: %s\n",
                     SDL_GetError());
        return false;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                        SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 0);
    g_window = SDL_CreateWindow(
        "ndsrecomp compute context", SDL_WINDOWPOS_UNDEFINED,
        SDL_WINDOWPOS_UNDEFINED, 1, 1,
        SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!g_window) {
        std::fprintf(stderr, "[gpu3d] compute GL window failed: %s\n",
                     SDL_GetError());
        SDL_GL_ResetAttributes();
        release_host_objects();
        return false;
    }
    g_context = SDL_GL_CreateContext(g_window);
    SDL_GL_ResetAttributes();
    if (!g_context || SDL_GL_MakeCurrent(g_window, g_context) != 0) {
        std::fprintf(stderr, "[gpu3d] compute GL 4.3 context failed: %s\n",
                     SDL_GetError());
        release_host_objects();
        return false;
    }
    if (!gladLoadGLLoader(
            reinterpret_cast<GLADloadproc>(SDL_GL_GetProcAddress)) ||
        !GLAD_GL_VERSION_4_3) {
        std::fprintf(stderr, "[gpu3d] OpenGL 4.3 unavailable\n");
        release_host_objects();
        return false;
    }
    std::fprintf(stderr, "[gpu3d] OpenGL %s / %s\n",
                 reinterpret_cast<const char*>(glGetString(GL_VERSION)),
                 reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
    if (!nds_gpu3d_use_compute_renderer()) {
        std::fprintf(stderr, "[gpu3d] compute renderer init failed\n");
        release_host_objects();
        return false;
    }
    g_active = true;
    std::fprintf(stderr, "[gpu3d] renderer: compute (experimental)\n");
    return true;
}

bool nds_compute_host_make_current()
{
    return !g_active || SDL_GL_MakeCurrent(g_window, g_context) == 0;
}

void nds_compute_host_stop()
{
    if (!g_active) {
        release_host_objects();
        return;
    }
    SDL_GL_MakeCurrent(g_window, g_context);
    nds_gpu3d_use_soft_renderer(false);
    release_host_objects();
}

bool nds_compute_host_active()
{
    return g_active;
}
