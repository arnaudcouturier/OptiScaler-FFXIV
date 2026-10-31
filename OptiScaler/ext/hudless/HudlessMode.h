#pragma once
// S10: the [HudLess] Mode value (docs/HUDLESS.md section 11). Header-only so the pure engine logic and its tests can use it
// without the config machinery.

#include <cstdint>
#include <string_view>

namespace ext::hudless
{
enum class Mode : uint8_t
{
    Off,           // today's behaviour, byte for byte
    Recomposition, // DLSS-G recomposes the UI itself from the hud-less + UI images (it still interpolates the UI layer)
    UiFree,        // frame generation runs on the UI-free image and the UI is pasted back at the real frame rate
};

namespace detail
{
constexpr char Lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }
constexpr bool EqualsNoCase(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (Lower(a[i]) != Lower(b[i]))
            return false;
    return true;
}
constexpr std::string_view Trim(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
        s.remove_suffix(1);
    return s;
}
} // namespace detail

// Case-insensitive; anything unknown (including "auto" and the empty string) is Off: fail open.
constexpr Mode ParseMode(std::string_view text)
{
    text = detail::Trim(text);
    if (detail::EqualsNoCase(text, "ui_free") || detail::EqualsNoCase(text, "uifree"))
        return Mode::UiFree;
    if (detail::EqualsNoCase(text, "recomposition"))
        return Mode::Recomposition;
    return Mode::Off;
}

// [HudLess] GuardMode: how the mask coverage guard trips and releases (docs/HUDLESS.md section 5).
//   legacy  trip on 5 fresh samples in a row over the limit, release after 30 in a row at or below 0.75 x the limit
//   density trip on 3 bad of the last 12 fresh samples, release after 60 clean ones in a row, and a frame anchored after
//           an off-screen UI draw keeps a tripped guard tripped at once
enum class GuardPolicy : uint8_t
{
    Legacy,
    Density,
};

// Case-insensitive; anything unknown (including "auto" and the empty string) is legacy: fail open, today's behaviour.
constexpr GuardPolicy ParseGuardPolicy(std::string_view text)
{
    return detail::EqualsNoCase(detail::Trim(text), "density") ? GuardPolicy::Density : GuardPolicy::Legacy;
}

// Upper bound of [HudLess] GuardReleaseHolds, applied by the config and by the guard (one summary window of presents).
constexpr int kGuardReleaseHoldsMax = 600;

// [HudLess] ProbeInterval: while the coverage guard is tripped, one present in N builds the capture and the mask image that keep the
// guard fed (1 = every present). The release needs 30 (density: 60) fresh samples, so it takes about that many times N presents.
constexpr int kProbeIntervalMax = 16;
constexpr int ClampProbeInterval(int interval) { return interval < 1 ? 1 : (interval > kProbeIntervalMax ? kProbeIntervalMax : interval); }
// The interval the engine runs with. The debug tint ([HudLess] ShowDetectedUi) draws the newest completed image of every probing present;
// with N above 1 the ring is empty between two probes, the tint would show next to nothing and every output would count as a skipped
// paste, so with the tint on every present probes.
constexpr int EffectiveProbeInterval(int configured, bool tint) { return tint ? 1 : ClampProbeInterval(configured); }
// [HudLess] ProbeAbSeconds: block length of the developer's blind A/B of the probe interval (0 = off).
constexpr int kProbeAbSecondsMax = 600;

// [HudLess] HdrPaste: off | on. Only "on" (case-insensitive) switches the HDR paste path on; anything else, "auto" and the empty
// string included, is off: fail open.
constexpr bool ParseHdrPaste(std::string_view text) { return detail::EqualsNoCase(detail::Trim(text), "on"); }

// [HudLess] AnchorRule: where the game-side source takes the UI-free capture on a frame whose first UI draw after the last scene stage
// goes to an off-screen render target (docs/HUDLESS.md section 10).
//   r1b   at the first UI draw into the backbuffer (today's behaviour)
//   early at that first off-screen UI draw: the swapchain backbuffer is copied there
enum class AnchorRule : uint8_t
{
    R1b,
    Early,
};

// Case-insensitive; anything unknown (including "auto" and the empty string) is r1b: fail open, today's behaviour.
constexpr AnchorRule ParseAnchorRule(std::string_view text)
{
    return detail::EqualsNoCase(detail::Trim(text), "early") ? AnchorRule::Early : AnchorRule::R1b;
}

constexpr const char* AnchorRuleName(AnchorRule rule) { return rule == AnchorRule::Early ? "early" : "r1b"; }

constexpr const char* ModeName(Mode mode)
{
    switch (mode)
    {
    case Mode::Recomposition:
        return "recomposition";
    case Mode::UiFree:
        return "ui_free";
    default:
        return "off";
    }
}
} // namespace ext::hudless
