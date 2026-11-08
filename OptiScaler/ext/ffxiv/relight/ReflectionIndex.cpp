#include "pch.h"

#include "ext/ffxiv/relight/Inject.h"

#include "ext/ffxiv/diag/ShaderBindings.h" // the DXBC RDEF reader the developer draw diagnostic already uses

#include <string_view>

// Contact shadows: the reflection index (ext/ffxiv/relight/Inject.h). It reads a pixel shader's DXBC bytecode and remembers the slots the
// four names this feature binds by are declared at; the shader-creation hook fills a table of the results (Inject.cpp), and a draw is a sun
// light draw when its shader is in that table. Pure apart from the RDEF reader, so a host test compiles a shader and checks the answer.
//
// The names arrive as the engine's compiler writes them: the sun mask and the depth are textures of an effect variable, so their reflected
// names carry the effect compiler's type suffix (`g_SamplerShadowMask.T`, `g_SamplerDepth.T`), while the two constant buffers keep their plain
// names. The comparison strips that suffix (diag::ResourceBaseName), otherwise the mask and the depth would never match and the draw would
// look like a shader without them. The mask answers to either name the two measured builds use (ContactShadowsRules.h's IsMaskName): a shader
// that declares both is read at the first one its reflection lists, which no shader of this build does (the 16 that declare `g_SamplerShadow`
// declare only that one, and the light draw's binding is named `g_SamplerShadowMask` in the view names, its reflection not captured). For the
// measured family the first name is the atlas', so such a shader would be refused by the atlas' format and size - fail-open, and the feature
// would not run for it.
namespace ext::ffxiv::relight
{
Layout LayoutOfShader(const void* code, size_t size)
{
    Layout layout;
    if (code == nullptr || size < 32)
        return layout;
    ext::ffxiv::diag::ShaderBinding parsed[ext::ffxiv::diag::kMaxBindings];
    const size_t count = ext::ffxiv::diag::ParseBindings(code, size, parsed, ext::ffxiv::diag::kMaxBindings);
    // Every name is looked up by the kind it binds (diag::BindPointOf). The effect compiler writes a texture and its sampler under one
    // variable name, as `<name>.T` and `<name>.S`, and writes the sampler first, so a lookup that compared names alone would record the
    // sampler's register as the depth's - a slot that holds a different texture at the draw. The mask answers to either name the two
    // measured builds use, so it is scanned with the kind filter and the first declaring texture wins.
    using ext::ffxiv::diag::BindKind;
    bool hasMask = false;
    uint32_t mask = ext::ffxiv::diag::kNoBindPoint;
    for (size_t i = 0; i < count; ++i)
    {
        const ext::ffxiv::diag::ShaderBinding& bind = parsed[i];
        if (bind.name == nullptr || !ext::ffxiv::diag::BindingAnswers(bind, BindKind::Texture, bind.bindPoint))
            continue;
        const std::string_view name =
            ext::ffxiv::diag::ResourceBaseName(std::string_view(bind.name, bind.nameLength));
        if (!hasMask && IsMaskName(name))
        {
            mask = bind.bindPoint;
            hasMask = true;
        }
    }
    const uint32_t depth = ext::ffxiv::diag::BindPointOf(parsed, count, BindKind::Texture, kDepthSampler);
    const uint32_t light = ext::ffxiv::diag::BindPointOf(parsed, count, BindKind::ConstantBuffer, kLightParamBuffer);
    const uint32_t camera = ext::ffxiv::diag::BindPointOf(parsed, count, BindKind::ConstantBuffer, kCameraParamBuffer);
    if (mask != ext::ffxiv::diag::kNoBindPoint)
        layout.maskSlot = mask;
    if (depth != ext::ffxiv::diag::kNoBindPoint)
        layout.depthSlot = depth;
    if (light != ext::ffxiv::diag::kNoBindPoint)
        layout.lightParamSlot = light;
    if (camera != ext::ffxiv::diag::kNoBindPoint)
        layout.cameraParamSlot = camera;
    layout.found = (hasMask ? 1u : 0u) | (depth != ext::ffxiv::diag::kNoBindPoint ? 2u : 0u) |
                   (light != ext::ffxiv::diag::kNoBindPoint ? 4u : 0u) | (camera != ext::ffxiv::diag::kNoBindPoint ? 8u : 0u);
    layout.ok = layout.found == 15u;
    return layout;
}

} // namespace ext::ffxiv::relight
