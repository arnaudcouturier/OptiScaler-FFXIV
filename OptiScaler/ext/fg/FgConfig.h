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

    void Bind(config::Binder& b);
};
} // namespace ext
