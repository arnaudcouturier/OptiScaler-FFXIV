#pragma once
// Stand-in for the product config in the screen-space reflections tests: the five reflection keys, the engine pass overrides' master and the
// character-preview keys the draw seam's own inline gate reads, all at the shipped defaults, so the module's publish path and its draw path can be
// driven without the ini reader.
#include "ext/ExtFeatures.h" // ext::F, ext::SetActive / Active: the real, header-only feature bits (the real ExtConfig.h includes it too)

#include <cstdint>
#include <string>

namespace ext
{
template <class T> struct OptionalValue
{
    T value {};
    T value_or_default() const { return value; }
};

// The character preview's keys: only the ones its inline gate reads, at the defaults a session with the preview off has.
struct CharaFfxivConfig
{
    OptionalValue<bool> previewAA { false };
    OptionalValue<bool> previewDlss { false };
    OptionalValue<bool> previewNr { false };
    OptionalValue<bool> previewProbe { false };
    OptionalValue<bool> previewDebugFlood { false };
};

struct FfxivConfig
{
    OptionalValue<bool> screenSpaceReflections { false }; // the card's master switch
    OptionalValue<bool> screenSpaceReflectionsDiagnostic { false };
    OptionalValue<int32_t> screenSpaceReflectionsSteps { 48 };
    OptionalValue<float> screenSpaceReflectionsThickness { 0.3f };
    OptionalValue<float> screenSpaceReflectionsBlend { 1.0f };
    OptionalValue<float> screenSpaceReflectionsFade { 0.1f };
    OptionalValue<bool> enginePassOverrides { false }; // the master switch of the module next door, which this card publishes through
    OptionalValue<std::string> drawDiagnosticsDump { "off" };
    // The screen-space light bounce's keys (the chain-order test links the real stage with its key off and on): at the shipped defaults.
    OptionalValue<bool> gi { false };
    OptionalValue<int32_t> giRays { 4 };
    OptionalValue<int32_t> giSteps { 16 };
    OptionalValue<float> giRange { 5.0f };
    OptionalValue<float> giStrength { 0.55f };
    OptionalValue<std::string> giResolution { "half" };
    static inline void (*giOnLoaded)(bool) = nullptr; // end of every config load (the stage's OnConfigLoaded)
    CharaFfxivConfig chara;
    static inline void (*screenSpaceReflectionsOnLoaded)(bool) = nullptr; // end of every config load (the card's Publish)
    static inline void (*screenSpaceReflectionsDiagnosticOnLoaded)(bool) = nullptr;
};

struct ExtConfig
{
    FfxivConfig ffxiv;
};

inline ExtConfig& Cfg()
{
    static ExtConfig config;
    return config;
}
} // namespace ext
