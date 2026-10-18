#pragma once
// [Window] Mode (docs/FEATURES.md, "Window mode"). Header-only so the pure rules and their tests can use it without the
// config machinery.
//
//   off   = nothing runs and nothing is logged (the default)
//   probe = log what the window looks like at every bridge resize and every ReShade set_fullscreen_state call; change nothing
//   on    = probe, plus the border fix (clear a stray window edge after leaving the fork's borderless "fullscreen") and the
//           deferred mode switch (frame generation goes off before the game's window-mode call reaches the hook)

#include <cstdint>
#include <string_view>

namespace ext::window
{
enum class Mode : uint8_t
{
    Off,
    Probe,
    On,
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

// Case-insensitive. Anything unknown (including "auto", "true" and the empty string) is Off: fail open.
constexpr Mode ParseMode(std::string_view text)
{
    text = detail::Trim(text);
    if (detail::EqualsNoCase(text, "on"))
        return Mode::On;
    if (detail::EqualsNoCase(text, "probe"))
        return Mode::Probe;
    return Mode::Off;
}

constexpr const char* ModeName(Mode mode)
{
    switch (mode)
    {
    case Mode::On:
        return "on";
    case Mode::Probe:
        return "probe";
    default:
        return "off";
    }
}
} // namespace ext::window
