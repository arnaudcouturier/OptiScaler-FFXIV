#pragma once
// Shared helpers of the quality-group resolver tests (P2a): the registry of the group alone, a dense image with the layout of build C, the
// recorded bytes of every site planted into it, and the .pdata entries that the EnclosingFunction / SameFunctionAs anchors need.
//
// Why the group's own registry and not BuildRegistry(): the three P2 groups are merged later, and a test of this group must neither depend on
// the other groups' seams nor change when they are added.
#include "old_build_b_quality.h"
#include "resolve_quality_data.h"

#include "ext/ffxiv/resolve/Entry.h"
#include "ext/ffxiv/resolve/Registry.h"
#include "ext/ffxiv/resolve/Resolver.h"

#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace qt
{
using namespace rt;
using namespace oldb;

inline Registry QualityRegistry()
{
    Registry r;
    AddQualityAnchors(r);
    return r;
}

// The bytes of a Guard field of a resolved build, and the value of an Rva field (by key, so a test names the field the way a binding does).
inline std::vector<std::uint8_t> GuardOf(const GameBuild& b, const char* key)
{
    const FieldRow* row = FindField(key);
    if (row == nullptr || row->kind != FieldKind::Guard)
    {
        std::printf("FAIL: %s is not a Guard field\n", key);
        ++g_failures;
        return {};
    }
    const std::uint8_t* p = FieldBytes(b, *row);
    return std::vector<std::uint8_t>(p, p + row->size);
}
inline bool AllZero(const std::vector<std::uint8_t>& v)
{
    for (std::uint8_t x : v)
    {
        if (x != 0)
            return false;
    }
    return !v.empty();
}
inline std::uint32_t RvaOf(const GameBuild& b, const char* key)
{
    const FieldRow* row = FindField(key);
    const std::uint32_t* p = row != nullptr ? FieldU32(b, *row) : nullptr;
    if (p == nullptr)
    {
        std::printf("FAIL: %s is not a 32 bit field\n", key);
        ++g_failures;
        return 0;
    }
    return *p;
}

// The P3 rule for a hooked function (AnchorsQuality.h, P3 CONTRACT): usable only when BOTH its location seam and its shape seam are usable. The
// same call on both builds: on an unknown build UsableOrTable is Usable, on a known build a location seam that fell back to the table counts
// and a failClosed shape seam never falls back (its Guard stays zero, so the hooks' own byte compare refuses).
inline bool HookSiteUsable(const ResolvedBuild& rb, const char* location, const char* shape)
{
    return rb.UsableOrTable(location) && rb.UsableOrTable(shape);
}

inline const SeamSpec* FindSeam(const Registry& reg, std::string_view id)
{
    for (const SeamSpec& s : reg.seams)
    {
        if (s.id == id)
            return &s;
    }
    return nullptr;
}
inline SeamSpec* FindSeam(Registry& reg, std::string_view id)
{
    return const_cast<SeamSpec*>(FindSeam(static_cast<const Registry&>(reg), id));
}

inline const SeamResult& Get(const ResolvedBuild& rb, const char* id)
{
    static const SeamResult missing;
    const SeamResult* s = rb.Find(id);
    if (s == nullptr)
    {
        std::printf("FAIL: no result for seam %s\n", id);
        ++g_failures;
        return missing;
    }
    return *s;
}

// ---- the .pdata of the sites that need it ----
//
// Eight RUNTIME_FUNCTION entries, sorted by begin address, in the real layout of the functions of build C (measured on the executable):
//   rendererUpdate  0x2da5b0 (entry 0) with the chained ranges 0x2daa8f (holds the resize block 0x2dab0d) and 0x2dab80 (holds the size block 0x2dab9a)
//   querySize       0x352350 (one range)
//   contextResolver 0x374590 (entry 4) with the chained ranges 0x3745c2 (holds the settings block 0x3745d7) and 0x37473d..0x3747ac (holds the
//                   second flags anchor 0x37479c; the real function has more ranges between them, the test image does not need them)
//   a decoy function 0x600000..0x601000 (an unrelated function that the decoy tests plant their copies in)
inline constexpr std::uint32_t kPdataC = 0x2f95000;
inline constexpr std::uint32_t kUnwindBase = 0x2200000; // inside .rdata of build C, unused by the sites
inline constexpr std::uint32_t kDecoyFunction = 0x600000, kDecoyEnd = 0x601000;
inline constexpr std::uint32_t kDecoyRdata = 0x2210000; // a .rdata address for RIP decoys
inline constexpr std::uint32_t kPdataEntries = 8;

inline void PutRf(DenseImage& img, std::uint32_t index, std::uint32_t begin, std::uint32_t end, std::uint32_t unwind)
{
    const std::uint32_t e[3] = { begin, end, unwind };
    if (!img.Put(kPdataC + index * 12, e, sizeof(e)))
    {
        std::printf("FAIL: could not write RUNTIME_FUNCTION %u\n", index);
        ++g_failures;
    }
}

inline void InstallPdata(DenseImage& img)
{
    const std::uint32_t u[8] = { kUnwindBase,        kUnwindBase + 0x40,  kUnwindBase + 0x80,  kUnwindBase + 0xc0,
                                 kUnwindBase + 0x100, kUnwindBase + 0x140, kUnwindBase + 0x180, kUnwindBase + 0x1c0 };
    PutRf(img, 0, 0x2da5b0, 0x2da5e0, u[0]);
    PutRf(img, 1, 0x2daa8f, 0x2dab80, u[1]);
    PutRf(img, 2, 0x2dab80, 0x2dac20, u[2]);
    PutRf(img, 3, 0x352350, 0x3523da, u[3]);
    PutRf(img, 4, 0x374590, 0x3745c2, u[4]);
    PutRf(img, 5, 0x3745c2, 0x3746c3, u[5]);
    PutRf(img, 6, 0x37473d, 0x3747ac, u[6]);
    PutRf(img, 7, kDecoyFunction, kDecoyEnd, u[7]);
    PutUnwind(img, u[0], 0, 0);
    PutUnwind(img, u[1], 4, 0, 0x2da5b0, 0x2da5e0, u[0]);
    PutUnwind(img, u[2], 4, 0, 0x2da5b0, 0x2da5e0, u[0]);
    PutUnwind(img, u[3], 0, 0);
    PutUnwind(img, u[4], 0, 0);
    PutUnwind(img, u[5], 4, 0, 0x374590, 0x3745c2, u[4]);
    PutUnwind(img, u[6], 4, 0, 0x374590, 0x3745c2, u[4]);
    PutUnwind(img, u[7], 0, 0);
}

// A dense image with the section layout of build C and the exception directory of InstallPdata. Nothing planted yet.
inline std::unique_ptr<DenseImage> MakeQualityC()
{
    auto img = DenseImage::Synthetic(kStampC, kSizeC,
                                     { { ".text", 0x1000, 0x210bf12, 0x210c000, DenseImage::kText },
                                       { ".rdata", 0x210d000, 0x7a3c30, 0x7a3e00, DenseImage::kRdata },
                                       { ".data", 0x28b1000, 0x6e38a0, 0x56800, DenseImage::kData },
                                       { ".pdata", kPdataC, 0x1fb750, 0x1fb800, DenseImage::kRdata },
                                       { "_RDATA", 0x3191000, 0xf4, 0x200, DenseImage::kRdata },
                                       { ".rsrc", 0x3192000, 0x5d6dd0, 0x5d6e00, DenseImage::kRdata },
                                       { ".reloc", 0x3769000, 0xb59f4, 0xb5a00, DenseImage::kRdata | kScnDiscardable } },
                                     kPdataC, kPdataEntries * 12);
    if (!img)
        return nullptr;
    InstallPdata(*img);
    return img;
}

// Plants every recorded site of build C (the rows of resolve_quality_data.h).
inline bool PlantSites(DenseImage& img)
{
    bool ok = true;
    for (const Site& s : kSites)
        ok = img.Put(s.rva, FromHex(s.hex)) && ok;
    return ok;
}

inline std::unique_ptr<DenseImage> MakeQualityCWithSites()
{
    auto img = MakeQualityC();
    if (img && !PlantSites(*img))
    {
        std::printf("FAIL: could not plant the recorded sites\n");
        ++g_failures;
    }
    return img;
}

inline const Site& SiteNamed(const char* name)
{
    for (const Site& s : kSites)
    {
        if (std::string_view(s.name) == name)
            return s;
    }
    std::printf("FAIL: no recorded site %s\n", name);
    ++g_failures;
    return kSites[0];
}

inline std::uint8_t ByteAt(const DenseImage& img, std::uint32_t rva)
{
    std::uint8_t v = 0;
    img.Read(rva, &v, 1);
    return v;
}
inline void SetByte(DenseImage& img, std::uint32_t rva, std::uint8_t v)
{
    if (!img.Put(rva, &v, 1))
    {
        std::printf("FAIL: could not write 0x%x\n", rva);
        ++g_failures;
    }
}
inline void SetU32(DenseImage& img, std::uint32_t rva, std::uint32_t v)
{
    if (!img.Put(rva, &v, 4))
    {
        std::printf("FAIL: could not write 0x%x\n", rva);
        ++g_failures;
    }
}

// ---- a readable one-line form of an anchor, for the pin tests ----
inline const char* DecodeName(DecodeKind k)
{
    switch (k)
    {
    case DecodeKind::None:
        return "None";
    case DecodeKind::RipDisp32:
        return "Rip";
    case DecodeKind::Rel32:
        return "Rel32";
    case DecodeKind::Field:
        return "Field";
    case DecodeKind::RipDeref32:
        return "Deref";
    }
    return "?";
}
inline const char* ValueFromName(ValueFrom v)
{
    return v == ValueFrom::Match ? "Match" : (v == ValueFrom::EnclosingFunction ? "Encl" : "Target");
}
inline const char* RegionName(RegionKind r)
{
    switch (r)
    {
    case RegionKind::Image:
        return "Image";
    case RegionKind::Code:
        return "Code";
    case RegionKind::ReadOnlyData:
        return "Rdata";
    case RegionKind::InitData:
        return "Init";
    case RegionKind::Bss:
        return "Bss";
    }
    return "?";
}
inline std::string PredicatesText(const AnchorSpec& a)
{
    std::string s;
    for (const Predicate& p : a.predicates)
    {
        if (!s.empty())
            s += ";";
        switch (p.kind)
        {
        case PredicateKind::TargetIn:
            s += std::string("TargetIn:") + RegionName(p.region);
            break;
        case PredicateKind::TargetIsSeam:
            s += "TargetIs:" + p.seam;
            break;
        case PredicateKind::CallsSeam:
            s += "CallsSeam:" + p.seam;
            break;
        case PredicateKind::Within:
            s += "Within:" + p.seam + ":" + std::to_string(p.lo) + ":" + std::to_string(p.hi);
            break;
        case PredicateKind::SameFunctionAs:
            s += "SameFunctionAs:" + p.seam;
            break;
        case PredicateKind::ValueIn:
            s += "ValueIn:" + std::to_string(p.valueLo) + "-" + std::to_string(p.valueHi);
            break;
        }
    }
    return s;
}

// What a pin test states about one anchor (everything the resolver reads from an AnchorSpec).
struct ExpectAnchor
{
    const char* name;
    const char* pattern;
    const char* from;
    int fromOffset;
    ValueFrom valueFrom;
    int valueOffset;
    DecodeKind kind;
    int at;
    int dispAt;
    int length;
    int width;
    bool entry;
    unsigned minMargin;
    const char* preds;
};

inline bool AnchorEquals(const AnchorSpec& a, const ExpectAnchor& e, std::string* why = nullptr)
{
    auto fail = [&](const char* what)
    {
        if (why != nullptr)
            *why = std::string(what) + " of anchor '" + a.name + "'";
        return false;
    };
    if (a.name != e.name)
        return fail("name");
    if (a.pattern != e.pattern)
        return fail("pattern");
    if (a.searchIn != SearchIn::Code || a.expectedHits != 1 || a.pickIndex != 0)
        return fail("searchIn / expectedHits / pickIndex");
    if (a.fromSeam != e.from || a.fromOffset != e.fromOffset)
        return fail("fromSeam / fromOffset");
    if (a.valueFrom != e.valueFrom || a.valueOffset != e.valueOffset)
        return fail("valueFrom / valueOffset");
    if (a.decode.kind != e.kind || a.decode.at != e.at || a.decode.dispAt != e.dispAt || a.decode.length != e.length || a.decode.width != e.width)
        return fail("decode");
    if (a.entryAnchor != e.entry)
        return fail("entryAnchor");
    if (a.minMargin != e.minMargin)
        return fail("minMargin");
    if (PredicatesText(a) != e.preds)
        return fail("predicates");
    return true;
}
} // namespace qt
