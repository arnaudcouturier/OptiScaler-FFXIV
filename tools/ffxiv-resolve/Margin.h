#pragma once
// OFFLINE ONLY. Never include this from OptiScaler\ (the product): it lives under tools\ so that the product DLL cannot pick it up.
//
// The uniqueness margin of a pattern: the smallest number of fixed bytes in which the pattern differs from ANY other location of the
// searched sections (the real hits, which the caller excludes, do not count). "Exactly one hit" only says the pattern is unique today;
// a pattern whose nearest other site differs in one or two bytes is one compiler tweak away from two hits. The tests and the CLI compute
// it on the executable; the game never does.
//
//   distance of a location = number of pattern bytes with a fixed bit whose data byte differs in a fixed bit
//   margin                 = the minimum distance over all locations except the excluded ones
//
// MinDistance stops counting at `cap`: a result of cap + 1 with found == false means "every other site differs in more than cap bytes".

#include "ext/ffxiv/resolve/ImageView.h"
#include "ext/ffxiv/resolve/Pattern.h"
#include "ext/ffxiv/resolve/ResolveTypes.h"

#include <algorithm>
#include <vector>

namespace ext::ffxiv::resolve::offline
{
inline constexpr std::uint32_t kDefaultMarginCap = 6;
inline constexpr std::uint32_t kRequiredMargin = 3; // the default AnchorSpec::minMargin: sites within 1 or 2 bytes are too close

struct MarginResult
{
    bool found = false;               // a site within `cap` bytes exists
    std::uint32_t distance = 0;       // the smallest distance found (cap + 1 when none within cap)
    std::uint32_t rva = 0;            // where the nearest site is (valid when found)
    std::uint32_t sitesAtDistance = 0; // how many sites share the smallest distance (counted up to the whole search)
    bool unreadable = false;
};

inline MarginResult MinDistance(const ImageView& image, const Pattern& pattern, SearchIn where, const std::vector<std::uint32_t>& exclude,
                                std::uint32_t cap = kDefaultMarginCap, std::size_t chunk = 1u << 20)
{
    MarginResult result;
    result.distance = cap + 1;
    const std::size_t n = pattern.size();
    if (n == 0 || chunk == 0 || !image.Valid())
        return result;

    std::vector<std::size_t> fixed; // pattern positions that carry a fixed bit
    for (std::size_t k = 0; k < n; ++k)
    {
        if (pattern.mask[k] != 0)
            fixed.push_back(k);
    }
    std::vector<std::uint8_t> window(chunk + n);

    for (const ImageSection& section : image.Sections())
    {
        std::uint32_t lo = 0, hi = 0;
        if (!SearchRange(section, where, lo, hi))
            continue;
        for (std::uint64_t start = lo; start < hi; start += chunk)
        {
            const std::size_t len = static_cast<std::size_t>(std::min<std::uint64_t>(chunk + n - 1, hi - start));
            if (!image.Read(static_cast<std::uint32_t>(start), window.data(), len))
            {
                result.unreadable = true;
                return result;
            }
            if (len < n)
                continue;
            const std::size_t limit = std::min<std::size_t>(chunk, len - n + 1);
            for (std::size_t i = 0; i < limit; ++i)
            {
                // count mismatches, give up as soon as this site cannot beat the best so far
                const std::uint32_t best = result.distance;
                std::uint32_t mismatches = 0;
                for (std::size_t k : fixed)
                {
                    if (((window[i + k] ^ pattern.bytes[k]) & pattern.mask[k]) != 0 && ++mismatches > best)
                        break;
                }
                if (mismatches > best)
                    continue;
                const std::uint32_t rva = static_cast<std::uint32_t>(start + i);
                if (std::find(exclude.begin(), exclude.end(), rva) != exclude.end())
                    continue;
                if (mismatches < best)
                {
                    result.distance = mismatches;
                    result.rva = rva;
                    result.sitesAtDistance = 1;
                    result.found = mismatches <= cap;
                }
                else if (mismatches == best && result.found)
                    ++result.sitesAtDistance;
            }
        }
    }
    return result;
}

// A margin passes when it reaches `required` (0 = the predicates are the disambiguator: always passes).
inline bool MarginOk(const MarginResult& r, std::uint32_t required)
{
    return required == 0 || (!r.unreadable && r.distance >= required); // no site within the cap leaves distance = cap + 1
}

// The margin of one anchor of a resolved seam: its pattern against everything except the candidates the resolver kept.
inline MarginResult AnchorMargin(const ImageView& image, const AnchorSpec& spec, const AnchorResult& result, std::uint32_t cap = kDefaultMarginCap)
{
    Pattern p;
    if (!spec.fromSeam.empty() || !ParsePattern(spec.pattern, p))
        return MarginResult { false, cap + 1, 0, 0, false }; // a guard anchor is not scanned: no margin to speak of
    return MinDistance(image, p, spec.searchIn, result.candidateMatches, cap);
}
} // namespace ext::ffxiv::resolve::offline
