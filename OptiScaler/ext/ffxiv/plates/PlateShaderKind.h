#pragma once
// Which pixel shader a draw is using, for the plate classifier. The four nameplate variants are the four "UI only"
// CRCs of the UI shader catalog whose input layouts are the game's plate layouts (0, 4, 5 and 6), named here by the
// kind the classifier asks for (1, 5, 6 and 7 = layout + 1).
//
// The kind is stamped on the shader object at creation, for every pixel shader and whether or not the key is on: a key
// switched on in the middle of a session must find the shaders it needs already tagged. The stamp is our own private
// data, so no other feature's stamp can be mistaken for it.

#include "ext/ffxiv/ui/UiShaderCatalog.h"

#include <d3d11.h>

#include <cstddef>
#include <cstdint>

namespace ext::ffxiv::plates
{
inline constexpr GUID pixelShaderKindTag {0x6e0f4b2a,0x8d3c,0x4d21,{0x9a,0x57,0x2b,0x0c,0x44,0x1f,0x87,0x30}};

// 1 / 5 / 6 / 7 for the four nameplate pixel-shader variants, 0 for every other shader.
constexpr uint32_t PlateShaderKind(uint32_t crc)
{
    switch (crc)
    {
    case 3865947726u:
        return 1; // layout 0
    case 1823062889u:
        return 5; // layout 4
    case 3759127293u:
        return 6; // layout 5
    case 2966694105u:
        return 7; // layout 6
    default:
        return 0;
    }
}

// True for the CRCs above; every call site gates on this before touching the shader object.
constexpr bool IsNameplatePixelShader(uint32_t crc) { return PlateShaderKind(crc) != 0; }

// Stamps the kind on a newly created pixel shader. A shader that is not a nameplate variant is left untouched.
inline void OnPixelShaderCreated(ID3D11PixelShader* shader, uint32_t crc)
{
    const uint32_t kind = PlateShaderKind(crc);
    if (shader == nullptr || kind == 0)
        return;
    shader->SetPrivateData(pixelShaderKindTag, sizeof(kind), &kind);
}

// The kind of the pixel shader object a draw is using, 0 when it is not one of the nameplate variants (or was created
// before the hook). No call into the game, one GetPrivateData on the object.
inline uint32_t KindOf(ID3D11PixelShader* shader)
{
    if (shader == nullptr)
        return 0;
    uint32_t kind = 0;
    UINT size = sizeof(kind);
    if (FAILED(shader->GetPrivateData(pixelShaderKindTag, &size, &kind)) || size != sizeof(kind))
        return 0;
    return kind;
}
} // namespace ext::ffxiv::plates
