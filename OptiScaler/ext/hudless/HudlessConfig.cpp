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
    b.Add("HudLess", "GuardReleaseHolds", guardReleaseHolds);
    b.Add("HudLess", "GuardTripOnAnchorLater", guardTripOnAnchorLater);
    b.Add("HudLess", "ProbeInterval", probeInterval);
    b.Add("HudLess", "ProbeAbSeconds", probeAbSeconds);
    b.Add("HudLess", "AnchorRule", anchorRule);
    b.Add("HudLess", "HdrPaste", hdrPaste);
    b.Add("HudLess", "PlateDiagnostics", plateDiagnostics);
    b.Add("HudLess", "PlateInterp", plateInterp);
    b.Add("HudLess", "PlatePhaseOffset", platePhaseOffset);
    b.Add("HudLess", "PlateMaxStep", plateMaxStep);
}

void HudlessConfig::PostLoad()
{
    hudless::Publish(GetMode());
    // The probes have their own seam bit: with the key off nothing else about the seams changes.
    ext::SetActive(ext::F::PlateProbe, plateDiagnostics.value_or_default());
    // The native nameplate engine: the settings go first, the feature bit second. A presenting thread that sees the bit
    // set therefore already finds the key on, and the engine's drain (which clears the bit when the last hidden image
    // is gone and re-reads this snapshot) can never leave the key on with the bit off. The bit is never cleared here.
    hudless::PublishPlateSettings(PlateSettings());
    if (GetPlateInterp() != hudless::PlateInterp::Off)
        ext::SetActive(ext::F::Plate, true);
}
} // namespace ext
