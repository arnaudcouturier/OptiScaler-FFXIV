#pragma once
// The private-data tag the draw diagnostic stamps a shader's bytecode CRC under, and the one read of it. A capture's rows name a pixel shader
// by that CRC, so a module that writes a line about a draw it did not serve names the same shader the capture would: the tag is declared once,
// here, instead of being copied into every place that reads it.
//
// Not pure: it touches a D3D11 object, so it is included by the sources that have a shader at hand and never by a rules header.

#include <d3d11.h>

#include <cstdint>

namespace ext::ffxiv::diag
{
inline constexpr GUID kShaderCrcTag { 0x2d7b41c8, 0x9e04, 0x4a1d, { 0x8a, 0x3c, 0x1b, 0x6e, 0xd2, 0x57, 0x40, 0x91 } };

// The CRC the diagnostic stamped on this object, or 0 when it carries none (the diagnostic is off, or the shader was created before it started).
inline uint32_t ReadShaderCrc(ID3D11DeviceChild* shader)
{
    if (shader == nullptr)
        return 0;
    uint32_t crc = 0;
    UINT bytes = sizeof crc;
    if (FAILED(shader->GetPrivateData(kShaderCrcTag, &bytes, &crc)) || bytes != sizeof crc)
        return 0;
    return crc;
}
} // namespace ext::ffxiv::diag
