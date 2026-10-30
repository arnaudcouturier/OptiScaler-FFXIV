#pragma once
// Constant buffer of the HDR10 converter (OptiScaler/ext/hdr/shaders/Hdr10.hlsl, cbuffer Settings): 20 dwords,
// 80 bytes. The last row (continuous composition flag, UI expansion, padding) reads zero from an old 64-byte buffer,
// so every existing caller keeps the legacy two-rule composition until [HDR] LegacyComposition is switched off.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace ext::hdr
{
// Raw inputs: config values as bound, plus what the scene capture verified this frame.
struct HdrConstantSource
{
    float paper = 203.0f, peak = 1100.0f, expansion = 0.5f;
    float contrast = 1.0f, saturation = 1.0f, vibrance = 0.0f;
    bool legacyComposition = true; // [HDR] LegacyComposition: true keeps the two-rule composition
    bool uiExpansionSet = false;   // [HDR] UiExpansion: unset follows HighlightExpansion
    float uiExpansion = 0.5f;
    bool sceneRequested = false; // [HDR] Mode == 1
    bool sceneAvailable = false; // the capture verified a scene and a reference
    bool reshadeHighlights = false;
    bool referenceSrgb = false, hasPreviewMask = false;
    float rect[4] = { 0, 0, 1, 1 };
    unsigned width = 0, height = 0;
};

struct HdrConstants
{
    uint32_t w = 0, h = 0;
    float paper = 203.0f, peak = 1100.0f, expansion = 0.5f, contrast = 1.0f, saturation = 1.0f, vibrance = 0.0f;
    uint32_t scene = 0, reshadeHighlights = 0, referenceSrgb = 0, hasPreviewMask = 0;
    float rect[4] = { 0, 0, 1, 1 };
    uint32_t continuous = 0; // 1 selects the continuous law in the shader
    float uiExpansion = 0.5f;
    uint32_t reserved[2] = { 0, 0 }; // always zero: an old 64-byte buffer leaves the whole flag row at zero
};
static_assert(sizeof(HdrConstants) == 80, "the shader cbuffer is 20 dwords");
static_assert(offsetof(HdrConstants, rect) == 48, "SceneRect follows the 12 shared dwords");
static_assert(offsetof(HdrConstants, continuous) == 64, "the flag row follows SceneRect");

inline HdrConstants BuildHdrConstants(const HdrConstantSource& s)
{
    const auto finite = [](float v, float fallback) { return std::isfinite(v) ? v : fallback; };
    HdrConstants k {};
    k.w = s.width;
    k.h = s.height;
    k.peak = std::clamp(finite(s.peak, 1100.0f), 400.0f, 4000.0f);
    k.paper = std::clamp(finite(s.paper, 203.0f), 80.0f, std::min(400.0f, k.peak));
    k.expansion = std::clamp(finite(s.expansion, 0.5f), 0.0f, 1.0f);
    k.contrast = std::clamp(finite(s.contrast, 1.0f), 0.5f, 1.5f);
    k.saturation = std::clamp(finite(s.saturation, 1.0f), 0.0f, 2.0f);
    k.vibrance = std::clamp(finite(s.vibrance, 0.0f), -1.0f, 1.0f);
    k.scene = (s.sceneRequested && s.sceneAvailable) ? 1u : 0u;
    k.reshadeHighlights = s.reshadeHighlights ? 1u : 0u;
    k.referenceSrgb = s.referenceSrgb ? 1u : 0u;
    k.hasPreviewMask = s.hasPreviewMask ? 1u : 0u;
    for (int i = 0; i < 4; ++i)
        k.rect[i] = s.rect[i];
    k.continuous = s.legacyComposition ? 0u : 1u;
    k.uiExpansion = s.uiExpansionSet ? std::clamp(finite(s.uiExpansion, k.expansion), 0.0f, 1.0f) : k.expansion;
    if (s.sceneRequested && !s.sceneAvailable)
    {
        // No verified scene: the whole frame stays at paper white, the interface included, as before.
        k.expansion = 0.0f;
        k.uiExpansion = 0.0f;
    }
    return k;
}

// One snapshot of the UI expansion setting: the menu thread's Reset can clear it between the has_value and value reads.
inline std::pair<bool, float> ResolveUiExpansion(const std::optional<float>& ui, float fallback)
{
    return ui.has_value() ? std::pair<bool, float> { true, *ui } : std::pair<bool, float> { false, fallback };
}

// The preview-mask transitions Convert records around its dispatch: into NON_PIXEL_SHADER_RESOURCE before the dispatch,
// back to COMMON after. Both or neither; a scene without a mask, or no scene at all, records none.
inline bool NeedsPreviewMaskBarrier(const HdrConstants& k) { return k.scene != 0 && k.hasPreviewMask != 0; }
} // namespace ext::hdr
