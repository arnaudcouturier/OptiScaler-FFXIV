#pragma once
// Bytes of the CURRENT build (C, PE stamp 0x6ad10bfd) that the companion anchors cover, read from analysis\patch-20261016\ffxiv_dx11.exe
// (a data file, never committed) and committed here as short hex blocks, so that the companion tests run without the executable.
// Every block is a contiguous range of the executable at its real RVA; the anchors' patterns are matched against these bytes. The ranges
// are no bigger than the code the anchors and their decoy tests need.
//
// PlantAll() writes them into a dense image with the section layout of build C and a .pdata with the real function ranges (the batch
// functions are split in three chained ranges, as in the executable), so SameFunctionAs and EnclosingFunction see the real topology.
#include "old_build_b.h"

#include "ext/ffxiv/resolve/AnchorsCompanion.h"
#include "ext/ffxiv/resolve/Entry.h"
#include "ext/ffxiv/resolve/Resolver.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace cb
{
using namespace rt;
using namespace oldb;

struct Blob
{
    const char* name;
    std::uint32_t rva;
    const char* hex; // contiguous lowercase hex
};

// companion.batch16 (C 0x6e56d0): entry through the layout lookup; the head, the header loads, the vertex slot, the extension compare, the format and the lookup all lie in it
inline constexpr Blob kBatch16 = { "batch16", 0x6e56d0,
    "8854241048894c2408555356415441554157488d6c24d84881ec28010000488b81800500004c8be9488945e841b90200"
    "00008b81880500004533c0488b89e001000033d289442468e83394b3ff488bd848894424584c8bf848894424384c8be0"
    "498b856005000048d1e8488944245033c0894424308bf04885db0f849e0600000f1005611d1d0233d24889bc24200100"
    "00f20f100d5f1d1d028bf80f1144247089442440498bcd894424440f57c0894424348944244c884570498b85c0040000"
    "4c89b42418010000448b35d1b722020f1145d0c744246001000000f20f114d8048894590897de040887424484188b571"
    "860000e8e868f5ff33d2498bcde8de6bf5ff4533c033d2498bcde8816cf5ff33d2498bcde8176df5ff41b001488d5424"
    "70498bcde8b764f5ff65488b0c25580000008b0530c75402488b04c1b9c4030200488945b8403834017509e8b4b36b01"
    "488b45b84c8b4590b938020000488b1401418b8d40050000488d04494c8984c248110000488d04494889b4c240110000"
    "418b8540050000488d0c404489b4ca5011000033c9418b950405000049039570050000458b9d00050000488955884c89"
    "5d98894d78394c24680f861e050000448b4de0448d511033c044894c2464488945c80f29b42400010000660f1f440000"
    "4c8b4de84803c04d8b64c1084d85e40f84b5040000418b04248bd881e3f0000000807d6800740783f822410f44da4533"
    "d285db0f84db0300006641837c240402448b542440400f94c7443bd375778b4424704133442408a9ffffff077567488b"
    "4424784939442410755b8b4580413944241875518b4584413344241ca9ff0700007542418b4c24308bc183e00f3b4424"
    "4475328bc1c1e8040fb6c03b44243475248bc1c1e80c0fb6c03b44244c7516c1e91483e1013a4c2448750a403a7d700f"
    "8499010000488b4c24584d8bc7482b4c2438448bce4c2b442458418bd248d1f9894c2420498bcd49d1f8e8b15df5ff41"
    "8b5424308bc2c1e8040fb6c84c897c2458895c24403b4c2434741f8bd9894c24348bd1498bcde8e566f5ff8bd3498bcd"
    "e8db69f5ff418b5424308bda83e30f3b5c2444750640387d70745d440fb6c7895c24448bd340887d70498bcde85f6af5"
    "ff8bd3498bcde8f56af5ff418b542430488d0de9a591ff48638cd960c71702488bc14803c0418b84c504050000490385"
    "7005000048894588488d41504803c0418b4cc50048894d988b44247041334424" };

// companion.batch32 (C 0x6e6270): the same code with the shift by 2 (32-bit indices)
inline constexpr Blob kBatch32 = { "batch32", 0x6e6270,
    "8854241048894c2408555356415441554157488d6c24d84881ec28010000488b81800500004c8be9488945e841b90200"
    "00008b81880500004533c0488b89e001000033d289442468e89388b3ff488bd848894424584c8bf848894424384c8be0"
    "498b856005000048c1e802488944245033c0894424308bf04885db0f84b00600000f1005c0111d0233d24889bc242001"
    "0000f20f100dbe111d028bf80f1144247089442440498bcd894424440f57c0894424348944244c884570498b85c00400"
    "004c89b42418010000448b3530ac22020f1145d0c744246001000000f20f114d8048894590897de040887424484188b5"
    "71860000e8475df5ff33d2498bcde83d60f5ff4533c033d2498bcde8e060f5ff33d2498bcde87661f5ff41b001488d54"
    "2470498bcde81659f5ff8b0598bb540265488b0c2558000000bac4030200488b04c1488945b8403834027509e813a86b"
    "01488b45b84c8b4590b938020000488b1401418b8d40050000488d04494c8984c248110000488d04494889b4c2401100"
    "00418b8540050000488d0c404489b4ca5011000033c9418b950405000049039570050000458b9d00050000488955884c"
    "895d98894d78394c24680f862e050000448b4de0448d511033c044894c2464488945c80f29b424000100000f1f440000"
    "4c8b4de84803c04d8b64c1084d85e40f84c6040000418b04248bd881e3f0000000807d6800740783f822410f44da4533"
    "d285db0f84ea0300006641837c240402448b542440400f94c7443bd375778b4424704133442408a9ffffff077567488b"
    "4424784939442410755b8b4580413944241875518b4584413344241ca9ff0700007542418b4c24308bc183e00f3b4424"
    "4475328bc1c1e8040fb6c03b44243475248bc1c1e80c0fb6c03b44244c7516c1e91483e1013a4c2448750a403a7d700f"
    "849b010000488b4c24584d8bc7482b4c2438448bce4c2b442458418bd248c1f902894c2420498bcd49c1f802e80f52f5"
    "ff418b5424308bc2c1e8040fb6c84c897c2458895c24403b4c2434741f8bd9894c24348bd1498bcde8435bf5ff8bd349"
    "8bcde8395ef5ff418b5424308bda83e30f3b5c2444750640387d70745d440fb6c7895c24448bd340887d70498bcde8bd"
    "5ef5ff8bd3498bcde8535ff5ff418b542430488d0d479a91ff48638cd960c71702488bc14803c0418b84c50405000049"
    "03857005000048894588488d41504803c0418b4cc50048894d988b4424704133442408a9ffffff077542488b44247849" };

// the dispatcher call site (C 0x63b6f0, inside function 0x63b250): mov rax,[rbx+0x1e0] .. cmp [rax+0x3c],2 .. call batch32 .. jmp .. call batch16
inline constexpr Blob kDispatcher = { "dispatcher", 0x63b6f0,
    "488b83e0010000400fb6d5488bcb83783c027507e867ab0a00eb05e8c09f0a00" };

// the caller of enqueue (C 0x23c0d2): the tail of a wrapper, mov rdx,rdi ; mov [rdi+0x30],r14 ; mov rcx,rsi ; call enqueue ; the epilogue. The only
// caller-side bytes the third enqueue anchor (callerEpilogue) needs: a real call target, so the entry address does not rest on the leaf alone.
inline constexpr Blob kEnqueueCaller = { "enqueueCaller", 0x23c0d2,
    "488bd74c897730488bcee89f4a0000488b5c2430488b6c2438488b742440488b7c24484883c420415ec3" };

// the tail-call dispatcher stub (C 0x63b730): mov rax,[rcx+0x1e0] ; cmp [rax+0x3c],2 ; je batch32 ; jmp batch16. The second caller of batch16.
inline constexpr Blob kDispatcherStub = { "dispatcherStub", 0x63b730,
    "488b81e001000083783c020f842fab0a00e98a9f0a00" };

// companion.enqueue (C 0x240b80), the whole leaf function
inline constexpr Blob kEnqueue = { "enqueue", 0x240b80,
    "4863410c4c8bda4c8d04408b4108428344c128ff4e8d0cc10f85850000004c8b055b716c02ba00400000f0410fc190d4"
    "0000004d8b90c000000081c2004000008d0c52410388d00000004c3bd17317b800c0fffff0410fc180d400000041c741"
    "2801000000c38bca498b90c8000000482bd14903d2750941c7412801000000c3498b4920488911498b492048c7410800"
    "00000041ff412c4989512041c74128ff030000498b5120488d4a104989492089024c895a08c3" };

// companion.bindState (C 0x22d750), entry through the context call
inline constexpr Blob kBindState = { "bindState", 0x22d750,
    "405641564883ec48807a49004c8bf248895c2470488bf148896c24784c89642438458be04c897c24300f84be0000008b"
    "42188b6a0825ffffff076bc81f81e5ffffff078b421425ffffff0703c8418b461025ffffff076bd11f03d0418b460c25"
    "ffffff076bca1f03c86bc11f03e839aeb4170000750980beb317000000757648897c24404d8d4608488bbeb81700008b"
    "d5488b07488b9818010000488b050ea56d02488b88800a0e00e8e2a6000041b9ffffffff4c8d05ad988702488bd0488b"
    "cfffd3488b7c2440c686b31700000189aeb4170000410f1046080f118698000000418b46188986a8000000eb088b5208"
    "e88b360100498b0648b900e0fffffeffffff4c8bc083e01f4c23c18bd0488bcee8eb390100418b561c488bcee8bf4501"
    "00498b4620488bce458b4e504d8b4658498b563844896424284889442420e8dde5ffff498b5e304c8b7c24304c8b6424"
    "38488b6c247848399ec0000000743d4885db7431837b3c02ba39000000488b8eb817000041b82a000000440f45c28b15"
    "5c366e024533c9488b01488b54d348ff909800000048899ec00000004d8b4660" };

// the index-buffer map (C 0x21eb50), entry through the flag logic
inline constexpr Blob kIndexMap = { "indexMap", 0x21eb50,
    "48895c2408574883ec20488bd98b4938418bf84585c075048bf92bfa8d043a3bc80f82990000008b4b40f6c103740a41"
    "f6c1010f85870000008b05a5236f0248837cc34800750748837b60007472f6c1117423f6c140740f488b4360488b5c24"
    "304883c4205fc3488b4368488b5c24304883c4205fc3488b53604885d27419488b0502036f02488b4810488b01ff5020"
    "48c7436000000000488b05e9026f0241b8100000008bd7488b4810488b01ff501048894360488b5c24304883c4205fc3"
    "488b5c243033c04883c4205fc3cccccc" };

// nameplate chain site 1 (C 0x67361c)
inline constexpr Blob kChain1 = { "chain1", 0x67361c,
    "488b052d5044020fb691ef010000488b4808488b4108899090000000410fb780" };

// nameplate chain site 2 (C 0x673696)
inline constexpr Blob kChain2 = { "chain2", 0x673696,
    "488b05b34f4402488b4808488b4108c78090000000000000004883c428c3cccc" };

// nameplate chain site 3 (C 0x6a5324)
inline constexpr Blob kChain3 = { "chain3", 0x6a5324,
    "488b0525334102488b58080f859c000000488b4b0841c1ed0b410fb7c52bf039" };

// the layout -> vertex slot table in .rdata (C 0x217c760), 9 entries of 8 bytes
inline constexpr Blob kLayoutTable = { "layoutTable", 0x217c760,
    "000000000000000000000000000000000000000000000000000000000000000001000000050000000200000004000000"
    "020000000300000001000000020000000100000001000000" };

// A RUNTIME_FUNCTION range of the synthetic .pdata. `parent` >= 0 chains the range to that entry of the same list (UNW_FLAG_CHAININFO), the
// way the compiler splits a big function (the batch functions have three ranges each in the executable).
struct Range
{
    std::uint32_t begin, end;
    int parent; // index into the same vector, or -1
};

// the image layout of build C (old_build_b.h MakeCImage), but with an exception directory of `count` entries
inline std::unique_ptr<DenseImage> NewCImage(std::size_t count)
{
    return DenseImage::Synthetic(kStampC, kSizeC,
                                 { { ".text", 0x1000, 0x210bf12, 0x210c000, DenseImage::kText },
                                   { ".rdata", 0x210d000, 0x7a3c30, 0x7a3e00, DenseImage::kRdata },
                                   { ".data", 0x28b1000, 0x6e38a0, 0x56800, DenseImage::kData },
                                   { ".pdata", 0x2f95000, 0x1fb750, 0x1fb800, DenseImage::kRdata },
                                   { "_RDATA", 0x3191000, 0xf4, 0x200, DenseImage::kRdata },
                                   { ".rsrc", 0x3192000, 0x5d6dd0, 0x5d6e00, DenseImage::kRdata },
                                   { ".reloc", 0x3769000, 0xb59f4, 0xb5a00, DenseImage::kRdata | kScnDiscardable } },
                                 0x2f95000, static_cast<std::uint32_t>(count * 12));
}

inline constexpr std::uint32_t kPdataC = 0x2f95000, kUnwindC = 0x3192000;

// Writes the ranges (sorted by begin, as the exception directory must be) and one UNWIND_INFO block per entry (version 1, no codes; a chained
// entry carries its parent's RUNTIME_FUNCTION after the codes).
inline std::unique_ptr<DenseImage> MakeImageWithFunctions(std::vector<Range> ranges)
{
    // chain indices refer to the order given: resolve them to begin addresses before sorting
    std::vector<std::uint32_t> parentBegin(ranges.size(), 0);
    for (std::size_t i = 0; i < ranges.size(); ++i)
        parentBegin[i] = ranges[i].parent >= 0 ? ranges[static_cast<std::size_t>(ranges[i].parent)].begin : 0;
    std::vector<std::size_t> order(ranges.size());
    for (std::size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return ranges[a].begin < ranges[b].begin; });
    auto img = NewCImage(ranges.size());
    if (!img)
        return nullptr;
    auto unwindOf = [&](std::size_t rangeIndex) { return kUnwindC + static_cast<std::uint32_t>(rangeIndex) * 0x40; };
    for (std::size_t slot = 0; slot < order.size(); ++slot)
    {
        const std::size_t i = order[slot];
        const Range& r = ranges[i];
        const std::uint32_t e[3] = { r.begin, r.end, unwindOf(i) };
        img->Put(kPdataC + static_cast<std::uint32_t>(slot) * 12, e, sizeof(e));
        if (r.parent < 0)
            PutUnwind(*img, unwindOf(i), 0, 0);
        else
        {
            const Range& p = ranges[static_cast<std::size_t>(r.parent)];
            PutUnwind(*img, unwindOf(i), 4, 0, p.begin, p.end, unwindOf(static_cast<std::size_t>(r.parent)));
        }
    }
    return img;
}

// the function ranges of the executable for the functions whose bytes are committed here
inline std::vector<Range> RealRanges()
{
    return { { 0x21eb50, 0x21ec20, -1 }, // index-buffer map
             { 0x22d750, 0x22d8f0, -1 }, // bindState
             { 0x6e56d0, 0x6e5759, -1 }, { 0x6e5759, 0x6e58a2, 2 }, { 0x6e58a2, 0x6e5a50, 2 },   // batch16
             { 0x6e6270, 0x6e62fa, -1 }, { 0x6e62fa, 0x6e6443, 5 }, { 0x6e6443, 0x6e6600, 5 } }; // batch32
}

inline void PlantBlob(DenseImage& img, const Blob& blob, std::uint32_t at = 0)
{
    Plant(img, at == 0 ? blob.rva : at, blob.hex);
}

// every block of build C at its real RVA
inline void PlantAll(DenseImage& img)
{
    for (const Blob* b : { &kBatch16, &kBatch32, &kDispatcher, &kDispatcherStub, &kEnqueue, &kEnqueueCaller, &kBindState, &kIndexMap, &kChain1, &kChain2, &kChain3,
                           &kLayoutTable })
        PlantBlob(img, *b);
}

inline std::unique_ptr<DenseImage> MakeRealImage()
{
    auto img = MakeImageWithFunctions(RealRanges());
    if (img)
        PlantAll(*img);
    return img;
}

// the registry of this group alone (a worktree is self-contained: nothing here depends on another group's seams)
inline Registry CompanionRegistry()
{
    Registry r;
    ext::ffxiv::resolve::AddCompanionAnchors(r);
    return r;
}

inline const SeamSpec* FindSpec(const Registry& reg, const std::string& id)
{
    for (const SeamSpec& s : reg.seams)
    {
        if (s.id == id)
            return &s;
    }
    return nullptr;
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

// little-endian rel32 of a call at `from` (the call instruction's own RVA) to `to`
inline std::string Rel32Hex(std::uint32_t from, std::uint32_t to)
{
    const std::int32_t rel = static_cast<std::int32_t>(to - (from + 5));
    std::vector<std::uint8_t> b(4);
    std::memcpy(b.data(), &rel, 4);
    return ToHex(b);
}
} // namespace cb
