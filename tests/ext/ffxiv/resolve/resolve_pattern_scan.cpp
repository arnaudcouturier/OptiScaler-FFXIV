// Masked patterns and the chunked section scan (ext/ffxiv/resolve/Pattern.h, Scan.h): parsing, nibble masks, exact-one-hit
// bookkeeping, chunk straddling, section bounds (no read leaves a section), executable-only search, the hit cap.
#include "resolve_test_util.h"

#include <algorithm>

using namespace rt;

namespace
{
Pattern P(const char* text)
{
    Pattern p;
    std::string err;
    CHECK(ParsePattern(text, p, &err));
    return p;
}

ScanHits Scan1(const ImageView& img, const Pattern& p, SearchIn where = SearchIn::Code, std::size_t cap = kMaxScanHits,
               std::size_t chunk = 1u << 20)
{
    const auto out = ScanBatch(img, { { &p, where } }, cap, chunk);
    CHECK(out.perRequest.size() == 1);
    return out.perRequest.empty() ? ScanHits {} : out.perRequest[0];
}

void TestParse()
{
    Pattern p;
    std::string err;
    REQUIRE(ParsePattern("48 8B 05 ?? ?? ?? ?? 8B CA", p, &err));
    REQUIRE(p.size() == 9 && p.mask.size() == 9);
    CHECK(p.bytes[0] == 0x48 && p.bytes[1] == 0x8b && p.bytes[8] == 0xca);
    CHECK(p.mask[0] == 0xff && p.mask[3] == 0x00 && p.mask[6] == 0x00 && p.mask[7] == 0xff);
    CHECK(p.bytes[3] == 0x00); // wildcard bytes are stored as zero
    CHECK(p.fixedBytes() == 5);

    // case, extra white space, a tab
    CHECK(ParsePattern("  e8\t??  ?? ?? ??  ", p));
    CHECK(p.size() == 5 && p.bytes[0] == 0xe8 && p.fixedBytes() == 1);

    // nibble wildcards
    REQUIRE(ParsePattern("4? ?5 AB", p));
    REQUIRE(p.size() == 3);
    CHECK(p.mask[0] == 0xf0 && p.bytes[0] == 0x40 && p.mask[1] == 0x0f && p.bytes[1] == 0x05 && p.mask[2] == 0xff);
    CHECK(p.fixedBytes() == 1); // only fully fixed bytes count

    // refusals
    CHECK(!ParsePattern("", p));
    CHECK(!ParsePattern("   ", p));
    CHECK(!ParsePattern("48 8", p));
    CHECK(!ParsePattern("48 8B0", p));
    CHECK(!ParsePattern("4G", p));
    CHECK(!ParsePattern("48 ?", p));
    CHECK(!ParsePattern("?? ?? ??", p, &err) && !err.empty()); // nothing fixed: would match everywhere
    CHECK(ParsePattern("?5 ?? ??", p));                         // a nibble is something fixed
    std::string big;
    for (int i = 0; i < 513; ++i)
        big += "90 ";
    CHECK(!ParsePattern(big, p));
}

void TestMatchAt()
{
    const std::uint8_t data[] = { 0x48, 0x8b, 0x05, 0x11, 0x22, 0x33, 0x44, 0x8b, 0xca };
    REQUIRE(P("48 8B").size() == 2);
    CHECK(MatchAt(data, P("48 8B 05 ?? ?? ?? ?? 8B CA")));
    CHECK(MatchAt(data, P("48 8B 05 11 22 33 44 8B CA")));
    CHECK(!MatchAt(data, P("48 8B 05 11 22 33 45 8B CA")));
    CHECK(!MatchAt(data, P("48 8B 05 ?? ?? ?? ?? 8B CB")));
    CHECK(MatchAt(data, P("4? 8? ?5")));
    CHECK(!MatchAt(data, P("4? 9? ?5")));
    CHECK(!MatchAt(data, P("4? 8? ?6")));
    // mask is applied to both sides: a pattern byte under a zero mask never matters
    Pattern q = P("48 8B 05");
    REQUIRE(q.size() == 3);
    q.bytes[0] = 0x99;
    q.mask[0] = 0x00;
    CHECK(MatchAt(data, q));
}

void TestScanBasics()
{
    const Pattern p = P("48 8B 05 ?? ?? ?? ?? 8B CA 48 8B 84 C8 C8 02 00 00 C3");
    auto img = MakeImage();
    CHECK(img != nullptr);

    CHECK(Scan1(*img, p).rvas.empty());

    Plant(*img, 0x2345, "488b05b9386a028bca488b84c8c8020000c3");
    auto h = Scan1(*img, p);
    CHECK(h.rvas.size() == 1 && h.rvas[0] == 0x2345 && !h.overflow);

    // a different displacement still matches (wildcards), a different opcode byte does not
    Plant(*img, 0x3000, "488b05ffffffff8bca488b84c8c8020000c3");
    h = Scan1(*img, p);
    CHECK(h.rvas.size() == 2 && h.rvas[0] == 0x2345 && h.rvas[1] == 0x3000);
    Plant(*img, 0x3500, "488b05ffffffff8bca488b84c8c8020000c2");
    CHECK(Scan1(*img, p).rvas.size() == 2);

    // first byte and last byte of the section
    Plant(*img, kTextRva, "488b05010000008bca488b84c8c8020000c3");
    Plant(*img, kTextRva + kTextSize - 18, "488b05020000008bca488b84c8c8020000c3");
    h = Scan1(*img, p);
    CHECK(h.rvas.size() == 4 && h.rvas.front() == kTextRva && h.rvas.back() == kTextRva + kTextSize - 18);
    CHECK(std::is_sorted(h.rvas.begin(), h.rvas.end()));
}

void TestChunking()
{
    const Pattern p = P("48 8B 05 ?? ?? ?? ?? 8B CA 48 8B 84 C8 C8 02 00 00 C3");
    // a match at every offset, with chunk sizes that put the boundary through every byte of the pattern
    for (std::size_t chunk : { std::size_t(64), std::size_t(100), std::size_t(4096), std::size_t(1) << 20 })
    {
        for (std::uint32_t at = 0; at + 18 <= 600; at += 7)
        {
            auto img = MakeImage();
            Plant(*img, kTextRva + at, "488b05aabbccdd8bca488b84c8c8020000c3");
            const auto h = Scan1(*img, p, SearchIn::Code, kMaxScanHits, chunk);
            CHECK(h.rvas.size() == 1);
            CHECK(!h.rvas.empty() && h.rvas[0] == kTextRva + at);
        }
    }
    // two matches that sit on both sides of a boundary
    auto img = MakeImage();
    Plant(*img, kTextRva + 60, "488b05010000008bca488b84c8c8020000c3");
    Plant(*img, kTextRva + 130, "488b05020000008bca488b84c8c8020000c3");
    const auto h = Scan1(*img, p, SearchIn::Code, kMaxScanHits, 64);
    CHECK(h.rvas.size() == 2 && h.rvas[0] == kTextRva + 60 && h.rvas[1] == kTextRva + 130);
}

void TestSectionBounds()
{
    const Pattern p = P("48 8B 05 ?? ?? ?? ?? 8B CA 48 8B 84 C8 C8 02 00 00 C3");
    auto img = MakeImage();
    // The first 9 bytes of the pattern are the last 9 bytes of .text; the rest would lie in the unmapped gap after it. Not a match, and
    // the scan must not have tried to read there.
    Plant(*img, kTextRva + kTextSize - 9, "488b05aabbccdd8bca");
    std::uint32_t maxEnd = 0, minStart = 0xffffffff;
    img->readObserver = [&](std::uint32_t rva, std::size_t n)
    {
        minStart = std::min(minStart, rva);
        maxEnd = std::max<std::uint32_t>(maxEnd, rva + static_cast<std::uint32_t>(n));
    };
    const auto out = ScanBatch(*img, { { &p, SearchIn::Code } }, kMaxScanHits, 256);
    CHECK(!out.unreadable);
    CHECK(out.perRequest[0].rvas.empty());
    CHECK(minStart >= kTextRva);
    CHECK(maxEnd <= kTextRva + kTextSize); // not one byte past VirtualSize

    // The same on a section whose virtual size is not page aligned: the page tail belongs to the mapping but not to the scan.
    auto img2 = DenseImage::Synthetic(1, 0x4000, { { ".text", 0x1000, 0x1234, 0x1400, DenseImage::kText } });
    CHECK(img2 != nullptr);
    for (std::uint32_t i = 0x1000; i < 0x2000; ++i)
        img2->Bytes()[i] = 0x90;
    Plant(*img2, 0x1000 + 0x1234 - 9, "488b05aabbccdd8bca");
    Plant(*img2, 0x1000 + 0x1234, "488b84c8c8020000c3"); // the pattern tail sits in the page tail, outside VirtualSize
    maxEnd = 0;
    img2->readObserver = [&](std::uint32_t rva, std::size_t n) { maxEnd = std::max<std::uint32_t>(maxEnd, rva + static_cast<std::uint32_t>(n)); };
    const auto out2 = ScanBatch(*img2, { { &p, SearchIn::Code } }, kMaxScanHits, 4096);
    CHECK(out2.perRequest[0].rvas.empty());
    CHECK(maxEnd <= 0x1000 + 0x1234);
}

void TestSearchClasses()
{
    const Pattern p = P("DE AD BE EF 01 02 03");
    auto img = MakeImage();
    Plant(*img, kRdataRva + 0x100, "deadbeef010203");
    Plant(*img, kDataRva + 0x80, "deadbeef010203");
    Plant(*img, kBssRva + 0x40, "deadbeef010203"); // in the bss tail of .data
    Plant(*img, kTextRva + 0x200, "deadbeef010203");
    Plant(*img, kPdataRva + 0x10, "deadbeef010203"); // .pdata is never searched

    auto h = Scan1(*img, p, SearchIn::Code);
    CHECK(h.rvas.size() == 1 && h.rvas[0] == kTextRva + 0x200);
    h = Scan1(*img, p, SearchIn::ReadOnlyData);
    CHECK(h.rvas.size() == 1 && h.rvas[0] == kRdataRva + 0x100);
    h = Scan1(*img, p, SearchIn::InitData);
    CHECK(h.rvas.size() == 1 && h.rvas[0] == kDataRva + 0x80); // the bss tail is not part of the initialized data

    // all three requests in one pass
    const auto out = ScanBatch(*img, { { &p, SearchIn::Code }, { &p, SearchIn::ReadOnlyData }, { &p, SearchIn::InitData } });
    CHECK(out.perRequest.size() == 3);
    CHECK(out.perRequest[0].rvas.size() == 1 && out.perRequest[1].rvas.size() == 1 && out.perRequest[2].rvas.size() == 1);
}

void TestCapAndFailures()
{
    const Pattern p = P("E8 ?? ?? ?? ?? 90");
    auto img = MakeImage();
    for (std::uint32_t i = 0; i < 20; ++i)
        Plant(*img, kTextRva + 0x40 + i * 0x20, "e8000000009090");
    auto h = Scan1(*img, p, SearchIn::Code, 5);
    CHECK(h.rvas.size() == 5 && h.overflow);
    h = Scan1(*img, p, SearchIn::Code, 20);
    CHECK(h.rvas.size() == 20 && !h.overflow); // exactly at the cap is not an overflow
    h = Scan1(*img, p, SearchIn::Code, 19);
    CHECK(h.rvas.size() == 19 && h.overflow);

    // an unreadable image is reported, never turned into "no hit"
    struct Failing : ImageView
    {
        const DenseImage& inner;
        explicit Failing(const DenseImage& i) : inner(i) { _info = i.Info(); }
        bool Read(std::uint32_t rva, void* out, std::size_t n) const override { return rva < 0x3000 && inner.Read(rva, out, n); }
    } failing(*img);
    const auto out = ScanBatch(failing, { { &p, SearchIn::Code } }, kMaxScanHits, 0x400);
    CHECK(out.unreadable && !out.error.empty());

    // an empty request list and a request without a pattern are harmless
    CHECK(ScanBatch(*img, {}).perRequest.empty());
    const auto none = ScanBatch(*img, { { nullptr, SearchIn::Code } });
    CHECK(none.perRequest.size() == 1 && none.perRequest[0].rvas.empty());
}

void TestWildcardLeadingAndNibbles()
{
    // patterns that start with a wildcard and use nibble masks go through the same scan
    auto img = MakeImage();
    Plant(*img, 0x2000, "112233445566");
    Plant(*img, 0x2100, "1f2233445566");
    const Pattern lead = P("?? 22 33 44");
    auto h = Scan1(*img, lead);
    CHECK(h.rvas.size() == 2 && h.rvas[0] == 0x2000 && h.rvas[1] == 0x2100);
    const Pattern nib = P("1? 22 33 4? ?5");
    h = Scan1(*img, nib);
    CHECK(h.rvas.size() == 2);
    const Pattern nib2 = P("1? 22 33 4? ?6");
    CHECK(Scan1(*img, nib2).rvas.empty());
    const Pattern nib3 = P("11 22 33 4? ?5");
    h = Scan1(*img, nib3);
    CHECK(h.rvas.size() == 1 && h.rvas[0] == 0x2000);
}

// L3: patterns of DIFFERENT lengths in one batch, chunk sizes of 1..150 bytes (matches straddle borders everywhere), a tiny byte alphabet so
// that patterns hit often: ScanBatch must equal a naive scan for every request (deterministic generator, no executable)
void TestMixedLengthBatchesAtChunkBorders()
{
    std::uint32_t state = 0x1234567u;
    auto next = [&state]
    {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    };
    std::size_t totalHits = 0;
    for (int round = 0; round < 80; ++round)
    {
        auto img = MakeImage();
        auto& bytes = img->Bytes();
        for (std::uint32_t i = kTextRva; i < kTextRva + kTextSize; ++i)
            bytes[i] = static_cast<std::uint8_t>(0xa0 + next() % 3);

        std::vector<Pattern> patterns;
        for (int k = 0; k < 4; ++k)
        {
            const std::size_t n = 1 + next() % 12;
            const std::uint32_t at = kTextRva + next() % (kTextSize - 12);
            std::string text;
            bool fixed = false;
            for (std::size_t j = 0; j < n; ++j)
            {
                char token[4];
                const bool wild = next() % 4 == 0;
                if (wild)
                    std::snprintf(token, sizeof(token), "?? ");
                else
                {
                    std::snprintf(token, sizeof(token), "%02X ", bytes[at + j]);
                    fixed = true;
                }
                text += token;
            }
            if (!fixed)
            {
                text[0] = 'A';
                text[1] = '1';
            }
            patterns.push_back(P(text.c_str()));
        }
        std::vector<ScanRequest> requests;
        for (const Pattern& p : patterns)
            requests.push_back({ &p, SearchIn::Code });
        const std::size_t chunk = 1 + next() % 150;
        const ScanOutcome out = ScanBatch(*img, requests, kMaxScanHits, chunk);
        REQUIRE(out.perRequest.size() == patterns.size() && !out.unreadable);
        for (std::size_t r = 0; r < patterns.size(); ++r)
        {
            std::vector<std::uint32_t> expect;
            for (std::uint32_t i = kTextRva; i + patterns[r].size() <= kTextRva + kTextSize; ++i)
            {
                if (MatchAt(&bytes[i], patterns[r]))
                    expect.push_back(i);
            }
            if (expect.size() > kMaxScanHits)
                expect.resize(kMaxScanHits);
            if (out.perRequest[r].rvas != expect)
                std::printf("FAIL: round %d request %zu (chunk %zu, length %zu): %zu hits, naive %zu\n", round, r, chunk, patterns[r].size(),
                            out.perRequest[r].rvas.size(), expect.size());
            CHECK(out.perRequest[r].rvas == expect);
            totalHits += expect.size();
        }
    }
    CHECK(totalHits > 1000); // the generator really produces hits (a scan that finds nothing would also "equal" an empty naive scan)
}
} // namespace

int main()
{
    TestMixedLengthBatchesAtChunkBorders();
    TestParse();
    TestMatchAt();
    TestScanBasics();
    TestChunking();
    TestSectionBounds();
    TestSearchClasses();
    TestCapAndFailures();
    TestWildcardLeadingAndNibbles();
    return TEST_RESULT("resolve_pattern_scan");
}
