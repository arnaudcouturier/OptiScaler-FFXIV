#pragma once
// Masked byte patterns for the resolver. Written as IDA-style text: "48 8B 05 ?? ?? ?? ?? 8B CA 48 8B 84 C8 C8 02 00 00 C3".
//   "XY"  a fixed byte      "??"  any byte      "X?" / "?Y"  a fixed high / low nibble
// A byte matches when ((data ^ pattern) & mask) == 0. Pure and header-only.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ext::ffxiv::resolve
{
inline constexpr std::size_t kMaxPatternBytes = 512;
inline constexpr std::size_t kEntryJumpBytes = 14; // FF 25 00 00 00 00 <abs64>: the longest jump a hook writes over a function entry
inline constexpr std::size_t kMinRelaxedFixedBytes = 6;

struct Pattern
{
    std::vector<std::uint8_t> bytes; // wildcard bits are stored as 0
    std::vector<std::uint8_t> mask;  // 0xFF fixed, 0x00 wildcard, 0xF0 / 0x0F nibble

    std::size_t size() const { return bytes.size(); }
    // Bytes that are fully fixed (nibble-masked and wildcard bytes do not count).
    std::size_t fixedBytes() const
    {
        std::size_t n = 0;
        for (std::uint8_t m : mask)
            n += m == 0xff ? 1 : 0;
        return n;
    }
};

namespace detail
{
inline int HexNibble(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}
} // namespace detail

// Parses `text` into `out`. False (and `*error`) for an empty pattern, a token that is not exactly two characters of [0-9a-fA-F?],
// more than kMaxPatternBytes tokens, or a pattern with no fixed bit at all (it would match everywhere).
inline bool ParsePattern(std::string_view text, Pattern& out, std::string* error = nullptr)
{
    out = Pattern {};
    auto fail = [&](const char* why)
    {
        out = Pattern {};
        if (error != nullptr)
            *error = why;
        return false;
    };

    bool anyFixed = false;
    std::size_t i = 0;
    while (i < text.size())
    {
        const char c = text[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
        {
            ++i;
            continue;
        }
        std::size_t j = i;
        while (j < text.size() && text[j] != ' ' && text[j] != '\t' && text[j] != '\r' && text[j] != '\n')
            ++j;
        if (j - i != 2)
            return fail("a pattern token must be two characters");
        const char hi = text[i], lo = text[i + 1];
        std::uint8_t byte = 0, mask = 0;
        if (hi != '?')
        {
            const int v = detail::HexNibble(hi);
            if (v < 0)
                return fail("bad hex digit in the pattern");
            byte = static_cast<std::uint8_t>(byte | (v << 4));
            mask = static_cast<std::uint8_t>(mask | 0xf0);
        }
        if (lo != '?')
        {
            const int v = detail::HexNibble(lo);
            if (v < 0)
                return fail("bad hex digit in the pattern");
            byte = static_cast<std::uint8_t>(byte | v);
            mask = static_cast<std::uint8_t>(mask | 0x0f);
        }
        anyFixed = anyFixed || mask != 0;
        if (out.bytes.size() >= kMaxPatternBytes)
            return fail("the pattern is longer than 512 bytes");
        out.bytes.push_back(byte);
        out.mask.push_back(mask);
        i = j;
    }
    if (out.bytes.empty())
        return fail("empty pattern");
    if (!anyFixed)
        return fail("the pattern has no fixed byte");
    return true;
}

// True when data[0, p.size()) matches. The caller guarantees that many readable bytes.
inline bool MatchAt(const std::uint8_t* data, const Pattern& p)
{
    const std::size_t n = p.bytes.size();
    for (std::size_t k = 0; k < n; ++k)
    {
        if (((data[k] ^ p.bytes[k]) & p.mask[k]) != 0)
            return false;
    }
    return true;
}

// A copy of `p` with the first `entryBytes` bytes made wildcards: what a function looks like after a foreign hook wrote a jump over
// its entry. Only usable when enough fixed bytes remain (kMinRelaxedFixedBytes).
inline Pattern RelaxEntry(const Pattern& p, std::size_t entryBytes = kEntryJumpBytes)
{
    Pattern r = p;
    for (std::size_t k = 0; k < r.bytes.size() && k < entryBytes; ++k)
    {
        r.bytes[k] = 0;
        r.mask[k] = 0;
    }
    return r;
}
} // namespace ext::ffxiv::resolve
