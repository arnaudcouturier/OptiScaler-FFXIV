#pragma once
// The NGX flags and parameters of one preview feature, as pure data: the same sequence is applied to the real NGX
// parameter object (CharaDlss.cpp) and to a recording fake in the tests. Parameter names are the NGX SDK's; CharaDlss.cpp
// checks every name against the SDK macro at compile time.
//
// Feature: DLSS super resolution, render size W x H, output 2W x 2H, performance preset "MaxPerf" (0). Create flags: motion
// vectors at render resolution (0x2), automatic exposure (0x40), inverted depth (0x8) when the scene uses a reversed depth
// buffer. Not HDR, vectors not flagged as jittered, no sharpening, no alpha upscaling.
// Render preset hints: the same six DLSS.Hint.Render.Preset.* values as the game's own DLSS (see ComposePresetHints).
// Evaluate: colour, depth, motion vectors and output resources; the pass jitter in pixels (no sign flip); motion scale equal
// to the render size; no exposure texture and no masks; the history is reset when the slot never evaluated or its last
// evaluation is older than 250 ms.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "ext/nr/presr/PreSrModelHints.h"

namespace ext::ffxiv::chara::dlss::ngx
{
inline constexpr uint32_t kFlagMotionLowRes = 0x2;
inline constexpr uint32_t kFlagDepthInverted = 0x8;
inline constexpr uint32_t kFlagAutoExposure = 0x40;
inline constexpr int kPerfQualityMaxPerf = 0;
inline constexpr uint32_t kOutputScale = 2;

constexpr uint32_t CreateFlags(bool reversedZ)
{
    return kFlagMotionLowRes | kFlagAutoExposure | (reversedZ ? kFlagDepthInverted : 0u);
}

// Parameter names (NGX SDK strings).
inline constexpr const char* kWidth = "Width";
inline constexpr const char* kHeight = "Height";
inline constexpr const char* kOutWidth = "OutWidth";
inline constexpr const char* kOutHeight = "OutHeight";
inline constexpr const char* kPerfQuality = "PerfQualityValue";
inline constexpr const char* kCreateFlags = "DLSS.Feature.Create.Flags";
inline constexpr const char* kColor = "Color";
inline constexpr const char* kOutput = "Output";
inline constexpr const char* kDepth = "Depth";
inline constexpr const char* kMotionVectors = "MotionVectors";
inline constexpr const char* kJitterX = "Jitter.Offset.X";
inline constexpr const char* kJitterY = "Jitter.Offset.Y";
inline constexpr const char* kMvScaleX = "MV.Scale.X";
inline constexpr const char* kMvScaleY = "MV.Scale.Y";
inline constexpr const char* kSharpness = "Sharpness";
inline constexpr const char* kPreExposure = "DLSS.Pre.Exposure";
inline constexpr const char* kExposureScale = "DLSS.Exposure.Scale";
inline constexpr const char* kSubrectWidth = "DLSS.Render.Subrect.Dimensions.Width";
inline constexpr const char* kSubrectHeight = "DLSS.Render.Subrect.Dimensions.Height";
inline constexpr const char* kReset = "Reset";

// ---- Render preset hints --------------------------------------------------------------------------------------------------
// The isolated feature gets the same transformer-model preset hints as the game's own DLSS (DLSS.Hint.Render.Preset.*), so the
// preview and the main image use one model. The six values are composed from what the main DLSS path reads: the configuration's
// override ([DLSS] RenderPresetOverride with RenderPresetForAll or one key per quality level) when it is on, otherwise the values the
// game itself requested for its own feature. The legacy "latest" sentinel is mapped to the runtime default.
inline constexpr size_t kPresetHintCount = 6;
inline constexpr const char* kPresetHintNames[kPresetHintCount] = {
    "DLSS.Hint.Render.Preset.DLAA",         "DLSS.Hint.Render.Preset.UltraQuality",      "DLSS.Hint.Render.Preset.Quality",
    "DLSS.Hint.Render.Preset.Balanced",     "DLSS.Hint.Render.Preset.Performance",       "DLSS.Hint.Render.Preset.UltraPerformance",
};

struct PresetHints
{
    uint32_t value[kPresetHintCount] {};
    bool operator==(const PresetHints&) const = default;
};

// What the main DLSS path knows, in the order of kPresetHintNames.
struct PresetSource
{
    bool overrideOn = false;                         // RenderPresetOverride
    std::optional<uint32_t> forAll;                  // RenderPresetForAll (wins over the per-quality keys)
    std::optional<uint32_t> perQuality[kPresetHintCount]; // RenderPresetDLAA ... RenderPresetUltraPerformance
    uint32_t game[kPresetHintCount] {};              // the values the game requested for its own feature
};

inline PresetHints ComposePresetHints(const PresetSource& s)
{
    std::array<uint32_t, kPresetHintCount> game {};
    std::array<std::optional<uint32_t>, kPresetHintCount> perQuality {};
    for (size_t i = 0; i < kPresetHintCount; ++i)
    {
        game[i] = s.game[i];
        perQuality[i] = s.perQuality[i];
    }
    const DlssModelHints::Settings resolved = DlssModelHints::Resolve(0, game, s.overrideOn, s.forAll, perQuality);
    PresetHints hints;
    for (size_t i = 0; i < kPresetHintCount; ++i)
        hints.value[i] = resolved.presets[i];
    return hints;
}

// `S` provides SetUInt(name, uint32_t).
template <class S> void ApplyPresetHints(S& s, const PresetHints& hints)
{
    for (size_t i = 0; i < kPresetHintCount; ++i)
        s.SetUInt(kPresetHintNames[i], hints.value[i]);
}

// `S` provides SetUInt(name, uint32_t), SetInt(name, int), SetFloat(name, float).
template <class S> void ApplyCreate(S& s, uint32_t width, uint32_t height, bool reversedZ)
{
    s.SetUInt(kWidth, width);
    s.SetUInt(kHeight, height);
    s.SetUInt(kOutWidth, width * kOutputScale);
    s.SetUInt(kOutHeight, height * kOutputScale);
    s.SetInt(kPerfQuality, kPerfQualityMaxPerf);
    s.SetUInt(kCreateFlags, CreateFlags(reversedZ));
}

struct EvaluateInputs
{
    uint32_t width = 0;
    uint32_t height = 0;
    float jitterX = 0.0f; // pixels, as applied to the raster viewport
    float jitterY = 0.0f;
    bool reset = false;
};

// `R` is the resource handle type of the NGX parameter object (ID3D11Resource*). `S` additionally provides
// SetResource(name, R).
template <class S, class R> void ApplyEvaluate(S& s, const EvaluateInputs& in, R color, R output, R depth, R motionVectors)
{
    s.SetResource(kColor, color);
    s.SetResource(kOutput, output);
    s.SetResource(kDepth, depth);
    s.SetResource(kMotionVectors, motionVectors);
    s.SetFloat(kJitterX, in.jitterX);
    s.SetFloat(kJitterY, in.jitterY);
    s.SetFloat(kMvScaleX, static_cast<float>(in.width));
    s.SetFloat(kMvScaleY, static_cast<float>(in.height));
    s.SetFloat(kSharpness, 0.0f);
    s.SetFloat(kPreExposure, 1.0f);
    s.SetFloat(kExposureScale, 1.0f);
    s.SetUInt(kSubrectWidth, in.width);
    s.SetUInt(kSubrectHeight, in.height);
    s.SetInt(kReset, in.reset ? 1 : 0);
}
} // namespace ext::ffxiv::chara::dlss::ngx
