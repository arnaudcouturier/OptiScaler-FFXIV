#pragma once
// Native nameplates at the output rate: the [HudLess] PlateInterp switch, the per-output shift constant and the settings
// snapshot the engine reads. Header-only so the engine logic and its tests can use it without the config machinery.
//
// PlateInterp: off = today's behaviour (nameplates stay native); endpoint = the engine hides the nameplate draws and
// draws them back unshifted over every output (looks like today, proves the whole draw path); on = endpoint until the
// output phase is proven and the plate tracker is confident, then each plate is shifted per output.
// PlatePhaseOffset: auto | 0 | 1, the constant c of the shift share s = (k + c) / M. Auto resolves to 1, the order the
// frame generator batches are assumed to present in (generated frames first, the real frame last); set 0 if a steady
// pan shows the plates glued one step ahead instead of behind.

#include "ext/hudless/HudlessMode.h"

#include <atomic>
#include <cstdint>
#include <string_view>

namespace ext::hudless
{
enum class PlateInterp : uint8_t
{
    Off,
    Endpoint,
    On,
};

// Case-insensitive; anything unknown (including "auto" and the empty string) is Off: fail open.
constexpr PlateInterp ParsePlateInterp(std::string_view text)
{
    text = detail::Trim(text);
    if (detail::EqualsNoCase(text, "on"))
        return PlateInterp::On;
    if (detail::EqualsNoCase(text, "endpoint"))
        return PlateInterp::Endpoint;
    return PlateInterp::Off;
}

constexpr const char* PlateInterpName(PlateInterp value)
{
    switch (value)
    {
    case PlateInterp::On:
        return "on";
    case PlateInterp::Endpoint:
        return "endpoint";
    default:
        return "off";
    }
}

// Not a value the user can pick: "I have not measured this" (the driver may pick the count per frame).
inline constexpr int kPlatePhaseAuto = -1;

// Case-insensitive; anything unknown (including "auto" and the empty string) is auto: fail open.
constexpr int ParsePlatePhase(std::string_view text)
{
    text = detail::Trim(text);
    if (detail::EqualsNoCase(text, "0"))
        return 0;
    if (detail::EqualsNoCase(text, "1"))
        return 1;
    return kPlatePhaseAuto;
}

constexpr int ResolvePlatePhase(int parsed) { return parsed == kPlatePhaseAuto ? 1 : parsed; }

// The plate settings the render thread and the presenting threads act on: a snapshot of the config, published at load
// and by the menu (the menu writes the config while those threads read). Out-of-range values are clamped, and unknown
// ones fail open (off, 1, 24).
struct PlateSnapshot
{
    PlateInterp interp = PlateInterp::Off;
    int phase = 1; // resolved (auto = 1)
    int step = 24; // largest shift, in output pixels per game frame; clamped 0..256

    // Clamped form of whatever the writer passed: the engine reads this, never the raw numbers.
    static PlateSnapshot Clamped(PlateInterp interp, int phase, int step)
    {
        PlateSnapshot out;
        out.interp = interp == PlateInterp::On || interp == PlateInterp::Endpoint ? interp : PlateInterp::Off;
        out.phase = phase == 0 ? 0 : 1;
        out.step = step < 0 ? 0 : (step > 256 ? 256 : step);
        return out;
    }
};

// The snapshot the engine reads once per frame (relaxed atomics, one frame of staleness is allowed).
//
// The state lives here, inline, so the config that writes it and the seams that read the switch need no engine
// translation unit (tests/ext/hudless/hudless_config.cpp links without it). Plain statics of trivial types: no destructor
// may run under the loader lock.
inline std::atomic<uint8_t> g_plateInterp { static_cast<uint8_t>(PlateInterp::Off) };
inline std::atomic<int> g_platePhase { 1 };
inline std::atomic<int> g_plateStep { 24 };

inline PlateSnapshot CurrentPlates()
{
    PlateSnapshot out;
    out.interp = static_cast<PlateInterp>(g_plateInterp.load(std::memory_order_relaxed));
    out.phase = g_platePhase.load(std::memory_order_relaxed);
    out.step = g_plateStep.load(std::memory_order_relaxed);
    return out;
}

// Publishes the snapshot. Called at load and by the menu after any plate key changed.
inline void PublishPlateSettings(const PlateSnapshot& snapshot)
{
    const PlateSnapshot clamped = PlateSnapshot::Clamped(snapshot.interp, snapshot.phase, snapshot.step);
    g_platePhase.store(clamped.phase, std::memory_order_relaxed);
    g_plateStep.store(clamped.step, std::memory_order_relaxed);
    // The switch last: a reader that sees the new interp sees the values it acts on.
    g_plateInterp.store(static_cast<uint8_t>(clamped.interp), std::memory_order_relaxed);
}
} // namespace ext::hudless
