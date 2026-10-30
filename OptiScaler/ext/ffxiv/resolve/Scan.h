#pragma once
// The matcher: a chunked, bounds-safe scan of the image sections for one or many masked patterns in a single pass.
//
// Rules: scan only the sections of the requested class (Code = executable sections for code anchors), never read past a
// section's end (the old scanner\scanner.cpp FindPattern reads one byte past VirtualSize), chunk the reads (1 MiB windows that overlap
// by pattern length - 1, so a match that straddles a window border is found exactly once), keep at most kMaxScanHits raw matches per
// request and flag the overflow. Counting "exactly one" is the resolver's job; this layer reports raw matches in ascending RVA order.

#include "ImageView.h"
#include "Pattern.h"

#include <algorithm>
#include <cstring>

namespace ext::ffxiv::resolve
{
struct ScanRequest
{
    const Pattern* pattern = nullptr;
    SearchIn searchIn = SearchIn::Code;
};

struct ScanHits
{
    std::vector<std::uint32_t> rvas; // ascending
    bool overflow = false;           // more than maxHits matches existed
};

struct ScanOutcome
{
    std::vector<ScanHits> perRequest; // one entry per request, same order
    bool unreadable = false;          // a window could not be read: the hit lists are incomplete and must not be trusted
    std::string error;
};

namespace detail
{
// How common a byte is at the start of x64 code: the scan looks for the rarest fixed byte first (memchr), then verifies the rest.
inline int Commonness(std::uint8_t b)
{
    switch (b)
    {
    case 0x00:
    case 0xff:
    case 0xcc:
    case 0x48:
        return 3;
    case 0x8b:
    case 0x89:
    case 0x0f:
    case 0x24:
    case 0x41:
    case 0x83:
    case 0xe8:
    case 0x8d:
    case 0x45:
    case 0x4c:
    case 0x01:
    case 0x02:
    case 0x04:
    case 0x08:
    case 0x10:
    case 0x20:
    case 0x40:
        return 2;
    default:
        return 0;
    }
}

struct Pivot
{
    bool has = false;
    std::size_t index = 0;
    std::uint8_t byte = 0;
};

inline Pivot ChoosePivot(const Pattern& p)
{
    Pivot best;
    int bestScore = 1 << 20;
    for (std::size_t k = 0; k < p.size(); ++k)
    {
        if (p.mask[k] != 0xff)
            continue;
        const int score = Commonness(p.bytes[k]);
        if (score < bestScore)
        {
            bestScore = score;
            best = { true, k, p.bytes[k] };
        }
    }
    return best;
}
} // namespace detail

inline ScanOutcome ScanBatch(const ImageView& image, const std::vector<ScanRequest>& requests, std::size_t maxHits = kMaxScanHits,
                             std::size_t chunk = 1u << 20)
{
    ScanOutcome out;
    out.perRequest.resize(requests.size());

    struct Prep
    {
        bool usable = false;
        bool done = false;
        detail::Pivot pivot;
    };
    std::vector<Prep> prep(requests.size());
    std::size_t maxPattern = 0;
    for (std::size_t r = 0; r < requests.size(); ++r)
    {
        const Pattern* p = requests[r].pattern;
        if (p == nullptr || p->size() == 0 || p->mask.size() != p->size())
            continue;
        prep[r].usable = true;
        prep[r].pivot = detail::ChoosePivot(*p);
        maxPattern = std::max(maxPattern, p->size());
    }
    if (maxPattern == 0 || chunk == 0)
        return out;
    if (!image.Valid())
    {
        out.unreadable = true;
        out.error = "the image headers are not valid";
        return out;
    }

    std::vector<std::uint8_t> scratch(chunk + maxPattern);
    constexpr SearchIn kClasses[] = { SearchIn::Code, SearchIn::ReadOnlyData, SearchIn::InitData };
    for (const ImageSection& section : image.Sections())
    {
        for (SearchIn cls : kClasses)
        {
            std::uint32_t lo = 0, hi = 0;
            if (!SearchRange(section, cls, lo, hi))
                continue;
            bool any = false;
            for (std::size_t r = 0; r < requests.size(); ++r)
                any = any || (prep[r].usable && !prep[r].done && requests[r].searchIn == cls);
            if (!any)
                continue;

            for (std::uint64_t start = lo; start < hi; start += chunk)
            {
                // The window holds the bytes of [start, start + chunk) plus the next maxPattern - 1, all inside [lo, hi).
                const std::size_t len = static_cast<std::size_t>(std::min<std::uint64_t>(chunk + maxPattern - 1, hi - start));
                if (!image.Read(static_cast<std::uint32_t>(start), scratch.data(), len))
                {
                    out.unreadable = true;
                    out.error = std::string("section ") + section.name + " could not be read";
                    return out;
                }
                for (std::size_t r = 0; r < requests.size(); ++r)
                {
                    Prep& pr = prep[r];
                    if (!pr.usable || pr.done || requests[r].searchIn != cls)
                        continue;
                    const Pattern& p = *requests[r].pattern;
                    const std::size_t n = p.size();
                    if (len < n)
                        continue;
                    const std::size_t limit = std::min<std::size_t>(chunk, len - n + 1); // match starts in [0, limit)
                    ScanHits& hits = out.perRequest[r];
                    std::size_t i = 0;
                    while (i < limit)
                    {
                        if (pr.pivot.has)
                        {
                            const void* f = std::memchr(scratch.data() + i + pr.pivot.index, pr.pivot.byte, limit - i);
                            if (f == nullptr)
                                break;
                            i = static_cast<std::size_t>(static_cast<const std::uint8_t*>(f) - scratch.data()) - pr.pivot.index;
                        }
                        if (MatchAt(scratch.data() + i, p))
                        {
                            if (hits.rvas.size() >= maxHits)
                            {
                                hits.overflow = true;
                                pr.done = true;
                                break;
                            }
                            hits.rvas.push_back(static_cast<std::uint32_t>(start + i));
                        }
                        ++i;
                    }
                }
            }
        }
    }
    return out;
}
} // namespace ext::ffxiv::resolve
