#pragma once
// The previous FFXIV build ("B", PE stamp 0x6ac3dc96) as a test fixture, and the current build ("C", 0x6ad10bfd) layout.
//
// The B executable no longer exists. What survives is a set of byte strings that were read from it and recorded before it was removed.
// A row is evidence for a second build ONLY when its confidence is exactly "B-read". Rows that are
// "B-read-partial" carry a mask (maskB: ff = recorded, 00 = unknown placeholder), and the placeholder bytes are never planted. Rows
// derived from the C bytes ("derived-from-C", "derived-from-B-RVAs") are refused by PlantBRow: a pattern written from C passes on them
// by construction, so they prove nothing about B.
//
// Rows are per group so that the P2 writers do not share a file: old_build_b_quality.h (CharaView, written in P1), and the writers add
// old_build_b_companion.h / old_build_b_jitter.h next to their own tests.
//
// The B section table: the start VAs and the .text size are recorded; the other virtual sizes are NOT, so each is chosen to fit before the
// next section. The one that matters, `.data`, has the recorded initialized end 0x28f2800 (raw size 0x56800): the B globals lie above it,
// in the bss tail, which is what a TargetIn(Bss) predicate sees.

#include "resolve_test_util.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace oldb
{
using namespace rt;

struct Row
{
    const char* site;
    std::uint32_t rva;        // in the build the row belongs to
    const char* bytes;        // contiguous lowercase hex
    const char* mask;         // contiguous hex, ff = recorded, 00 = unknown; empty = every byte recorded
    const char* confidence;   // "B-read", "B-read-partial", "derived-from-C", ...
};

inline constexpr std::uint32_t kStampB = 0x6ac3dc96, kSizeB = 0x380a000;
inline constexpr std::uint32_t kStampC = 0x6ad10bfd, kSizeC = 0x381f000;

// A 58 MB zero-filled image with the section layout of build B. Sizes not recorded are chosen (see the file header).
inline std::unique_ptr<DenseImage> MakeBImage()
{
    return DenseImage::Synthetic(kStampB, kSizeB,
                                 { { ".text", 0x1000, 0x210bf42, 0x210c000, DenseImage::kText },
                                   { ".rdata", 0x210d000, 0x78ef00, 0x78f000, DenseImage::kRdata },
                                   { ".data", 0x289c000, 0x6e38a0, 0x56800, DenseImage::kData },
                                   { ".pdata", 0x2f80000, 0x1fb750, 0x1fb800, DenseImage::kRdata },
                                   { ".rsrc", 0x317d000, 0x5d6dd0, 0x5d6e00, DenseImage::kRdata },
                                   { ".reloc", 0x3754000, 0xb59f4, 0xb5a00, DenseImage::kRdata | kScnDiscardable } },
                                 0x2f80000, 0);
}

// A zero-filled image with the section layout of build C (parsed from the executable).
inline std::unique_ptr<DenseImage> MakeCImage()
{
    return DenseImage::Synthetic(kStampC, kSizeC,
                                 { { ".text", 0x1000, 0x210bf12, 0x210c000, DenseImage::kText },
                                   { ".rdata", 0x210d000, 0x7a3c30, 0x7a3e00, DenseImage::kRdata },
                                   { ".data", 0x28b1000, 0x6e38a0, 0x56800, DenseImage::kData },
                                   { ".pdata", 0x2f95000, 0x1fb750, 0x1fb800, DenseImage::kRdata },
                                   { "_RDATA", 0x3191000, 0xf4, 0x200, DenseImage::kRdata },
                                   { ".rsrc", 0x3192000, 0x5d6dd0, 0x5d6e00, DenseImage::kRdata },
                                   { ".reloc", 0x3769000, 0xb59f4, 0xb5a00, DenseImage::kRdata | kScnDiscardable } },
                                 0x2f95000, 0);
}

// Plants a row's recorded bytes (mask aware: unknown bytes are left untouched).
inline bool PlantRow(DenseImage& img, const Row& row)
{
    const auto bytes = FromHex(row.bytes);
    const auto mask = std::string(row.mask).empty() ? std::vector<std::uint8_t>(bytes.size(), 0xff) : FromHex(row.mask);
    if (bytes.empty() || mask.size() != bytes.size())
        return false;
    for (std::size_t i = 0; i < bytes.size(); ++i)
    {
        if (mask[i] == 0)
            continue; // an unknown byte holds a 00 placeholder in the fixture: planting it would plant a fake byte
        if (!img.Put(row.rva + static_cast<std::uint32_t>(i), &bytes[i], 1))
            return false;
    }
    return true;
}

// Plants a row of build B. Refuses (returns false, plants nothing) every row that is not "B-read" or "B-read-partial": the derived tiers
// are not evidence about B.
inline bool PlantBRow(DenseImage& img, const Row& row)
{
    const std::string conf = row.confidence;
    if (conf != "B-read" && conf != "B-read-partial")
        return false;
    return PlantRow(img, row);
}
} // namespace oldb
