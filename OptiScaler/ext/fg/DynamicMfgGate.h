#pragma once
// Dynamic MFG on RTX 40: the pure decisions around the selector (DynamicMfg.h) that need no upstream types: the gating
// table, the target and ceiling arithmetic and the ForceDMFG / Ada-ceiling guard of the DLSS-G options. The glue that
// reads the live state is DynamicMfgSeams.cpp, the DlssgSeams.cpp seam applies the guard.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ext::fg
{
// NV_GPU_ARCHITECTURE_GB100: below it a card is Ada or older (nvngx_dlssg's own gate, see MfgUnlock.h).
inline constexpr uint32_t kBlackwellArchitecture = 0x1B0;
inline constexpr bool IsBlackwellArchitecture(uint32_t archId)
{
    return archId >= kBlackwellArchitecture;
}
// The menu card (MenuAutoMfg.cpp) is shown for the DLSS-G output on every card but RTX 50, where NVIDIA's own dynamic mode
// is the tool. Pure, so the visibility rule is tested without ImGui; the card passes the two facts in.
inline constexpr bool AutoMfgCardVisible(bool dlssgOutputActive, bool blackwell)
{
    return dlssgOutputActive && !blackwell;
}
// The same rule for a caller whose Blackwell fact is not free (a GPU enumeration): `blackwell` is called only when the DLSS-G
// output is the active one, so the menu does not ask the GPU on every draw while another output (or none) is in use.
template <class AskBlackwell>
bool AutoMfgCardVisibleAsking(bool dlssgOutputActive, AskBlackwell&& blackwell)
{
    return dlssgOutputActive && !blackwell();
}

// Asking IdentifyGpu for the primary GPU. It answers with vendor Invalid while its adapter list is empty, and every call then
// repeats the whole enumeration (and writes its own error line when the DXGI factory fails). So the ask is spaced by
// kRetryMs and capped at kMaxTries; after the last try the GPU stays unknown for the session and GaveUp() says so once, for
// the caller to log. One object per asking thread (not synchronised).
class GpuAsk
{
  public:
    static constexpr uint64_t kRetryMs = 5000;
    static constexpr int kMaxTries = 12;

    // True when the caller should ask now (the first time at once).
    bool ShouldAsk(uint64_t nowMs)
    {
        if (answered_ || tries_ >= kMaxTries)
            return false;
        if (tries_ != 0 && nowMs - lastMs_ < kRetryMs)
            return false;
        lastMs_ = nowMs;
        ++tries_;
        return true;
    }
    void Answered() { answered_ = true; }
    // After an unusable answer: true exactly once, when that was the last try.
    bool GaveUp()
    {
        if (answered_ || tries_ < kMaxTries || told_)
            return false;
        told_ = true;
        return true;
    }

  private:
    bool answered_ = false;
    bool told_ = false;
    int tries_ = 0;
    uint64_t lastMs_ = 0;
};
// An automatic choice (ours or NVIDIA's dynamic mode) never goes above 4x: three generated frames.
inline constexpr int kAutoMaxGenerated = 3;

enum class AutoMfgGate : uint8_t
{
    Open,
    KeyOff,      // [DLSSG] AutoMFG is false: silent, nothing is logged for it
    Stopped,     // the kill-switch ended automatic mode for this session
    NotDx12,     // the Vulkan DLSS-G output has no run-time state to watch
    NotDlssg,    // another frame generation output is active
    NotNvidia,
    Blackwell,   // RTX 50: NVIDIA's own dynamic mode is the right tool
    FollowsGame, // Frame Gen is on Default: the game owns the count
    DynamicMode, // Force Dynamic MFG is on: NVIDIA's mode picks the multiplier
    MaxBelow3x,  // the DLSS-G maximum is below 2 generated frames (3x): nothing to choose between
};

inline const char* GateText(AutoMfgGate gate)
{
    switch (gate)
    {
    case AutoMfgGate::Open:
        return "active";
    case AutoMfgGate::KeyOff:
        return "AutoMFG is off";
    case AutoMfgGate::Stopped:
        return "stopped for this session after a count change misbehaved";
    case AutoMfgGate::NotDx12:
        return "the DLSS-G output is not the D3D12 one";
    case AutoMfgGate::NotDlssg:
        return "the active frame generation output is not DLSS-G";
    case AutoMfgGate::NotNvidia:
        return "the GPU is not an NVIDIA one";
    case AutoMfgGate::Blackwell:
        return "RTX 50 series: use NVIDIA's own dynamic multi frame generation";
    case AutoMfgGate::FollowsGame:
        return "Frame Gen is on Default, so the game controls the count";
    case AutoMfgGate::DynamicMode:
        return "Force Dynamic MFG is on, NVIDIA's dynamic mode picks the multiplier";
    case AutoMfgGate::MaxBelow3x:
        return "the DLSS-G maximum is below 3x (on RTX 40 enable the Ada MFG unlock and restart)";
    }
    return "unknown";
}

struct GateInputs
{
    bool keyOn = false;        // [DLSSG] AutoMFG
    bool stopped = false;      // kill-switch
    bool dx12 = false;         // the DLSS-G output is the D3D12 one
    bool dlssgOutput = false;  // State::activeFgOutput == DLSSG
    bool nvidia = false;
    uint32_t archId = 0;       // NVIDIA architecture id
    bool followsGame = false;  // Config::FGFollowsGame()
    bool dynamicMode = false;  // the options' mode is eDynamic
    int maxGenerated = 0;      // IFGFeature::GetMaxInterpolationCount()
};

// First gate that fails, in the order of the table in the study (3.2); Open when all hold.
inline AutoMfgGate EvaluateGate(const GateInputs& in)
{
    if (!in.keyOn)
        return AutoMfgGate::KeyOff;
    if (in.stopped)
        return AutoMfgGate::Stopped;
    if (!in.dx12)
        return AutoMfgGate::NotDx12;
    if (!in.dlssgOutput)
        return AutoMfgGate::NotDlssg;
    if (!in.nvidia)
        return AutoMfgGate::NotNvidia;
    if (IsBlackwellArchitecture(in.archId))
        return AutoMfgGate::Blackwell;
    if (in.followsGame)
        return AutoMfgGate::FollowsGame;
    if (in.dynamicMode)
        return AutoMfgGate::DynamicMode;
    if (in.maxGenerated < 2)
        return AutoMfgGate::MaxBelow3x;
    return AutoMfgGate::Open;
}

// The display refresh as Windows reports it: dmDisplayFrequency is 0 or 1 when the mode does not say ("unspecified"), and
// no real display runs below 24 Hz. Below that the refresh is unknown (0), so it never becomes a 30 fps target.
inline double UsableRefresh(double hz)
{
    return std::isfinite(hz) && hz >= 24.0 ? hz : 0.0;
}

// Gates that cannot reopen during a session without a change of the keys: the GPU and the DLS-G output type are fixed, the
// kill-switch only ends with the process, and a key that is off stays off until the user ticks it (the glue notices that: a
// change of either key is a full tick). The glue stops ticking once one of them was seen (DynamicMfgGlue.h).
inline constexpr bool IsPermanentlyClosed(AutoMfgGate gate)
{
    return gate == AutoMfgGate::NotNvidia || gate == AutoMfgGate::Blackwell || gate == AutoMfgGate::NotDx12 ||
           gate == AutoMfgGate::Stopped || gate == AutoMfgGate::KeyOff;
}

// Target frame rate: [DLSSG] AutoMFGTarget when set, else the lower of the frame rate limit and the display refresh
// (whichever of the two is known), clamped to 30..500. 0 = unknown, the selector then stays inert.
inline double ResolveTarget(double configured, double limit, double refresh)
{
    double target = 0.0;
    if (std::isfinite(configured) && configured > 0.0)
        target = configured;
    else
    {
        const bool haveLimit = std::isfinite(limit) && limit > 0.0;
        const bool haveRefresh = std::isfinite(refresh) && refresh > 0.0;
        if (haveLimit && haveRefresh)
            target = std::min(limit, refresh);
        else if (haveLimit)
            target = limit;
        else if (haveRefresh)
            target = refresh;
    }
    return target > 0.0 ? std::clamp(target, 30.0, 500.0) : 0.0;
}

// Highest factor (generated frames + 1) an automatic choice may use: AutoMFGMax (2..4), the DLSS-G maximum, never 5x/6x.
inline int HardMaxFactor(int configuredMax, int maxGenerated)
{
    return std::clamp(std::min({ 4, configuredMax, maxGenerated + 1 }), 2, 4);
}

// DLSSGOptions guard, applied to every frame's options (DlssgSeams.cpp):
//  - eDynamic is set without a support check by Force Dynamic MFG; where Streamline reports dynamic MFG as unsupported it
//    falls back to eOn (the manual count stays);
//  - on a card whose ceiling the Ada unlock opened, an automatic choice (NVIDIA's dynamic mode or the selector) can never
//    be 5x/6x. The manual slider keeps its range.
struct OptionsGuardIn
{
    bool dynamicMode = false;
    bool dmfgSupported = false;
    bool adaUnlockActive = false;
    bool autoMfgControls = false; // the selector chose this frame's count
    int generated = 1;            // options.numFramesToGenerate
};
struct OptionsGuardOut
{
    bool dynamicToOn = false;
    bool ceilingClamped = false;
    int generated = 1;
};

inline OptionsGuardOut GuardDlssgOptions(const OptionsGuardIn& in)
{
    OptionsGuardOut out;
    out.generated = in.generated;
    out.dynamicToOn = in.dynamicMode && !in.dmfgSupported;
    const bool automatic = (in.dynamicMode && in.dmfgSupported) || in.autoMfgControls;
    if (in.adaUnlockActive && automatic && in.generated > kAutoMaxGenerated)
    {
        out.generated = kAutoMaxGenerated;
        out.ceilingClamped = true;
    }
    return out;
}
} // namespace ext::fg
