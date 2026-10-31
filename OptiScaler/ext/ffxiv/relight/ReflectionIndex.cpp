#include "pch.h"

#include "ext/ffxiv/relight/Inject.h"

#include "ext/ffxiv/diag/ShaderBindings.h" // the DXBC RDEF reader the developer draw diagnostic already uses

#include <string_view>

// Contact shadows: the reflection index (ext/ffxiv/relight/Inject.h). It reads a pixel shader's DXBC bytecode and remembers the slots the
// four names this feature binds by are declared at; the shader-creation hook fills a table of the results (Inject.cpp), and a draw is a sun
// light draw when its shader is in that table. Pure apart from the RDEF reader, so a host test compiles a shader and checks the answer.
//
// The names arrive as the engine's compiler writes them: the sun mask and the depth are textures of an effect variable, so their reflected
// names carry the effect compiler's type suffix (`g_SamplerShadow.T`, `g_SamplerDepth.T`), while the two constant buffers keep their plain
// names. The comparison strips that suffix (diag::ResourceBaseName), otherwise the mask and the depth would never match and the draw would
// look like a shader without them.
namespace ext::ffxiv::relight
{
Layout LayoutOfShader(const void* code, size_t size)
{
    Layout layout;
    if (code == nullptr || size < 32)
        return layout;
    ext::ffxiv::diag::ShaderBinding parsed[ext::ffxiv::diag::kMaxBindings];
    const size_t count = ext::ffxiv::diag::ParseBindings(code, size, parsed, ext::ffxiv::diag::kMaxBindings);
    bool hasMask = false, hasDepth = false, hasLight = false, hasCamera = false;
    for (size_t i = 0; i < count; ++i)
    {
        const ext::ffxiv::diag::ShaderBinding& bind = parsed[i];
        const std::string_view name =
            ext::ffxiv::diag::ResourceBaseName(std::string_view(bind.name, bind.nameLength));
        if (bind.type == ext::ffxiv::diag::kBindTexture)
        {
            if (name == kMaskSampler)
            {
                layout.maskSlot = bind.bindPoint;
                hasMask = true;
            }
            else if (name == kDepthSampler)
            {
                layout.depthSlot = bind.bindPoint;
                hasDepth = true;
            }
        }
        else if (bind.type == ext::ffxiv::diag::kBindConstantBuffer)
        {
            if (name == kLightParamBuffer)
            {
                layout.lightParamSlot = bind.bindPoint;
                hasLight = true;
            }
            else if (name == kCameraParamBuffer)
            {
                layout.cameraParamSlot = bind.bindPoint;
                hasCamera = true;
            }
        }
    }
    layout.found = (hasMask ? 1u : 0u) | (hasDepth ? 2u : 0u) | (hasLight ? 4u : 0u) | (hasCamera ? 8u : 0u);
    layout.ok = layout.found == 15u;
    return layout;
}

} // namespace ext::ffxiv::relight
