#pragma once
// [HudLess] config keys (docs/HUDLESS.md section 11, docs/SEAMS.md section 3.4). Owner: S10 package B. Use:
// ext::Cfg().hudless.GetMode(). `Diagnostics` of the same section is bound by package A (ext/ffxiv/ui/UiConfig).
//
// The old [FrameGen] ExternalUI* / ExternalHudless keys are neither read nor written.

#include <Config.h>

#include "ext/hudless/HudlessMode.h"
#include "ext/hudless/PlateMode.h"

#include <cmath>
#include <cstdint>
#include <string>

namespace ext::config
{
class Binder;
}

namespace ext
{
struct HudlessConfig
{
    static constexpr float kThresholdDefault = 0.008f;
    static constexpr float kThresholdMin = 0.001f;
    static constexpr float kThresholdMax = 1.0f;
    static constexpr int kDilationMax = 4;
    static constexpr float kGuardDefault = 0.60f;
    static constexpr int kPlateStepMax = 256;

    // off | recomposition | ui_free. A string key: anything else reads as off.
    CustomOptional<std::string> mode { "off" }; // Mode
    // Advanced
    CustomOptional<float> maskThreshold { kThresholdDefault }; // MaskThreshold, 0.001 .. 1.0
    CustomOptional<int> maskDilation { 1 };                    // MaskDilation, 0 .. 4
    CustomOptional<bool> pasteCleanup { true };                // PasteCleanup
    CustomOptional<bool> steadyTiming { false };               // SteadyTiming
    CustomOptional<float> coverageGuard { kGuardDefault };     // CoverageGuard, 0 = off
    CustomOptional<bool> showDetectedUi { false };             // ShowDetectedUi (debug tint on the pasted area)
    // legacy | density. A string key, ini only (see hudless::GuardPolicy); anything else reads as legacy.
    CustomOptional<std::string> guardMode { "legacy" };        // GuardMode
    // Density policy only, ini only (see hudless::CoverageGuard).
    CustomOptional<int> guardReleaseHolds { 0 };               // GuardReleaseHolds, 0 = off, clamped to kMaxReleaseHolds
    CustomOptional<bool> guardTripOnAnchorLater { false };     // GuardTripOnAnchorLater
    // While the guard is tripped, probe one present in N (1 = every present), live, 1 .. kProbeIntervalMax.
    CustomOptional<int> probeInterval { 1 };                   // ProbeInterval
    // Developer, ini only: blind A/B of the probe interval in blocks of this many seconds (0 = off), 0 .. kProbeAbSecondsMax.
    CustomOptional<int> probeAbSeconds { 0 };                  // ProbeAbSeconds
    // off | on. A string key, ini only. on = under HDR output the paste's HDR stage is made before the first paste image (the PQ
    // conversion path of the paste has not run in a game yet); anything else reads as off.
    CustomOptional<std::string> hdrPaste { "off" };            // HdrPaste
    // Developer probe, log only, ini only (docs/HUDLESS.md "Developer probes").
    CustomOptional<bool> plateDiagnostics { false };           // PlateDiagnostics
    // Native nameplates, live: off | endpoint | on. A string key; anything else reads as off (today's behaviour).
    CustomOptional<std::string> plateInterp { "off" };         // PlateInterp
    // auto | 0 | 1, the constant of the per-output shift share. A string key; anything else reads as auto.
    CustomOptional<std::string> platePhaseOffset { "auto" };   // PlatePhaseOffset
    // Largest plate shift, in output pixels per game frame (0..256).
    CustomOptional<int> plateMaxStep { 24 };                   // PlateMaxStep
    // r1b | early, live: where the UI-free capture is taken when the first UI draw goes off-screen. A string key; anything else
    // reads as r1b (today's behaviour).
    CustomOptional<std::string> anchorRule { "r1b" };          // AnchorRule

    // Values as the engine uses them: out-of-range or non-finite input is clamped / replaced, the stored text is not
    // rewritten.
    hudless::Mode GetMode() const { return hudless::ParseMode(mode.value_or_default()); }
    bool HdrPasteOn() const { return hudless::ParseHdrPaste(hdrPaste.value_or_default()); }
    hudless::GuardPolicy GetGuardPolicy() const { return hudless::ParseGuardPolicy(guardMode.value_or_default()); }
    hudless::AnchorRule GetAnchorRule() const { return hudless::ParseAnchorRule(anchorRule.value_or_default()); }
    // 0 = off. Out-of-range input is clamped, the stored text is not rewritten.
    int ReleaseHolds() const
    {
        const int v = guardReleaseHolds.value_or_default();
        return v < 0 ? 0 : (v > hudless::kGuardReleaseHoldsMax ? hudless::kGuardReleaseHoldsMax : v);
    }
    bool TripOnAnchorLater() const { return guardTripOnAnchorLater.value_or_default(); }
    // Out-of-range input is clamped, the stored text is not rewritten.
    int ProbeInterval() const { return hudless::ClampProbeInterval(probeInterval.value_or_default()); }
    int ProbeAbSeconds() const
    {
        const int v = probeAbSeconds.value_or_default();
        return v < 0 ? 0 : (v > hudless::kProbeAbSecondsMax ? hudless::kProbeAbSecondsMax : v);
    }
    float Threshold() const
    {
        const float v = maskThreshold.value_or_default();
        if (!std::isfinite(v))
            return kThresholdDefault;
        return v < kThresholdMin ? kThresholdMin : (v > kThresholdMax ? kThresholdMax : v);
    }
    uint32_t Dilation() const
    {
        const int v = maskDilation.value_or_default();
        return static_cast<uint32_t>(v < 0 ? 0 : (v > kDilationMax ? kDilationMax : v));
    }
    // Share of flagged pixels above which the engine falls back; 0 = guard off.
    float CoverageLimit() const
    {
        const float v = coverageGuard.value_or_default();
        if (!std::isfinite(v))
            return kGuardDefault;
        return v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
    }

    // Native nameplates: the switch, the resolved phase constant and the step cap, clamped and fail-open.
    hudless::PlateInterp GetPlateInterp() const { return hudless::ParsePlateInterp(plateInterp.value_or_default()); }
    int GetPlatePhase() const
    {
        return hudless::ResolvePlatePhase(hudless::ParsePlatePhase(platePhaseOffset.value_or_default()));
    }
    int GetPlateStep() const
    {
        const int v = plateMaxStep.value_or_default();
        return v < 0 ? 0 : (v > kPlateStepMax ? kPlateStepMax : v);
    }
    hudless::PlateSnapshot PlateSettings() const
    {
        return hudless::PlateSnapshot::Clamped(GetPlateInterp(), GetPlatePhase(), GetPlateStep());
    }

    void Bind(config::Binder& b);
    void PostLoad(); // end of config::Load: publishes the mode (cached copy, shared Wanted flag, feature bit)
};
} // namespace ext

namespace ext::hudless
{
// The mode the engine acts on. Cached atomic copy of the config: cheap on the Present path, safe from any thread.
Mode CurrentMode();

// Menu: stores the mode in the config (saved with the other keys) and republishes the cached state and the Wanted flag.
void SetMode(Mode mode);
} // namespace ext::hudless
