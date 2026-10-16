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
