/*
    Runner-owned GLCompositor adapter for melonDS's verbatim ComputeRenderer.
    The runner consumes the low-resolution PBO through its existing GPU2D/SDL
    compositor, so this adapter intentionally owns no full-screen compositor.

    GPL-3.0-or-later; see THIRD_PARTY_ATTRIBUTION.md.
*/
#pragma once

#include <optional>
#include <string>

#include "OpenGLSupport.h"

namespace melonDS
{
class GPU;
class Renderer3D;

class GLCompositor
{
public:
    static std::optional<GLCompositor> New() noexcept;
    GLCompositor() = default;
    GLCompositor(const GLCompositor&) = delete;
    GLCompositor& operator=(const GLCompositor&) = delete;
    GLCompositor(GLCompositor&&) noexcept = default;
    GLCompositor& operator=(GLCompositor&&) noexcept = default;
    ~GLCompositor() = default;

    void SetScaleFactor(int scale) noexcept { Scale = scale; }
    [[nodiscard]] int GetScaleFactor() const noexcept { return Scale; }
    void Stop(const GPU&) noexcept {}
    void RenderFrame(const GPU&, Renderer3D&) noexcept {}
    void BindOutputTexture(int) {}

private:
    int Scale = 0;
};
}
