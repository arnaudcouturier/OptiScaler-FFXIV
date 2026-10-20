#pragma once
// [Menu] Style (docs/FEATURES.md, "Menu style"). Header-only so the parser and its tests need no config machinery.
//
//   classic = upstream's look: surfaces, buttons, headers and the title bar are all tinted by the accent colour (the default)
//   ember   = flat neutral surfaces with 1px hairlines, rounder corners and more room; the accent colour only marks state
//             (check mark, slider grab, selected tab, focus ring, selection)
//
// Anything else, including an empty or missing key, reads as classic, so a typo never changes the look.

#include <cstdint>
#include <string_view>

namespace ext::menustyle
{
enum class Style : uint8_t
{
    Classic,
    Ember,
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

constexpr Style Parse(std::string_view text) noexcept
{
    return detail::EqualsNoCase(detail::Trim(text), "ember") ? Style::Ember : Style::Classic;
}

constexpr const char* Name(Style style) noexcept { return style == Style::Ember ? "ember" : "classic"; }
} // namespace ext::menustyle
