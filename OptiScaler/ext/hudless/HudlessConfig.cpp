#include "pch.h"

#include "ext/ExtFeatures.h"
#include "ext/core/ExtConfig.h"
#include "ext/hudless/HudlessConfig.h"
#include "ext/hudless/HudlessState.h"

#include <atomic>

namespace ext::hudless
{
namespace
{
// Plain static of a trivial type: no destructor to run under the loader lock.
std::atomic<uint8_t> g_mode { static_cast<uint8_t>(Mode::Off) };

void Publish(Mode mode)
{
    g_mode.store(static_cast<uint8_t>(mode), std::memory_order_relaxed);
    // The A-side hooks (boundary detector) arm from this flag; it costs them one atomic load while the mode is off.
    SetWanted(mode != Mode::Off);
    // The seams' inline guard. It stays set after a switch to off until the engine has seen the mode change (cleanup),
    // and it starts set so the first seam entry can log the start-up mode line (the logger does not exist yet while
    // the first Config::Reload runs).
    ext::SetActive(ext::F::Hudless, true);
}
} // namespace

Mode CurrentMode() { return static_cast<Mode>(g_mode.load(std::memory_order_relaxed)); }

void SetMode(Mode mode)
{
    Cfg().hudless.mode = std::string(ModeName(mode));
    Publish(mode);
}
} // namespace ext::hudless

namespace ext
{
void HudlessConfig::Bind(config::Binder& b)
{
    b.Add("HudLess", "Mode", mode);
    b.Add("HudLess", "MaskThreshold", maskThreshold);
    b.Add("HudLess", "MaskDilation", maskDilation);
    b.Add("HudLess", "PasteCleanup", pasteCleanup);
    b.Add("HudLess", "SteadyTiming", steadyTiming);
    b.Add("HudLess", "CoverageGuard", coverageGuard);
    b.Add("HudLess", "ShowDetectedUi", showDetectedUi);
    b.Add("HudLess", "GuardMode", guardMode);
    b.Add("HudLess", "HdrPaste", hdrPaste);
    b.Add("HudLess", "PlateDiagnostics", plateDiagnostics);
}

void HudlessConfig::PostLoad()
{
    hudless::Publish(GetMode());
    // The probes have their own seam bit: with the key off nothing else about the seams changes.
    ext::SetActive(ext::F::PlateProbe, plateDiagnostics.value_or_default());
}
} // namespace ext
