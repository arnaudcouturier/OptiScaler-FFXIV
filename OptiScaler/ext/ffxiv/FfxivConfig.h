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

    void PostLoad()
    {
        if (automaticReShadeDepthOnLoaded != nullptr)
            automaticReShadeDepthOnLoaded(automaticReShadeDepth.value_or_default());
        if (drawDiagnosticsOnLoaded != nullptr)
            drawDiagnosticsOnLoaded(drawDiagnostics.value_or_default() == "capture");
        if (vfxMaskProbeOnLoaded != nullptr)
            vfxMaskProbeOnLoaded(vfxMaskProbe.value_or_default().c_str());
    }
};
} // namespace ext
