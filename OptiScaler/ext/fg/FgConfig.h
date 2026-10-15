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

    void Bind(config::Binder& b);
};
} // namespace ext
