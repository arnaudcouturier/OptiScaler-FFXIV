#pragma once
// The one rule that decides whether OptiScaler asks DLSS-G for UI recomposition. Pure (no Windows, NVIDIA or logger headers), so
// tests/ext/fg/fg_ui_recomposition_rule.cpp builds it standalone.
//
// Why a rule on the PRESET and not on what the runtime reported: nvngx_dlssg reads the flag when the feature is created, and a
// change after that makes Streamline recreate the feature ("UserInterfaceRecomposition flag changed from create-time value 1 to 0").
// The runtime's confirmation of a preset only exists after the creation, so a decision taken from it asks for recomposition on the
// first SetOptions and withdraws it a moment later. Measured 2026-10-22: that recreation left about 59 of 132 presents/s as distinct
// pictures. The decision is taken from the configured / active request instead, on every SetOptions, the first one included, exactly
// like the reference build (`preset != 1 && preset != 3`).
//
// Verified in nvngx_dlssg.dll 310.9.16 (sha256 8d59d49b...), EndpointConfiguration::ApplyRenderPreset: A forces UI recomposition off
// (and FG Lite off), B forces it on, C forces it off (and FG Lite on), Auto leaves it to the application. So under A and C the
// application's request is overridden by the runtime itself and asking for it only costs a feature recreation.
//
// 310.9.1.0 ignores C (the runtime reports applied=0), so there C does NOT turn recomposition off by itself. The rule is the same
// there: C and recomposition lose recomposition on every version. Reason: one rule that does not depend on the runtime build (and so
// not on a reply that arrives after creation) is what keeps the creation stable; a 310.9.1.0 user who wants the recomposition picks
// Auto or B, which the menu says.
//
// The runtime's confirmation (PresetLog.h Readout::Confirm) feeds the menu readout only (PresetMenuText.h UiRecompositionReadoutNote). It
// never decides the flag; tests/ext/fg/fg_preset_seams_text.cpp pins that.

#include "ext/fg/dlssg/PresetRequest.h"
#include "ext/fg/dlssg/PresetSwitch.h"

#include <atomic>
#include <optional>

namespace ext::fg::preset
{
// Presets under which the runtime forces its UI recomposition off: A and C. Anything else, an invalid number included, is read as
// Auto (RequestState already turns an invalid configured value into Auto).
constexpr bool PresetDropsUiRecomposition(int request) { return request == kPresetA || request == kPresetC; }

// The request the runtime is really told: the driver-settings seam only answers while the control is available (PresetDrs.cpp), so a
// companion runtime or a replacement runtime, or an output other than DLSS-G, is never told the preset and runs as Auto.
constexpr int EffectiveRequest(Unavailable availability, int activeRequest)
{
    return availability == Unavailable::None && ValidRequest(activeRequest) ? activeRequest : kAuto;
}

// The availability of the control as the runtime lived it: remembered ONCE, by whichever consumer comes first (the driver-settings seam, when
// the runtime first asks the driver for its preset at boot, or the first SetOptions), and never changed afterwards. The runtime was told the
// preset (or not) on that first look, and the rule has to describe that runtime: the companion-runtime checkbox writes its config at once
// although it says "restart", so a second look could disagree with what the runtime was told and the flag would no longer match the preset
// it applied. Before anything is remembered a reader that does not commit (the menu) sees the live value.
class AvailabilityLatch
{
public:
    // `live`: the availability right now. `commit`: this caller is one of the two that fix it. Returns the remembered value, or `live` while
    // nothing is remembered and this caller does not commit.
    Unavailable Resolve(Unavailable live, bool commit)
    {
        int value = remembered_.load(std::memory_order_acquire);
        if (value < 0)
        {
            value = static_cast<int>(live);
            if (commit)
            {
                int expected = -1;
                remembered_.compare_exchange_strong(expected, value, std::memory_order_acq_rel);
                value = remembered_.load(std::memory_order_acquire);
            }
        }
        return static_cast<Unavailable>(value);
    }

    // The remembered value, or nothing while no consumer has looked yet. Never changes the latch.
    std::optional<Unavailable> Peek() const
    {
        const int value = remembered_.load(std::memory_order_acquire);
        return value < 0 ? std::nullopt : std::optional<Unavailable>(static_cast<Unavailable>(value));
    }

private:
    std::atomic<int> remembered_ { -1 };
};

// The availability the CONTROL (menu, live preset switch, driver answer) works from: sticky in the unavailable direction. When boot fixed
// a reason why the runtime was not told the preset (companion runtime, replacement runtime, another output), a live value that later
// goes back to None (the box was unticked, a fallback happened) must not make a switch to A or C possible: the rule keeps reading the
// remembered value, so the preset the switch commits would no longer be the one the flag follows. Latched None (the runtime was told)
// or nothing latched yet: the live value, as before. Reading never commits.
inline Unavailable ControlAvailability(const AvailabilityLatch& latch, Unavailable live)
{
    const auto kept = latch.Peek();
    return kept.has_value() && *kept != Unavailable::None ? *kept : live;
}

struct UiRecompositionDecision
{
    bool requested = false;        // set enableUserInterfaceRecomposition and tag the hud-less image
    bool droppedByPreset = false;  // recomposition was wanted and the preset takes it away
    int preset = kAuto;            // the request the decision was taken from (the effective one)
};

// `modeWantsIt`: [HudLess] Mode is recomposition, a game source exists and [DLSSG] UIRecomposition is not off
// (ext::hudless::QueryRecomposition().enabled). `activeRequest`: Requests().Active(), the boot request until a live switch commits.
constexpr UiRecompositionDecision DecideUiRecomposition(bool modeWantsIt, Unavailable availability, int activeRequest)
{
    const int preset = EffectiveRequest(availability, activeRequest);
    const bool dropped = modeWantsIt && PresetDropsUiRecomposition(preset);
    return { modeWantsIt && !dropped, dropped, preset };
}
} // namespace ext::fg::preset
