#pragma once
// S10 package A: the pixel-shader sets the boundary detector recognises (docs/HUDLESS.md section 10).
//
// Pure and dependency free (no pch, no D3D) so tests/ext/ffxiv/ui builds it standalone.
//
// Stage numbers follow ext::hdr::FfxivSceneHdr::Stage (tonemap 1 ... fullscreen gamma 5) and FfxivHdrPreview::Tag
// (UI only = 6). The CRC values below are MIRRORS of those two tables: the scene-HDR header is too heavy to include here,
// so D1 cross-checks every shader on first sight against the real private-data tag (shaders.csv column `tag_agree`).

#include <array>
#include <cstdint>

namespace ext::ffxiv::ui
{
// Stage of a pixel shader in the game's frame. 1..5 are scene/post stages, kUiStage is "UI only", kStageUnknown means the
// shader was created before the hook (no bytecode, so no CRC) and its stage cannot be told.
inline constexpr uint8_t kStageNone = 0;
inline constexpr uint8_t kUiStage = 6;
inline constexpr uint8_t kStageUnknown = 255;

// The seven CRCs of the HDR "UI only" stage (FfxivHdrPreview::Tag). The Companion's four are a subset. They are
// transmittance-related and may not cover every UI draw: D1 measures that.
inline constexpr std::array<uint32_t, 7> kUiCrcs {
    3865947726u, 2966694105u, 3759127293u, 1898885941u, 4076256744u, 3915247300u, 1823062889u,
};

constexpr bool IsUiCrc(uint32_t crc)
{
    for (uint32_t c : kUiCrcs)
        if (c == crc)
            return true;
    return false;
}

// Scene/post stage 1..5 of a CRC, 0 when the shader is not one of the five recognised stages.
constexpr uint8_t SceneStageOfCrc(uint32_t crc)
{
    switch (crc)
    {
    case 0x85e777efu:
        return 1; // tonemap
    case 0xf8f57f0au:
        return 2; // post-tonemap, pre-LUT
    case 0x27ebc404u:
        return 3; // LUT
    case 0x1f264d17u:
        return 4; // LUT
    case 0xf6e81a1bu:
        return 5; // fullscreen gamma
    default:
        return 0;
    }
}

// Stage of a CRC as the draw-time tracker sees it: 1..5, kUiStage, or kStageNone.
constexpr uint8_t StageOfCrc(uint32_t crc)
{
    if (const uint8_t scene = SceneStageOfCrc(crc))
        return scene;
    return IsUiCrc(crc) ? kUiStage : kStageNone;
}
} // namespace ext::ffxiv::ui
