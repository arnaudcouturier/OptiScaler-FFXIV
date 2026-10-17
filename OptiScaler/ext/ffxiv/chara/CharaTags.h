#pragma once
// Pixel-shader identification for the character preview feature. Shaders are recognised by the CRC-32 of their DXBC
// container (the same hash the other FFXIV shader tags use). The CRCs are content hashes of the game's shader bytecode:
// they survive an executable patch unless the shaders themselves change.
//
// Roles are stored as one bit mask in a single private-data value on the shader, so a draw reads one value.

#include <array>
#include <cstdint>
#include <d3d11.h>

namespace ext::ffxiv::chara
{
enum Role : uint32_t
{
    RoleNone = 0,
    RoleGeometry = 1,      // a pixel shader of the preview's G-buffer draws
    RoleFinisher = 2,      // the pixel shader of the preview's final composition draw
    RoleEquipDisplay = 4,  // the pixel shader that shows the equipment preview image in the UI
    RoleUi = 8,            // one of the UI shaders that show a preview image (ext::hdr::FfxivHdrPreview::tag)
};

inline constexpr std::array<uint32_t, 7> kGeometryCrcs { 0x68029DFCu, 0x05626DF1u, 0x478C1594u, 0x67AD01CAu,
                                                         0xA657BFE5u, 0xCC2C6A71u, 0xD2AEC0AAu };
inline constexpr uint32_t kFinisherCrc = 0xFDB2F4FCu;
inline constexpr uint32_t kEquipDisplayCrc = 0x7B40E31Au;

// Role bits of a shader CRC (RoleUi is added by the creation hook from the existing UI tag, not here).
constexpr uint32_t RoleOfCrc(uint32_t crc)
{
    for (const uint32_t g : kGeometryCrcs)
    {
        if (g == crc)
            return RoleGeometry;
    }
    if (crc == kFinisherCrc)
        return RoleFinisher;
    if (crc == kEquipDisplayCrc)
        return RoleEquipDisplay;
    return RoleNone;
}

// Private data on an ID3D11PixelShader: a uint32_t bit mask of Role values.
inline constexpr GUID kRoleTag { 0x6f0c9a35, 0x1b7e, 0x4d2a, { 0x9c, 0x41, 0x8e, 0x3d, 0x57, 0xa2, 0xb6, 0x10 } };

// Role bits of a shader (0 when untagged or null). One private-data read.
inline uint32_t RoleOf(ID3D11PixelShader* shader)
{
    if (shader == nullptr)
        return RoleNone;

    uint32_t roles = 0;
    UINT size = sizeof(roles);
    if (FAILED(shader->GetPrivateData(kRoleTag, &size, &roles)) || size != sizeof(roles))
        return RoleNone;
    return roles;
}

// Writes the role bits (a no-op for 0). Returns whether anything was written.
inline bool SetRoles(ID3D11PixelShader* shader, uint32_t roles)
{
    if (shader == nullptr || roles == RoleNone)
        return false;
    return SUCCEEDED(shader->SetPrivateData(kRoleTag, sizeof(roles), &roles));
}
} // namespace ext::ffxiv::chara
