#pragma once

#include <Config.h>

#include "ext/ffxiv/chara/CharaConfig.h"

namespace ext
{
namespace config
{
class Binder;
}

struct FfxivConfig
{
    CustomOptional<bool> jitterProbe { false };      // [FFXIV] JitterProbe: read-only log of the game's applied versus reported jitter (ext/ffxiv/jitter)
    CustomOptional<bool> jitterCorrection { false }; // [FFXIV] JitterCorrection: experimental, learns the ratio the game applies and scales the reported jitter by it
    CustomOptional<float> jitterScaleOverride { 0.0f }; // [FFXIV] JitterScaleOverride: experiment, a fixed scale (0.25 to 1.0) instead of the learned one; 0 = off
    CustomOptional<bool> jitterSource { false };     // [FFXIV] JitterSource: experimental, fixes the jitter at the source (full lattice, repeating cycle, told what was drawn); ext/ffxiv/jitter
    // Game build handling (ext/ffxiv/resolve). Persistent keys, read once when the game's first device is hooked; no menu control.
    CustomOptional<bool> legacyGameBuild { false };   // [FFXIV] FfxivLegacyGameBuild: rollback, exactly the behaviour before pattern search (exact build match, compile-time table, no resolve)
    CustomOptional<bool> unknownBuildHooks { false }; // [FFXIV] FfxivUnknownBuildHooks: allow the features that read game addresses on a game build the table does not know
    CustomOptional<bool> forceUnknownBuild { false }; // [FFXIV] FfxivForceUnknownBuild: debug, ignore the table so the resolver's values alone can be tried on today's exe
    CustomOptional<bool> automaticReShadeDepth { false }; // [FFXIV] AutomaticReShadeDepth: ReShade effects get a copy of the scene depth at the output size (ext/reshade/ReShadeDepth.h)
    // Developer draw diagnostic (ext/ffxiv/diag), ini only, off by default: `off` | `capture`. Recorded when the game creates its shaders,
    // so it is read once at start; anything but `capture` reads as off, which is today's behaviour.
    CustomOptional<std::string> drawDiagnostics { "off" };     // [FFXIV] DrawDiagnostics
    CustomOptional<int32_t> drawDiagnosticsFrames { 3 };       // [FFXIV] DrawDiagnosticsFrames: how many frames one capture writes (1..16)
    // When the capture starts (ext/ffxiv/diag/DrawDiagFormat.h has the rules): seconds after the config load, or a key press. Both
    // default to today's behaviour, the capture collecting from the first draw.
    CustomOptional<int32_t> drawDiagnosticsDelaySeconds { 0 }; // [FFXIV] DrawDiagnosticsDelaySeconds: 0..3600, 0 = at the first draw
    CustomOptional<std::string> drawDiagnosticsKey { "off" };  // [FFXIV] DrawDiagnosticsKey: a letter, a digit or F1..F24; off = no key
    // Contact shadows before the sun light pass (ext/ffxiv/relight): the switch and its tuning, off by default, and the two keys that
    // settle what the measurement of the sun vector could not (ext/ffxiv/relight/ContactShadowsRules.h has the ranges).
    CustomOptional<bool> contactShadows { false };            // [FFXIV] ContactShadows
    CustomOptional<float> contactShadowsLength { 0.5f };      // [FFXIV] ContactShadowsLength: metres of march, 0.05..2.0
    CustomOptional<float> contactShadowsStrength { 0.5f };    // [FFXIV] ContactShadowsStrength: 0..1 of the sun's visibility
    CustomOptional<int32_t> contactShadowsSteps { 12 };       // [FFXIV] ContactShadowsSteps: 4..24
    CustomOptional<std::string> contactShadowsSunSpace { "auto" }; // [FFXIV] ContactShadowsSunSpace: auto | world | view
    CustomOptional<std::string> contactShadowsSunSign { "auto" };  // [FFXIV] ContactShadowsSunSign: auto | toward | from
    // [FFXIV] ContactShadowsDump: seconds after the game start at which one frame is written out, 0 = never (the developer check)
    CustomOptional<int32_t> contactShadowsDump { 0 };
    // Developer probe, ini only: a constant half-screen mask handed to the upscaler's mask input, to see whether the backend reads
    // one at all. off | left | right, anything else reads as off (ext/ffxiv/vfxmask).
    CustomOptional<std::string> vfxMaskProbe { "off" }; // [FFXIV] VfxMaskProbe
    ffxiv::chara::CharaConfig chara; // ext/ffxiv/chara: [FFXIV] Preview* and [HDR] PreviewNRTrace

    void Bind(config::Binder& b);

    // End of config::Load (every load and reload). Hands the AutomaticReShadeDepth key to ext/reshade/ReShadeDepth.cpp, which registers
    // the hook at start-up. A pointer rather than a call so that ExtConfig.cpp keeps no link dependency on the glue: the unit tests that
    // build ExtConfig.cpp alone find it null and nothing happens. The hook only STORES the value (this can run inside DllMain).
    using BoolHook = void (*)(bool on);
    static inline BoolHook automaticReShadeDepthOnLoaded = nullptr;
    // The developer draw diagnostic's key (ext/ffxiv/diag): arms one capture. Same pointer pattern and same reason.
    static inline BoolHook drawDiagnosticsOnLoaded = nullptr;

    // Same for the VFX mask probe (ext/ffxiv/vfxmask/VfxMaskProbe.cpp): the text of [FFXIV] VfxMaskProbe, parsed there.
    using TextHook = void (*)(const char* text);
    static inline TextHook vfxMaskProbeOnLoaded = nullptr;
    // And for the contact shadow switch (ext/ffxiv/relight/Inject.cpp): the key arms the seam's feature bit.
    static inline BoolHook contactShadowsOnLoaded = nullptr;

    void PostLoad()
    {
        if (automaticReShadeDepthOnLoaded != nullptr)
            automaticReShadeDepthOnLoaded(automaticReShadeDepth.value_or_default());
        if (drawDiagnosticsOnLoaded != nullptr)
            drawDiagnosticsOnLoaded(drawDiagnostics.value_or_default() == "capture");
        if (vfxMaskProbeOnLoaded != nullptr)
            vfxMaskProbeOnLoaded(vfxMaskProbe.value_or_default().c_str());
        if (contactShadowsOnLoaded != nullptr)
            contactShadowsOnLoaded(contactShadows.value_or_default());
    }
};
} // namespace ext
