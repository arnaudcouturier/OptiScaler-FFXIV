#include "pch.h"

#include "ext/ffxiv/engine/AoApply.h"

#include <d3d11.h>

// Ambient occlusion: the two things this module does to a game object - the stamp it writes on a shader, and the one pipeline change a reader
// draw gets (ext/ffxiv/engine/AoApply.h) - in a translation unit of their own. This file depends on nothing but D3D11 and the two small types
// it serves, so a device test can drive the stamp and the swap with their restore without the module's config, log and pass around them - which
// is what pins the three things that would matter in a session: the slots a shader declares survive every other shader the game creates, the
// game's own view comes back, and it comes back with the reference this module took.
namespace ext::ffxiv::engine::ao
{
namespace detail
{
// The one private-data tag this module uses on a game object: the packed word AoRules.h's PackInputs makes from the slots a shader declares.
// The game has its own tags on its own shaders, and a GUID this specific does not collide with them.
constexpr GUID kInputsTag { 0x8a2d4c17, 0x5e93, 0x4b62, { 0xb1, 0x0e, 0x2f, 0x76, 0x9a, 0x54, 0x3c, 0x81 } };

bool StampShader(ID3D11DeviceChild* shader, uint32_t packed)
{
    if (shader == nullptr || packed == 0)
        return false; // 0 is "nothing read": writing it would only make an unstamped shader indistinguishable at the same cost
    return SUCCEEDED(shader->SetPrivateData(kInputsTag, sizeof packed, &packed));
}

uint32_t ReadStamp(ID3D11DeviceChild* shader)
{
    if (shader == nullptr)
        return 0;
    uint32_t packed = 0;
    UINT bytes = sizeof packed;
    if (FAILED(shader->GetPrivateData(kInputsTag, &bytes, &packed)) || bytes != sizeof packed)
        return 0;
    return packed;
}

ID3D11ShaderResourceView* SwapSlot(ID3D11DeviceContext* context, uint32_t slot, ID3D11ShaderResourceView* replacement)
{
    if (context == nullptr || replacement == nullptr)
        return nullptr; // nothing to swap, so the caller leaves the draw as the game made it
    ID3D11ShaderResourceView* previous = nullptr;
    context->PSGetShaderResources(slot, 1, &previous);
    if (previous == nullptr)
        return nullptr; // nothing was bound there: nothing to replace and nothing to put back
    context->PSSetShaderResources(slot, 1, &replacement);
    return previous; // owned by the caller, which hands it to the guard that restores it
}
} // namespace detail

BoundReader::~BoundReader()
{
    if (_context == nullptr)
        return;
    _context->PSSetShaderResources(_slot, 1, &_original);
    _context = nullptr;
    if (_original != nullptr)
    {
        _original->Release();
        _original = nullptr;
    }
}
} // namespace ext::ffxiv::engine::ao
