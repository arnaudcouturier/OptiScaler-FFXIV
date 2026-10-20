#include "check.h"
#include "ext/ffxiv/GameBuild.h"
#include "ext/ffxiv/chara/CharaTable.h"

#include <cstring>
#include <initializer_list>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

using namespace ext::ffxiv;
using namespace ext::ffxiv::chara::table;

namespace
{
const CharaViewBuild kBuild {};

// 18 bytes with the displacement filled in.
std::vector<uint8_t> AccessorBytes(int32_t disp)
{
    std::vector<uint8_t> b(kBuild.pattern.begin(), kBuild.pattern.end());
    std::memcpy(b.data() + 3, &disp, sizeof(disp));
    return b;
}

std::vector<uint8_t> Text(size_t size)
{
    std::vector<uint8_t> t(size);
    for (size_t i = 0; i < size; ++i)
        t[i] = static_cast<uint8_t>(0x90 + (i * 7) % 5); // filler that never starts the pattern (0x48)
    return t;
}

void Put(std::vector<uint8_t>& t, size_t offset, const std::vector<uint8_t>& bytes)
{
    std::memcpy(t.data() + offset, bytes.data(), bytes.size());
}

ScanResult Scan(const std::vector<uint8_t>& t)
{
    return ScanSpan(t.data(), t.size(), kBuild.pattern.data(), kBuild.patternMask.data(), kBuild.pattern.size());
}

ScanResult ScanChunked(const std::vector<uint8_t>& t, size_t chunk, bool& unreadable)
{
    std::vector<uint8_t> scratch(chunk + kBuild.pattern.size());
    return ScanRegion(
        [&](size_t offset, void* out, size_t bytes)
        {
            if (offset + bytes > t.size())
                return false;
            std::memcpy(out, t.data() + offset, bytes);
            return true;
        },
        t.size(), kBuild.pattern.data(), kBuild.patternMask.data(), kBuild.pattern.size(), chunk, scratch.data(), unreadable);
}

void TestPatternScan()
{
    // documented facts about the pattern
    CHECK(kBuild.pattern.size() == 18);
    CHECK(kBuild.patternMask[0] && kBuild.patternMask[2] && !kBuild.patternMask[3] && !kBuild.patternMask[6] &&
          kBuild.patternMask[7] && kBuild.patternMask[17]);

    // 0 hits
    auto t = Text(4096);
    CHECK(Scan(t).hits == 0);

    // 1 hit, any displacement
    Put(t, 1000, AccessorBytes(0x026a38b9));
    auto r = Scan(t);
    CHECK(r.hits == 1 && r.firstOffset == 1000);

    // the displacement bytes are wildcards, the opcode bytes are not
    auto broken = t;
    broken[1000 + 7] ^= 0xff; // 8B -> something else
    CHECK(Scan(broken).hits == 0);
    broken = t;
    broken[1000 + 17] = 0xc2; // ret -> ret imm
    CHECK(Scan(broken).hits == 0);

    // 2 hits: counting stops at two and reports both offsets
    Put(t, 3000, AccessorBytes(-16));
    r = Scan(t);
    CHECK(r.hits == 2 && r.firstOffset == 1000 && r.secondOffset == 3000);

    // a match at the very start and at the very end
    auto edge = Text(200);
    Put(edge, 0, AccessorBytes(1));
    Put(edge, 200 - 18, AccessorBytes(2));
    r = Scan(edge);
    CHECK(r.hits == 2 && r.firstOffset == 0 && r.secondOffset == 182);

    // too small a span
    CHECK(ScanSpan(t.data(), 17, kBuild.pattern.data(), kBuild.patternMask.data(), 18).hits == 0);
}

void TestChunkedScan()
{
    // A match that straddles every chunk boundary is found exactly once; offsets are reported in region coordinates.
    for (size_t chunk : { size_t(64), size_t(100), size_t(4096) })
    {
        for (size_t at = 0; at + 18 <= 1000; at += 13)
        {
            auto t = Text(1000);
            Put(t, at, AccessorBytes(5));
            bool unreadable = true;
            const auto r = ScanChunked(t, chunk, unreadable);
            CHECK(!unreadable);
            CHECK(r.hits == 1);
            CHECK(r.firstOffset == at);
        }
    }

    // two matches, one on each side of a chunk boundary
    auto t = Text(300);
    Put(t, 60, AccessorBytes(1));
    Put(t, 130, AccessorBytes(2));
    bool unreadable = true;
    auto r = ScanChunked(t, 64, unreadable);
    CHECK(r.hits == 2 && r.firstOffset == 60 && r.secondOffset == 130);

    // an unreadable window fails the scan
    r = ScanRegion([](size_t, void*, size_t) { return false; }, 300, kBuild.pattern.data(), kBuild.patternMask.data(), 18, 64,
                   std::vector<uint8_t>(64 + 18).data(), unreadable);
    CHECK(unreadable && r.hits == 0);
}

void TestRipDecoding()
{
    // global = match + 7 + disp32 (the load is 7 bytes long)
    CHECK(RipTargetRva(0x26abd0, kBuild.ripInstructionLength, 0x026a38b9) == 0x290e490);
    CHECK(RipTargetRva(0x1000, 7, -0x10) == 0xff7);
    CHECK(RipTargetRva(0x10, 7, -0x100) < 0);

    // and the documented constants agree with each other
    CHECK(kBuild.accessorRva + 7 + 0x026a38b9 == kBuild.managerPointerRva);
    // bytes of the real function, as read from the executable: the displacement b9 38 6a 02
    auto bytes = AccessorBytes(0x026a38b9);
    CHECK(bytes[3] == 0xb9 && bytes[4] == 0x38 && bytes[5] == 0x6a && bytes[6] == 0x02);
}

// A fake PE header copy with the sections named
std::vector<uint8_t> FakeImage(const char* firstName, uint32_t rva, uint32_t vsize, size_t sections = 2)
{
    std::vector<uint8_t> image(0x1000, 0);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image.data());
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = 0x80;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(image.data() + 0x80);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.NumberOfSections = static_cast<WORD>(sections);
    nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
    nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    auto* sh = reinterpret_cast<IMAGE_SECTION_HEADER*>(image.data() + 0x80 + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) +
                                                      sizeof(IMAGE_OPTIONAL_HEADER64));
    for (size_t i = 0; i < sections; ++i)
    {
        std::memset(sh[i].Name, 0, sizeof(sh[i].Name));
        const char* name = i == 0 ? firstName : ".rdata";
        std::memcpy(sh[i].Name, name, std::strlen(name));
        sh[i].VirtualAddress = rva + static_cast<uint32_t>(i) * 0x100000;
        sh[i].Misc.VirtualSize = vsize;
    }
    return image;
}

void TestPeSection()
{
    Section s;
    auto image = FakeImage(".text", 0x1000, 0x2345);
    CHECK(FindSection(image.data(), image.size(), ".text", s) && s.rva == 0x1000 && s.size == 0x2345);
    CHECK(FindSection(image.data(), image.size(), ".rdata", s) && s.rva == 0x101000);
    CHECK(!FindSection(image.data(), image.size(), ".data", s));

    // validation
    auto bad = image;
    bad[0] = 'X';
    CHECK(!FindSection(bad.data(), bad.size(), ".text", s));
    bad = image;
    reinterpret_cast<IMAGE_NT_HEADERS64*>(bad.data() + 0x80)->Signature = 0;
    CHECK(!FindSection(bad.data(), bad.size(), ".text", s));
    bad = image;
    reinterpret_cast<IMAGE_NT_HEADERS64*>(bad.data() + 0x80)->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
    CHECK(!FindSection(bad.data(), bad.size(), ".text", s));
    bad = image;
    reinterpret_cast<IMAGE_NT_HEADERS64*>(bad.data() + 0x80)->FileHeader.NumberOfSections = 0x61;
    CHECK(!FindSection(bad.data(), bad.size(), ".text", s));
    bad = image;
    reinterpret_cast<IMAGE_NT_HEADERS64*>(bad.data() + 0x80)->FileHeader.NumberOfSections = 0;
    CHECK(!FindSection(bad.data(), bad.size(), ".text", s));
    bad = image;
    reinterpret_cast<IMAGE_DOS_HEADER*>(bad.data())->e_lfanew = 0x7fff'ffff;
    CHECK(!FindSection(bad.data(), bad.size(), ".text", s));
    CHECK(!FindSection(image.data(), 16, ".text", s));
    CHECK(!FindSection(image.data(), 0x90, ".text", s)); // section table beyond the copy
    auto empty = FakeImage(".text", 0x1000, 0);
    CHECK(!FindSection(empty.data(), empty.size(), ".text", s)); // zero-sized .text is no .text
}

// ---- The verification of the accessor on a synthetic executable (the success path and every refusal) ----

constexpr uintptr_t kImageBase = 0x140000000ull;
constexpr uint32_t kTextRva = 0x1000;
constexpr uint32_t kTextSize = 0x3000;

struct Hit
{
    uint32_t rva;        // where the accessor code is
    uint32_t managerRva; // the global it loads
};

// Headers (a first section named `firstName` at 0x1000), the filler code and the accessor patterns at the given places.
std::vector<uint8_t> MakeImage(std::initializer_list<Hit> hits, const char* firstName = ".text")
{
    std::vector<uint8_t> image(0x6000, 0);
    const auto headers = FakeImage(firstName, kTextRva, kTextSize, 1);
    std::memcpy(image.data(), headers.data(), headers.size());
    const auto text = Text(kTextSize);
    std::memcpy(image.data() + kTextRva, text.data(), text.size());
    for (const Hit& h : hits)
    {
        const int32_t disp = static_cast<int32_t>(static_cast<int64_t>(h.managerRva) - (static_cast<int64_t>(h.rva) + 7));
        Put(image, h.rva, AccessorBytes(disp));
    }
    return image;
}

// A reader of the image; `failAbove` makes reads that end above that address fail, `failExact` makes the 4-byte read at one address fail.
struct ImageReader
{
    const std::vector<uint8_t>& image;
    uintptr_t failAbove = ~uintptr_t(0);
    uintptr_t failExact = 0;
    bool everything = true;

    bool operator()(uintptr_t address, void* out, size_t bytes) const
    {
        if (!everything || address < kImageBase || address - kImageBase + bytes > image.size())
            return false;
        if (address - kImageBase + bytes > failAbove - kImageBase)
            return false;
        if (failExact != 0 && address == failExact && bytes == 4)
            return false;
        std::memcpy(out, image.data() + (address - kImageBase), bytes);
        return true;
    }
};

CharaViewBuild BuildAt(uint32_t accessorRva, uint32_t managerRva)
{
    CharaViewBuild cv;
    cv.accessorRva = accessorRva;
    cv.managerPointerRva = managerRva;
    cv.maxTextBytes = 0x10000;
    return cv;
}

bool Contains(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

void TestResolveFromImage()
{
    constexpr uint32_t accessor = kTextRva + 0x850;
    constexpr uint32_t manager = 0x5000;
    const CharaViewBuild cv = BuildAt(accessor, manager);

    // one hit, matching the documented values: Ready
    auto image = MakeImage({ { accessor, manager } });
    Resolution r = ResolveFromImage(ImageReader { image }, kImageBase, cv, 0x400);
    CHECK(r.state == AccessorState::Ready && r.reason.empty());
    CHECK(r.accessorRva == accessor && r.managerPointerRva == manager);
    CHECK(r.managerPointerAddress == kImageBase + manager && r.textBytes == kTextSize);

    // a hit that straddles a scan window (the window is 0x400 bytes): found once
    for (const uint32_t offset : { 0x3f0u, 0x3fbu, 0x400u, 0x7ffu })
    {
        const uint32_t rva = kTextRva + offset;
        image = MakeImage({ { rva, manager } });
        r = ResolveFromImage(ImageReader { image }, kImageBase, BuildAt(rva, manager), 0x400);
        CHECK(r.state == AccessorState::Ready && r.accessorRva == rva);
    }
    // the very end of the section
    {
        const uint32_t rva = kTextRva + kTextSize - static_cast<uint32_t>(kBuild.pattern.size());
        image = MakeImage({ { rva, manager } });
        r = ResolveFromImage(ImageReader { image }, kImageBase, BuildAt(rva, manager), 0x400);
        CHECK(r.state == AccessorState::Ready && r.accessorRva == rva);
    }

    // no hit, two hits: exactly one is needed
    image = MakeImage({});
    r = ResolveFromImage(ImageReader { image }, kImageBase, cv, 0x400);
    CHECK(r.state == AccessorState::Inert && Contains(r.reason, "found 0 times"));
    image = MakeImage({ { accessor, manager }, { kTextRva + 0x1850, manager } });
    r = ResolveFromImage(ImageReader { image }, kImageBase, cv, 0x400);
    CHECK(r.state == AccessorState::Inert && Contains(r.reason, "found 2 times"));

    // a hit that disagrees with the documented values of the matched build: either of the two
    image = MakeImage({ { accessor, manager } });
    r = ResolveFromImage(ImageReader { image }, kImageBase, BuildAt(accessor + 1, manager), 0x400);
    CHECK(r.state == AccessorState::Inert && Contains(r.reason, "documents"));
    // the numbers are RVAs: printed in hex, the way the documentation writes them
    CHECK(r.reason == "the accessor is at RVA 0x1850 and the manager pointer at RVA 0x5000, the matched build documents 0x1851 and 0x5000");
    r = ResolveFromImage(ImageReader { image }, kImageBase, BuildAt(accessor, manager + 8), 0x400);
    CHECK(r.state == AccessorState::Inert && Contains(r.reason, "documents"));
    CHECK(r.reason == "the accessor is at RVA 0x1850 and the manager pointer at RVA 0x5000, the matched build documents 0x1850 and 0x5008");
    CHECK(r.managerPointerAddress == 0); // nothing usable is handed out

    // the load points below the image
    image = MakeImage({ { accessor, 0 } });
    {
        const int32_t disp = -0x100000;
        Put(image, accessor, AccessorBytes(disp));
        r = ResolveFromImage(ImageReader { image }, kImageBase, cv, 0x400);
        CHECK(r.state == AccessorState::Inert && Contains(r.reason, "outside"));
    }

    // headers: unreadable, no .text section, .text above the scan limit
    image = MakeImage({ { accessor, manager } });
    ImageReader nothing { image };
    nothing.everything = false;
    r = ResolveFromImage(nothing, kImageBase, cv, 0x400);
    CHECK(r.state == AccessorState::Inert && Contains(r.reason, "headers"));
    r = ResolveFromImage(ImageReader { image }, 0, cv, 0x400); // no module
    CHECK(r.state == AccessorState::Inert && Contains(r.reason, "headers"));
    auto noText = MakeImage({ { accessor, manager } }, ".rdata");
    r = ResolveFromImage(ImageReader { noText }, kImageBase, cv, 0x400);
    CHECK(r.state == AccessorState::Inert && Contains(r.reason, "no readable .text"));
    CharaViewBuild limited = cv;
    limited.maxTextBytes = kTextSize - 1;
    r = ResolveFromImage(ImageReader { image }, kImageBase, limited, 0x400);
    CHECK(r.state == AccessorState::Inert && Contains(r.reason, "larger than the scan limit"));
    limited.maxTextBytes = kTextSize; // the limit itself is allowed
    r = ResolveFromImage(ImageReader { image }, kImageBase, limited, 0x400);
    CHECK(r.state == AccessorState::Ready);

    // a part of the code that cannot be read
    ImageReader partial { image };
    partial.failAbove = kImageBase + kTextRva + 0x1000;
    r = ResolveFromImage(partial, kImageBase, cv, 0x400);
    CHECK(r.state == AccessorState::Inert && Contains(r.reason, "could not be read for the accessor scan"));

    // the displacement read after the scan fails
    ImageReader noDisp { image };
    noDisp.failExact = kImageBase + accessor + 3;
    r = ResolveFromImage(noDisp, kImageBase, cv, 0x400);
    CHECK(r.state == AccessorState::Inert && Contains(r.reason, "displacement"));
}

// An exception during the verification becomes Inert (it used to leave the once-flag unset, so every draw threw again).
void TestResolveSafely()
{
    bool threw = true;
    const Resolution ok = ResolveSafely(
        []
        {
            Resolution r;
            r.state = AccessorState::Ready;
            r.accessorRva = 5;
            return r;
        },
        &threw);
    CHECK(!threw && ok.state == AccessorState::Ready && ok.accessorRva == 5);

    const Resolution bad = ResolveSafely([]() -> Resolution { throw std::bad_alloc(); }, &threw);
    CHECK(threw && bad.state == AccessorState::Inert && !bad.reason.empty());
    CHECK(ResolveSafely([]() -> Resolution { throw 1; }).state == AccessorState::Inert); // any exception, no out parameter

    // the real chain: a reader that throws
    const auto image = MakeImage({});
    const Resolution fromReader = ResolveSafely(
        [&]
        { return ResolveFromImage([](uintptr_t, void*, size_t) -> bool { throw std::runtime_error("read"); }, kImageBase, kBuild, 0x400); },
        &threw);
    CHECK(threw && fromReader.state == AccessorState::Inert);
    (void)image;
}

// A fake manager and entry objects in a map "address -> bytes"
struct FakeGame
{
    std::vector<uint8_t> manager = std::vector<uint8_t>(0x400, 0);
    std::vector<std::pair<uintptr_t, std::vector<uint8_t>>> objects;
    int reads = 0;

    void SetPointer(size_t entry, uint64_t pointer)
    {
        const uint32_t off = EntryPointerOffset(Layout {}, entry);
        std::memcpy(manager.data() + off, &pointer, 8);
    }

    uintptr_t AddEntry(size_t entry, uint64_t resource, uint32_t w, uint32_t h, uintptr_t address = 0, size_t size = 0x70)
    {
        if (address == 0)
            address = 0x10000 + objects.size() * 0x1000;
        std::vector<uint8_t> o(size, 0);
        if (size >= 0x6c)
        {
            std::memcpy(o.data() + 0x68, &resource, 8);
            std::memcpy(o.data() + 0x40, &w, 4);
            std::memcpy(o.data() + 0x44, &h, 4);
        }
        objects.push_back({ address, o });
        SetPointer(entry, address);
        return address;
    }

    Snapshot Parse()
    {
        return ParseManager(manager.data(), manager.size(), Layout {},
                            [&](uintptr_t address, void* out, size_t bytes)
                            {
                                ++reads;
                                for (const auto& [a, o] : objects)
                                {
                                    if (a == address && o.size() >= bytes)
                                    {
                                        std::memcpy(out, o.data(), bytes);
                                        return true;
                                    }
                                }
                                return false;
                            });
    }
};

void TestEntryOffsets()
{
    const Layout l {};
    // 0..12 at 0x2c8 + 8i, 13..17 at 0x378 + 8(i-13), 18 at 0x360, 19 at 0x368, nothing else
    CHECK(EntryPointerOffset(l, 0) == 0x2c8);
    CHECK(EntryPointerOffset(l, 7) == 0x2c8 + 56);
    CHECK(EntryPointerOffset(l, 8) == 0x308);
    CHECK(EntryPointerOffset(l, 12) == 0x328);
    CHECK(EntryPointerOffset(l, 13) == 0x378);
    CHECK(EntryPointerOffset(l, 15) == 0x388);
    CHECK(EntryPointerOffset(l, 17) == 0x398);
    CHECK(EntryPointerOffset(l, 18) == 0x360);
    CHECK(EntryPointerOffset(l, 19) == 0x368);
    CHECK(EntryPointerOffset(l, 20) == 0);
    // all 20 pointers lie inside the 0x400 bytes that are read and are distinct
    for (size_t i = 0; i < kEntries; ++i)
    {
        CHECK(EntryPointerOffset(l, i) + 8 <= 0x400);
        for (size_t j = i + 1; j < kEntries; ++j)
            CHECK(EntryPointerOffset(l, i) != EntryPointerOffset(l, j));
    }
    CHECK(kBuild.targetsOffset == l.targetsOffset && kBuild.transparentOffset == l.transparentOffset &&
          kBuild.depthOffset0 == l.depthOffset0 && kBuild.depthOffset1 == l.depthOffset1 && kBuild.managerBytes == l.managerBytes &&
          kBuild.entryBytes == l.entryBytes && kBuild.entryResourceOffset == l.resourceOffset &&
          kBuild.entryWidthOffset == l.widthOffset && kBuild.entryHeightOffset == l.heightOffset &&
          kBuild.maxExtent == l.maxExtent);
}

void TestManagerParse()
{
    FakeGame g;
    // the layout of the real renderer: 576 x 960 everywhere
    for (size_t i = 0; i < kEntries; ++i)
        g.AddEntry(i, 0xA000 + i * 0x100, 576, 960);
    auto s = g.Parse();
    CHECK(s.ready && s.ValidCount() == kEntries && s.AnyValid());
    for (size_t i = 0; i < kEntries; ++i)
        CHECK(s.entries[i].resource == 0xA000 + i * 0x100 && s.entries[i].width == 576 && s.entries[i].height == 960);

    // lookup: pointer AND both sizes
    CHECK(Lookup(s, 0xA000, 576, 960) == 0);
    CHECK(Lookup(s, 0xA000 + 8 * 0x100, 576, 960) == 8);
    CHECK(Lookup(s, 0xA000 + 19 * 0x100, 576, 960) == 19);
    CHECK(Lookup(s, 0xA000, 575, 960) == -1);
    CHECK(Lookup(s, 0xA000, 576, 961) == -1);
    CHECK(Lookup(s, 0xC000, 576, 960) == -1);
    CHECK(Lookup(s, 0, 576, 960) == -1);
    CHECK(IndexOfPointer(s, 0xA000 + 5 * 0x100) == 5);
    CHECK(IndexOfPointer(s, 0xA000 + 5 * 0x100 + 8) == -1);
    CHECK(IndexOfPointer(s, 0) == -1);

    // the first of two identical entries wins
    FakeGame dup;
    dup.AddEntry(3, 0xA000, 64, 64);
    dup.AddEntry(9, 0xA000, 64, 64);
    CHECK(Lookup(dup.Parse(), 0xA000, 64, 64) == 3);
}

void TestEntryRules()
{
    // an all-null table: ready, nothing valid, nothing matches ("inert")
    FakeGame empty;
    auto s = empty.Parse();
    CHECK(s.ready && !s.AnyValid() && s.ValidCount() == 0);
    CHECK(Lookup(s, 0x1234, 576, 960) == -1);
    CHECK(empty.reads == 0); // null entry pointers are not followed

    // zeroing rules
    FakeGame g;
    g.AddEntry(0, 0xA000, 0, 960);      // width 0
    g.AddEntry(1, 0xA100, 576, 0);      // height 0
    g.AddEntry(2, 0xA200, 0x4001, 960); // width above 0x4000
    g.AddEntry(3, 0xA300, 576, 0x4001); // height above 0x4000
    g.AddEntry(4, 0, 576, 960);         // null resource
    g.AddEntry(5, 0xA500, 0x4000, 0x4000); // the limit itself is accepted
    g.AddEntry(6, 0xA600, 1, 1);
    g.SetPointer(7, 0xdead0000);        // points at memory that cannot be read
    s = g.Parse();
    for (size_t i : { size_t(0), size_t(1), size_t(2), size_t(3), size_t(4), size_t(7) })
    {
        CHECK(s.entries[i].resource == 0 && s.entries[i].width == 0 && s.entries[i].height == 0);
        CHECK(!s.entries[i].Valid());
    }
    CHECK(s.entries[5].Valid() && s.entries[5].width == 0x4000 && s.entries[5].height == 0x4000);
    CHECK(s.entries[6].Valid());
    CHECK(s.ValidCount() == 2);
    CHECK(Lookup(s, 0xA000, 0, 960) == -1); // a zeroed entry never matches its old values

    // the entry object is read as 0x70 bytes: a reader that has fewer bytes fails the entry
    FakeGame tiny;
    tiny.AddEntry(0, 0xA000, 576, 960, 0x20000, 0x40);
    CHECK(!tiny.Parse().entries[0].Valid());

    // a manager copy that is too short for an entry pointer leaves that entry empty
    FakeGame shortCopy;
    for (size_t i = 0; i < kEntries; ++i)
        shortCopy.AddEntry(i, 0xA000 + i * 0x100, 576, 960);
    auto partial = ParseManager(shortCopy.manager.data(), 0x2d0, Layout {},
                                [&](uintptr_t address, void* out, size_t bytes)
                                {
                                    for (const auto& [a, o] : shortCopy.objects)
                                    {
                                        if (a == address)
                                        {
                                            std::memcpy(out, o.data(), bytes);
                                            return true;
                                        }
                                    }
                                    return false;
                                });
    CHECK(partial.entries[0].Valid() && !partial.entries[1].Valid() && !partial.entries[19].Valid());
}

void TestCache()
{
    Cache cache;
    int loads = 0;
    auto load = [&]
    {
        ++loads;
        Snapshot s;
        s.ready = true;
        s.entries[0] = { 0x1000u + static_cast<unsigned>(loads), 8, 8 };
        return s;
    };

    CHECK(cache.Get(1000, load).entries[0].resource == 0x1001 && loads == 1);
    CHECK(cache.Get(1050, load).entries[0].resource == 0x1001 && loads == 1);  // younger than 100 ms
    CHECK(cache.Get(1099, load).entries[0].resource == 0x1001 && loads == 1);
    CHECK(cache.Get(1100, load).entries[0].resource == 0x1002 && loads == 2);  // exactly 100 ms: refreshed
    CHECK(cache.Get(1150, load).entries[0].resource == 0x1002 && loads == 2);  // age counts from the refresh
    CHECK(cache.Get(500, load).entries[0].resource == 0x1003 && loads == 3);   // the clock went backwards: refreshed
    cache.Reset();
    cache.Get(500, load);
    CHECK(loads == 4);
    CHECK(kRefreshMs == 100);
}

void TestRoles()
{
    CHECK(std::strcmp(EntryRole(0), "output") == 0 && std::strcmp(EntryRole(7), "output") == 0);
    CHECK(std::strcmp(EntryRole(8), "GBuffer") == 0 && std::strcmp(EntryRole(12), "GBuffer") == 0);
    CHECK(std::strcmp(EntryRole(13), "transparent GBuffer") == 0 && std::strcmp(EntryRole(17), "transparent GBuffer") == 0);
    CHECK(std::strcmp(EntryRole(18), "depth candidate") == 0 && std::strcmp(EntryRole(19), "depth candidate") == 0);
}
} // namespace

int main()
{
    TestPatternScan();
    TestChunkedScan();
    TestRipDecoding();
    TestPeSection();
    TestResolveFromImage();
    TestResolveSafely();
    TestEntryOffsets();
    TestManagerParse();
    TestEntryRules();
    TestCache();
    TestRoles();
    return TEST_RESULT("chara_table");
}
