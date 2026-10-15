#pragma once
// Frame-generation config keys (docs/SEAMS.md section 3.4). Owner: D1-D5. Sections [DLSSG], [XeFG], [LowLatency],
// key names identical to the reference build. Use: ext::Cfg().fg.adaMfgUnlock.value_or_default()
//
// Dropped with their code: [DLSSG] AdaBlackwellKernels (never read), [FrameGen] External (E6), [FrameGen]
// ExternalHudless (C11 add-on gate). The [FrameGen] ExternalUI* keys belong to the hudless engine (C11, S10).

#include <Config.h>

#include <cstdint>
#include <string>

namespace ext::config
{
class Binder;
}

namespace ext
{
// D3: the provider's ceiling for generated frames per real frame (XeFG 2.x). Code is authoritative (SEAMS 5); the ini
// text says 1..7 as well. This was Config::XeFGMaxInterpolations in the reference build.
inline constexpr int32_t kXeFgMaxInterpolations = 7;

struct FgConfig
{
    // D1: built-in RTX 40 (Ada) MFG unlock plus targeted temporal correction. Memory-only, restart after changing.
    CustomOptional<bool> adaMfgUnlock { false };

    // D4: RTX 20/30 (Ampere/Turing) companion DLSS-G runtime, loaded before Streamline. Restart after changing.
    CustomOptional<bool> ampereMfgUnlock { false };
    CustomOptional<int> ampereMfgMaxFrames { 3 };
    CustomOptional<std::string> ampereMfgKernelImage { "Auto" };
    CustomOptional<std::string> ampereMfgRouter { "Auto" };
    CustomOptional<bool> ampereMfgHardwareBilinear { false };
    CustomOptional<int> ampereMfgOptimized { 1 };
    CustomOptional<std::string> ampereMfgPreset { "Auto" };
    CustomOptional<std::string> ampereMfgSpoofArchToGame { "Auto" };
    CustomOptional<int> ampereMfgLogLevel { 1 };

    // D2/C11: DLSS-G UI recomposition (DLSSGOptions::enableUserInterfaceRecomposition). Unset = on while a hud-less source
    // is in use (see ext/fg/HudlessLink.h).
    CustomOptional<bool> uiRecomposition { false };

    // D3: XeFG extras ([XeFG] section, shared with upstream's keys, which stay in Config.h). PostLoad() range-checks
    // InterpolationCount's upper bound, which upstream does not know.
    CustomOptional<bool> xefgDynamic { false };                               // DynamicMFG
    CustomOptional<float> xefgDynamicTarget { 138.0f };                       // DynamicTargetFPS
    CustomOptional<bool> xefgUnlock { true };                                 // UnlockMFG (XeFGUnlock provider patches)
    CustomOptional<int> xefgMaxInterpolatedFrames { kXeFgMaxInterpolations }; // MaxInterpolatedFrames
    CustomOptional<bool> xefgExtraPacing { true };                            // ExtraPacing (needs a game restart)
    CustomOptional<bool> xefgResetOnUpscalerChange { true };                  // ResetOnUpscalerChange

    void Bind(config::Binder& b);
    void PostLoad(); // end of ext::config::Load: range checks that ran right after the keys were read
};
} // namespace ext
