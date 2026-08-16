#include "GPU3D_TexcacheOpenGL.h"

// ndsrecomp: optional texture upscaling. Project-owned, MIT; see
// runner/src/melonds_compute/TextureUpscale.h.
#include "../../src/melonds_compute/TextureUpscale.h"

namespace melonDS
{

GLuint TexcacheOpenGLLoader::GenerateTexture(u32 width, u32 height, u32 layers)
{
    GLuint texarray;
    // ndsrecomp: storage is allocated at the upscale factor; the sampled
    // coordinates stay normalized, so the renderer's texel addressing and
    // the DS wrap modes are unaffected by the larger backing store.
    const u32 factor = (u32)nds_texture_upscale_factor();
    glGenTextures(1, &texarray);
    glBindTexture(GL_TEXTURE_2D_ARRAY, texarray);
    glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA8UI,
        width * factor, height * factor, layers);
    return texarray;
}

void TexcacheOpenGLLoader::UploadTexture(GLuint handle, u32 width, u32 height, u32 layer, void* data)
{
    // ndsrecomp: filter into the layer when upscaling is active. A failed
    // dispatch falls through to the native upload below, which is correct
    // only at factor 1 -- so the factor is forced to 1 on any init failure.
    if (nds_texture_upscale_dispatch(handle, width, height, layer, data))
        return;
    glBindTexture(GL_TEXTURE_2D_ARRAY, handle);
    glTexSubImage3D(GL_TEXTURE_2D_ARRAY,
        0, 0, 0, layer,
        width, height, 1,
        GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, data);
}

void TexcacheOpenGLLoader::DeleteTexture(GLuint handle)
{
    glDeleteTextures(1, &handle);
}

}