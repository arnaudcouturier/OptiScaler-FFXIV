#pragma once
// Shared helpers of the jitter-source (P2c) tests: the build-C layout image with the group's committed bytes, small readers and the
// pattern helpers the decoy tests use. Header only, tests include it next to old_build_b_jitter.h.
#include "old_build_b_jitter.h"

#include "ext/ffxiv/resolve/AnchorsJitter.h"
#include "ext/ffxiv/resolve/Resolver.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace jt
{
using namespace rt;
using namespace oldb;
using ext::ffxiv::resolve::AddJitterAnchors;

inline constexpr std::uintptr_t kBase = 0x140000000ull;

// An extra function (a decoy's home) that the exception directory lists, with a trivial unwind block (no chain, no handler).
struct ExtraFunction
{
    std::uint32_t begin, end;
};

inline Registry JitterRegistry()
{
    Registry r;
    AddJitterAnchors(r);
    return r;
}

// The build-C layout (sections as the executable has them) with a SMALL exception directory: the three real entries of the camera update
// and the generator (their unwind bytes are planted as well) plus `extra`. `stamp` other than 0x6ad10bfd makes it an UNKNOWN build for a
// resolver run (the sizes still equal C's).
inline std::unique_ptr<DenseImage> MakeJitterImage(std::uint32_t stamp = kStampC, const std::vector<ExtraFunction>& extra = {})
{
    struct Entry
    {
        std::uint32_t begin, end, unwind;
    };
    std::vector<Entry> entries;
    for (const PdataEntry& e : kPdataC)
        entries.push_back({ e.begin, e.end, e.unwind });
    std::uint32_t unwindAt = 0x3180000; // inside .pdata, past the table
    for (const ExtraFunction& f : extra)
    {
        entries.push_back({ f.begin, f.end, unwindAt });
        unwindAt += 8;
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.begin < b.begin; });

    auto img = DenseImage::Synthetic(stamp, kSizeC,
                                     { { ".text", 0x1000, 0x210bf12, 0x210c000, DenseImage::kText },
                                       { ".rdata", 0x210d000, 0x7a3c30, 0x7a3e00, DenseImage::kRdata },
                                       { ".data", 0x28b1000, 0x6e38a0, 0x56800, DenseImage::kData },
                                       { ".pdata", 0x2f95000, 0x1fb750, 0x1fb800, DenseImage::kRdata },
                                       { "_RDATA", 0x3191000, 0xf4, 0x200, DenseImage::kRdata },
                                       { ".rsrc", 0x3192000, 0x5d6dd0, 0x5d6e00, DenseImage::kRdata },
                                       { ".reloc", 0x3769000, 0xb59f4, 0xb5a00, DenseImage::kRdata | kScnDiscardable } },
                                     0x2f95000, static_cast<std::uint32_t>(entries.size() * 12));
    if (!img)
        return nullptr;
    for (std::size_t i = 0; i < entries.size(); ++i)
        img->Put(0x2f95000 + static_cast<std::uint32_t>(i) * 12, &entries[i], 12);
    {
        std::uint32_t at = 0x3180000;
        const std::uint8_t trivial[8] = { 0x01, 0, 0, 0, 0, 0, 0, 0 };
        for (std::size_t i = 0; i < extra.size(); ++i, at += 8)
            img->Put(at, trivial, sizeof(trivial));
    }
    for (const Row* row : { &kGeneratorC, &kCameraGetterC, &kCameraGetterDecoyC, &kCameraUpdateC, &kProducerC, &kDeviceSizeC, &kDeviceSizeSecondC,
                            &kStrengthFloorC, &kSignMaskC, &kUnwindHeadC, &kUnwindBodyC, &kUnwindGeneratorC })
    {
        if (!PlantRow(*img, *row))
        {
            std::printf("FAIL: could not plant row %s\n", row->site);
            ++g_failures;
        }
    }
    return img;
}

inline auto ReaderOf(const DenseImage& img)
{
    return [&img](std::uintptr_t address, void* out, std::size_t bytes)
    { return address >= kBase && address - kBase <= 0xffffffffull && img.Read(static_cast<std::uint32_t>(address - kBase), out, bytes); };
}

inline const SeamResult& Get(const ResolvedBuild& rb, const std::string& id)
{
    static const SeamResult missing;
    const SeamResult* s = rb.Find(id);
    if (s == nullptr)
    {
        std::printf("FAIL: no result for seam %s\n", id.c_str());
        ++g_failures;
        return missing;
    }
    return *s;
}

inline const SeamSpec* FindSeam(const Registry& r, const std::string& id)
{
    for (const SeamSpec& s : r.seams)
        if (s.id == id)
            return &s;
    return nullptr;
}
inline SeamSpec* FindSeam(Registry& r, const std::string& id)
{
    for (SeamSpec& s : r.seams)
        if (s.id == id)
            return &s;
    return nullptr;
}

// The bytes a pattern describes, with every wildcard byte set to `filler`.
inline std::vector<std::uint8_t> Materialize(const std::string& pattern, std::uint8_t filler = 0)
{
    Pattern p;
    ParsePattern(pattern, p);
    std::vector<std::uint8_t> out(p.size());
    for (std::size_t i = 0; i < p.size(); ++i)
        out[i] = static_cast<std::uint8_t>((p.bytes[i] & p.mask[i]) | (filler & ~p.mask[i]));
    return out;
}

inline std::vector<std::string> Tokens(const std::string& pattern)
{
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < pattern.size())
    {
        while (i < pattern.size() && pattern[i] == ' ')
            ++i;
        std::size_t j = i;
        while (j < pattern.size() && pattern[j] != ' ')
            ++j;
        if (j > i)
            out.push_back(pattern.substr(i, j - i));
        i = j;
    }
    return out;
}

inline std::string PrefixOf(const std::string& pattern, std::size_t tokens)
{
    const auto t = Tokens(pattern);
    std::string out;
    for (std::size_t i = 0; i < tokens && i < t.size(); ++i)
        out += (i == 0 ? "" : " ") + t[i];
    return out;
}

// A human readable one-line summary of an anchor, used by the pin test (every field that matters, nothing else).
inline std::string Summary(const AnchorSpec& a)
{
    auto hex = [](long long v)
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), v < 0 ? "-0x%llx" : "0x%llx", v < 0 ? -v : v);
        return std::string(buf);
    };
    static const char* kDecode[] = { "none", "rip", "rel32", "field", "deref" };
    static const char* kValue[] = { "match", "function", "target" };
    static const char* kPred[] = { "targetIn", "targetIs", "calls", "within", "sameFunction", "valueIn" };
    std::string s = a.name + " | " + a.pattern + " | " + (a.searchIn == SearchIn::Code ? "code" : "other") + " hits " + std::to_string(a.expectedHits) + "/" +
                    std::to_string(a.pickIndex) + " | from " + (a.fromSeam.empty() ? std::string("-") : a.fromSeam + "@" + hex(a.fromOffset)) + " | value " +
                    kValue[static_cast<int>(a.valueFrom)] + "+" + hex(a.valueOffset) + " | decode " + kDecode[static_cast<int>(a.decode.kind)] + "(" +
                    std::to_string(a.decode.at) + "," + std::to_string(a.decode.dispAt) + "," + std::to_string(a.decode.length) + "," +
                    std::to_string(a.decode.width) + ") | entry " + (a.entryAnchor ? "1" : "0") + " margin " + std::to_string(a.minMargin) + " |";
    for (const Predicate& p : a.predicates)
    {
        s += std::string(" ") + kPred[static_cast<int>(p.kind)];
        switch (p.kind)
        {
        case PredicateKind::TargetIn:
            s += "(" + std::to_string(static_cast<int>(p.region)) + ")";
            break;
        case PredicateKind::Within:
            s += "(" + p.seam + "," + hex(p.lo) + "," + hex(p.hi) + ")";
            break;
        case PredicateKind::ValueIn:
            s += "(" + hex(p.valueLo) + "," + hex(p.valueHi) + ")";
            break;
        default:
            s += "(" + p.seam + ")";
            break;
        }
    }
    return s;
}
} // namespace jt
