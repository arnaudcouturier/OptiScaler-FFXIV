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
    CustomOptional<bool> companionHudReplacement { false };
    CustomOptional<bool> companionHudInterpolation { false };
    CustomOptional<bool> jitterProbe { false };      // [FFXIV] JitterProbe: read-only log of the game's applied versus reported jitter (ext/ffxiv/jitter)
    CustomOptional<bool> jitterCorrection { false }; // [FFXIV] JitterCorrection: experimental, learns the ratio the game applies and scales the reported jitter by it
    CustomOptional<float> jitterScaleOverride { 0.0f }; // [FFXIV] JitterScaleOverride: experiment, a fixed scale (0.25 to 1.0) instead of the learned one; 0 = off
    CustomOptional<bool> jitterSource { false };     // [FFXIV] JitterSource: experimental, fixes the jitter at the source (full lattice, repeating cycle, told what was drawn); ext/ffxiv/jitter
    CustomOptional<bool> automaticReShadeDepth { false }; // [FFXIV] AutomaticReShadeDepth: ReShade effects get a copy of the scene depth at the output size (ext/reshade/ReShadeDepth.h)
    // [FFXIV] AutomaticReShadeMotionScale: ini only (no menu control). The motion vectors that REST hands to ReShade are realigned with the
    // rest of the G-buffers; this multiplies their VALUES by output/render per axis as well. DERIVED (not yet confirmed in game): the switch
    // exists to turn it off if it turns out wrong, and goes once it is confirmed. Only matters while AutomaticReShadeDepth is on.
    CustomOptional<bool> automaticReShadeMotionScale { true };
    ffxiv::chara::CharaConfig chara; // ext/ffxiv/chara: [FFXIV] Preview* and [HDR] PreviewNRTrace

    void Bind(config::Binder& b);

    // End of config::Load (every load and reload). Hands the AutomaticReShadeDepth key to ext/reshade/ReShadeDepth.cpp, which registers
    // the hook at start-up. A pointer rather than a call so that ExtConfig.cpp keeps no link dependency on the glue: the unit tests that
    // build ExtConfig.cpp alone find it null and nothing happens. The hook only STORES the value (this can run inside DllMain).
    using BoolHook = void (*)(bool on);
    static inline BoolHook automaticReShadeDepthOnLoaded = nullptr;

    void PostLoad()
    {
        if (automaticReShadeDepthOnLoaded != nullptr)
            automaticReShadeDepthOnLoaded(automaticReShadeDepth.value_or_default());
    }
};
} // namespace ext
