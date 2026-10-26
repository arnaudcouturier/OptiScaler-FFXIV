// The image side of the resolver: PE header parsing, DenseImage (zero-filled, section-mapped, readable only inside headers+sections),
// section classification (code / read-only data / initialized data / bss), the live process image, and the enclosing-function lookup
// through .pdata (leaf functions without an entry, chained unwind entries, malformed chains).
#include "resolve_test_util.h"

#include "ext/ffxiv/resolve/LiveImage.h"
#include "ext/ffxiv/resolve/Unwind.h"

using namespace rt;

namespace
{
void TestParsePe()
{
    auto img = MakeImage(0xabcdef01);
    REQUIRE(img != nullptr && img->Valid());
    CHECK(img->TimeDateStamp() == 0xabcdef01);
    CHECK(img->SizeOfImage() == kImageSize);
    REQUIRE(img->Sections().size() == 4);
    CHECK(img->Sections()[0].NameIs(".text") && img->Sections()[0].Executable() && !img->Sections()[0].Writable());
    CHECK(img->Sections()[2].NameIs(".data") && img->Sections()[2].Writable() && img->Sections()[2].InitializedEnd() == kDataRva + kDataRaw);
    CHECK(img->Info().exceptionRva == kPdataRva);

    // validation: take the header bytes of a good image and break one field at a time
    const std::vector<std::uint8_t> good(img->Bytes().begin(), img->Bytes().begin() + 0x400);
    PeInfo info;
    CHECK(ParsePe(good.data(), good.size(), info) && info.valid);
    auto bad = good;
    bad[0] = 'X';
    CHECK(!ParsePe(bad.data(), bad.size(), info) && !info.valid && !info.error.empty());
    bad = good;
    bad[0x80] = 0; // PE signature
    CHECK(!ParsePe(bad.data(), bad.size(), info));
    bad = good;
    bad[0x84] = 0x4c; // machine i386
    bad[0x85] = 0x01;
    CHECK(!ParsePe(bad.data(), bad.size(), info));
    bad = good;
    bad[0x98] = 0x0b; // PE32 magic 0x10b
    bad[0x99] = 0x01;
    CHECK(!ParsePe(bad.data(), bad.size(), info));
    bad = good;
    bad[0x86] = 0; // zero sections
    CHECK(!ParsePe(bad.data(), bad.size(), info));
    bad = good;
    bad[0x86] = 97; // too many
    CHECK(!ParsePe(bad.data(), bad.size(), info));
    bad = good;
    bad[0x3c] = 0xff; // e_lfanew far away
    bad[0x3d] = 0xff;
    CHECK(!ParsePe(bad.data(), bad.size(), info));
    CHECK(!ParsePe(good.data(), 0x100, info)); // the section table is outside the copy
    CHECK(!ParsePe(good.data(), 8, info));
    bad = good;
    bad[0x98 + 56 + 3] = 0x7f; // SizeOfImage far above 1 GiB
    CHECK(!ParsePe(bad.data(), bad.size(), info));
    bad = good;
    // a section that lies outside SizeOfImage: .pdata VirtualAddress -> 0xFF000
    const std::size_t pdata = 0x98 + 0xf0 + 3 * 40;
    bad[pdata + 12 + 1] = 0xf0;
    bad[pdata + 12 + 2] = 0x0f;
    CHECK(!ParsePe(bad.data(), bad.size(), info));
    // overlapping sections: .rdata VirtualAddress -> .text start
    bad = good;
    const std::size_t rdata = 0x98 + 0xf0 + 1 * 40;
    bad[rdata + 12] = 0x00;
    bad[rdata + 12 + 1] = 0x20;
    CHECK(!ParsePe(bad.data(), bad.size(), info));
}

void TestDenseImage()
{
    auto img = MakeImage();
    REQUIRE(img != nullptr);
    std::uint8_t b[16] = {};

    // headers are readable, the gap between the header page and .text is not
    CHECK(img->Read(0, b, 2) && b[0] == 'M' && b[1] == 'Z');
    CHECK(img->Read(0x3ff, b, 1));
    CHECK(img->Read(0xfff, b, 2));             // headers occupy the first page and touch .text: one mapped range
    CHECK(!img->Read(kImageSize - 1, b, 2));   // past the image
    CHECK(!img->Read(kImageSize, b, 1));
    CHECK(!img->Read(0xffffffff, b, 1));
    CHECK(!img->Read(0, b, 0));
    CHECK(!img->Read(kTextRva + kTextSize - 4, b, 16)); // runs into the unmapped page after .text

    // a read must lie inside one mapped range: .text and .rdata are separated by one unmapped page
    CHECK(img->Read(kTextRva + kTextSize - 16, b, 16));
    CHECK(!img->Read(kTextRva + kTextSize - 8, b, 16)); // crosses into the gap 0x5000..0x6000
    CHECK(img->Read(kRdataRva, b, 16));
    CHECK(!img->Read(kRdataRva - 1, b, 2));

    // contiguous sections read as one range (.rdata 0x6000..0x8000 then .data 0x8000..)
    CHECK(img->Read(kRdataRva + kRdataSize - 8, b, 16));
    CHECK(img->Read(kDataRva + kDataSize - 16, b, 16));
    CHECK(img->Read(kDataRva + kDataSize - 8, b, 16)); // .data continues into .pdata: one mapped range

    // bss reads as zero, the file part as written
    auto data = MakeImage();
    Plant(*data, kDataRva + 0x10, "01020304");
    std::uint32_t v = 7;
    CHECK(data->Read(kDataRva + 0x10, &v, 4) && v == 0x04030201);
    CHECK(data->Read(kBssRva + 0x100, &v, 4) && v == 0);
    CHECK(!data->Put(kImageSize - 2, b, 4)); // outside the image
}

void TestFileMapping()
{
    // Build a "file" with raw data at file offsets and map it: headers, then min(rawSize, virtualSize) bytes per section, zero elsewhere.
    auto src = DenseImage::Synthetic(0x1234, 0x6000,
                                     { { ".text", 0x1000, 0x1800, 0x1000, DenseImage::kText },
                                       { ".data", 0x3000, 0x2000, 0x400, DenseImage::kData } });
    REQUIRE(src != nullptr);
    std::vector<std::uint8_t> file(0x400 + 0x1000 + 0x400, 0);
    std::memcpy(file.data(), src->Bytes().data(), 0x400); // headers (the section table carries PointerToRawData 0x400 and 0x1400)
    for (std::size_t i = 0; i < 0x1000; ++i)
        file[0x400 + i] = static_cast<std::uint8_t>(0xa0 + (i & 7));
    for (std::size_t i = 0; i < 0x400; ++i)
        file[0x1400 + i] = static_cast<std::uint8_t>(0x30 + (i & 7));

    std::string err;
    auto img = DenseImage::FromFileBytes(file, &err);
    REQUIRE(img != nullptr);
    std::uint8_t b[4] = {};
    CHECK(img->Read(0x1000, b, 4) && b[0] == 0xa0 && b[3] == 0xa3);
    CHECK(img->Read(0x1000 + 0x1000 - 4, b, 4) && b[0] == 0xa4);      // last raw byte
    CHECK(img->Read(0x1000 + 0x1000, b, 4) && b[0] == 0 && b[3] == 0); // virtual size above the raw size is zero
    CHECK(img->Read(0x3000, b, 4) && b[0] == 0x30);
    CHECK(img->Read(0x3000 + 0x400, b, 4) && b[0] == 0); // bss
    CHECK(img->Sections()[1].InitializedEnd() == 0x3000 + 0x400);

    // a section whose raw data lies outside the file is refused
    std::vector<std::uint8_t> shortFile(file.begin(), file.begin() + 0x800);
    CHECK(DenseImage::FromFileBytes(shortFile, &err) == nullptr && !err.empty());
    CHECK(DenseImage::FromFileBytes(std::vector<std::uint8_t>(64, 0), &err) == nullptr);
    CHECK(DenseImage::FromFile("C:/this/does/not/exist.exe", &err) == nullptr && !err.empty());
}

void TestRegions()
{
    auto img = MakeImage();
    REQUIRE(img != nullptr);
    const PeInfo& info = img->Info();
    CHECK(InRegion(info, RegionKind::Code, kTextRva + 5) && !InRegion(info, RegionKind::Code, kRdataRva));
    CHECK(InRegion(info, RegionKind::ReadOnlyData, kRdataRva + 5) && !InRegion(info, RegionKind::ReadOnlyData, kTextRva));
    CHECK(!InRegion(info, RegionKind::ReadOnlyData, kPdataRva)); // .pdata is not searched data
    CHECK(InRegion(info, RegionKind::InitData, kDataRva + 5));
    CHECK(!InRegion(info, RegionKind::InitData, kBssRva));       // the first bss byte
    CHECK(InRegion(info, RegionKind::InitData, kBssRva - 1));    // the last initialized byte
    CHECK(InRegion(info, RegionKind::Bss, kBssRva) && InRegion(info, RegionKind::Bss, kDataRva + kDataSize - 1));
    CHECK(!InRegion(info, RegionKind::Bss, kBssRva - 1));
    CHECK(!InRegion(info, RegionKind::Bss, kDataRva + kDataSize)); // beyond the section
    CHECK(!InRegion(info, RegionKind::Bss, kTextRva + 4));
    CHECK(InRegion(info, RegionKind::Image, kPdataRva) && !InRegion(info, RegionKind::Image, 0x500));
    CHECK(!InRegion(info, RegionKind::Image, kImageSize));

    // a section flagged uninitialized is bss from its first byte
    auto bssOnly = DenseImage::Synthetic(1, 0x4000, { { ".text", 0x1000, 0x1000, 0x1000, DenseImage::kText },
                                                      { ".bss", 0x2000, 0x1000, 0, kScnUninitData | kScnRead | kScnWrite } });
    REQUIRE(bssOnly != nullptr);
    CHECK(InRegion(bssOnly->Info(), RegionKind::Bss, 0x2000) && !InRegion(bssOnly->Info(), RegionKind::InitData, 0x2000));
}

void TestLiveImage()
{
    // the test executable's own image: read through the same interface the game uses
    auto live = LiveImage::OpenMainModule();
    REQUIRE(live != nullptr && live->Valid());
    CHECK(live->SizeOfImage() > 0x1000);
    bool hasText = false;
    for (const ImageSection& s : live->Sections())
        hasText = hasText || (s.NameIs(".text") && s.Executable());
    CHECK(hasText);
    std::uint8_t b[2] = {};
    CHECK(live->Read(0, b, 2) && b[0] == 'M' && b[1] == 'Z');
    CHECK(!live->Read(live->SizeOfImage(), b, 1));
    CHECK(!live->Read(live->SizeOfImage() - 1, b, 2));
    CHECK(!live->Read(0xffffff00, b, 2));

    // the live image maps an RVA to the process address: the bytes of a function of this very executable read back identically, and a
    // scan of the code sections finds that function at the same RVA
    const auto here = reinterpret_cast<std::uintptr_t>(&TestRegions);
    const auto base = reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr));
    REQUIRE(here > base);
    const std::uint32_t rva = static_cast<std::uint32_t>(here - base);
    std::uint8_t expected[16], got[16] = {};
    std::memcpy(expected, reinterpret_cast<const void*>(here), sizeof(expected));
    CHECK(live->Read(rva, got, sizeof(got)) && std::memcmp(expected, got, sizeof(got)) == 0);
    Pattern p;
    p.bytes.assign(expected, expected + sizeof(expected));
    p.mask.assign(sizeof(expected), 0xff);
    const auto out = ScanBatch(*live, { { &p, SearchIn::Code } });
    CHECK(!out.unreadable);
    bool found = false;
    for (std::uint32_t h : out.perRequest[0].rvas)
        found = found || h == rva;
    CHECK(found);
}

void TestUnwind()
{
    // RUNTIME_FUNCTION table (sorted), unwind blocks in .rdata
    //   0x1100..0x1180  plain function A (flags 0)
    //   0x1180..0x11a0  chained to A
    //   0x11a0..0x11c0  chained to the chained one (two hops)
    //   0x1200..0x1240  plain function B
    //   0x1300..0x1310  chained to itself (malformed: loop)
    auto img = MakeImage(1, 5 * 12);
    REQUIRE(img != nullptr);
    const std::uint32_t uA = kRdataRva + 0x00, uB = kRdataRva + 0x40, uC = kRdataRva + 0x80, uD = kRdataRva + 0xc0, uE = kRdataRva + 0x100;
    PutUnwind(*img, uA, 0, 2);
    PutUnwind(*img, uB, 4, 3, 0x1100, 0x1180, uA);             // chain -> A (3 codes: padded to 4)
    PutUnwind(*img, uC, 4, 0, 0x1180, 0x11a0, uB);             // chain -> the chained entry
    PutUnwind(*img, uD, 1, 1);                                 // exception handler flag, not a chain
    PutUnwind(*img, uE, 4, 0, 0x1300, 0x1310, uE);             // loops on itself
    PutRuntimeFunction(*img, 0, 0x1100, 0x1180, uA);
    PutRuntimeFunction(*img, 1, 0x1180, 0x11a0, uB);
    PutRuntimeFunction(*img, 2, 0x11a0, 0x11c0, uC);
    PutRuntimeFunction(*img, 3, 0x1200, 0x1240, uD);
    PutRuntimeFunction(*img, 4, 0x1300, 0x1310, uE);

    auto at = [&](std::uint32_t rva) { return FunctionStart(*img, rva); };
    CHECK(at(0x1100).status == FunctionStartStatus::Found && at(0x1100).start == 0x1100);
    CHECK(at(0x117f).status == FunctionStartStatus::Found && at(0x117f).start == 0x1100);
    CHECK(at(0x1180).status == FunctionStartStatus::Found && at(0x1180).start == 0x1100); // chained: the parent's start
    CHECK(at(0x1195).start == 0x1100);
    CHECK(at(0x11b0).status == FunctionStartStatus::Found && at(0x11b0).start == 0x1100); // two hops
    CHECK(at(0x1200).start == 0x1200 && at(0x123f).start == 0x1200);

    // no entry: before the first, in a gap, at an end address, after the last (a leaf function has no entry)
    CHECK(at(0x10ff).status == FunctionStartStatus::NoUnwindEntry);
    CHECK(at(0x11c0).status == FunctionStartStatus::NoUnwindEntry);
    CHECK(at(0x1240).status == FunctionStartStatus::NoUnwindEntry);
    CHECK(at(0x12ff).status == FunctionStartStatus::NoUnwindEntry);
    CHECK(at(0x1310).status == FunctionStartStatus::NoUnwindEntry);
    CHECK(at(0x4000).status == FunctionStartStatus::NoUnwindEntry);
    CHECK(at(0xffffffff).status != FunctionStartStatus::Found);

    // a chain that loops is refused, never followed forever
    CHECK(at(0x1305).status == FunctionStartStatus::Unreadable);

    // no exception directory at all
    auto none = MakeImage(1, 0);
    CHECK(FunctionStart(*none, 0x1100).status == FunctionStartStatus::NoUnwindEntry);

    // an unwind block that cannot be read (outside the image) is Unreadable, not "no entry"
    auto broken = MakeImage(1, 12);
    PutRuntimeFunction(*broken, 0, 0x1100, 0x1180, 0x900000);
    CHECK(FunctionStart(*broken, 0x1100).status == FunctionStartStatus::Unreadable);
}

void TestUnwindManyEntries()
{
    // a binary search over many entries lands on the right one for every address
    constexpr std::uint32_t kN = 150;
    auto img = MakeImage(1, kN * 12);
    REQUIRE(img != nullptr);
    PutUnwind(*img, kRdataRva, 0, 0);
    for (std::uint32_t i = 0; i < kN; ++i)
        PutRuntimeFunction(*img, i, 0x1000 + i * 0x20, 0x1000 + i * 0x20 + 0x18, kRdataRva);
    for (std::uint32_t i = 0; i < kN; ++i)
    {
        const auto first = FunctionStart(*img, 0x1000 + i * 0x20);
        CHECK(first.status == FunctionStartStatus::Found && first.start == 0x1000 + i * 0x20);
        const auto last = FunctionStart(*img, 0x1000 + i * 0x20 + 0x17);
        CHECK(last.status == FunctionStartStatus::Found && last.start == 0x1000 + i * 0x20);
        CHECK(FunctionStart(*img, 0x1000 + i * 0x20 + 0x18).status == FunctionStartStatus::NoUnwindEntry);
    }
}

// L8: an exception directory that is not inside the image is unusable: no address arithmetic on it, the status says so
void TestExceptionDirectoryOutsideImage()
{
    const std::vector<DenseImage::SectionDef> sections = { { ".text", kTextRva, kTextSize, kTextSize, DenseImage::kText }, { ".pdata", kPdataRva, kPdataSize, kPdataSize, DenseImage::kRdata } };
    // the directory starts inside the image but runs past its end
    auto past = DenseImage::Synthetic(1, kImageSize, sections, kImageSize - 0x10, 0x100);
    REQUIRE(past != nullptr);
    CHECK(FunctionStart(*past, kTextRva + 0x40).status == FunctionStartStatus::Unreadable);
    // a 32 bit wrap: rva + size overflows a uint32
    auto wrap = DenseImage::Synthetic(1, kImageSize, sections, 0xfffffff0u, 0x100);
    REQUIRE(wrap != nullptr);
    CHECK(FunctionStart(*wrap, kTextRva + 0x40).status == FunctionStartStatus::Unreadable);
    // a directory that lies inside the image (empty here) is not an error: a leaf function has no entry
    auto inside = DenseImage::Synthetic(1, kImageSize, sections, kPdataRva, 12);
    REQUIRE(inside != nullptr);
    CHECK(FunctionStart(*inside, kTextRva + 0x40).status == FunctionStartStatus::NoUnwindEntry);
}
} // namespace

int main()
{
    TestExceptionDirectoryOutsideImage();
    TestParsePe();
    TestDenseImage();
    TestFileMapping();
    TestRegions();
    TestLiveImage();
    TestUnwind();
    TestUnwindManyEntries();
    return TEST_RESULT("resolve_image_unwind");
}
