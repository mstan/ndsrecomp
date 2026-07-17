/* GPL-3.0-or-later; see GPU_OpenGL.h and THIRD_PARTY_ATTRIBUTION.md. */
#include "GPU_OpenGL.h"

namespace melonDS
{
std::optional<GLCompositor> GLCompositor::New() noexcept
{
    return GLCompositor{};
}
}
