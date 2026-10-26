// The offline uniqueness-margin helper (tools/ffxiv-resolve/Margin.h): the smallest distance, in fixed bytes, between a pattern and any
// other location. A pattern that is unique but within one or two bytes of another site must be flagged: the resolver says "Found", the
// margin says "one compiler tweak from two hits".
#include "resolve_test_util.h"

#include "Margin.h"
#include "ext/ffxiv/resolve/Registry.h"
#include "ext/ffxiv/resolve/Resolver.h"

using namespace rt;
using namespace ext::ffxiv::resolve::offline;

namespace
{
// byte index:  0:48 1:8B 2:05 [3..6 wildcard] 7:8B 8:CA 9:48 10:8B 11:84 12:C8 13:C8 14:02 15:00 16:00 17:C3  -> 14 fixed bytes
const char* const kPattern = "48 8B 05 ?? ?? ?? ?? 8B CA 48 8B 84 C8 C8 02 00 00 C3";
const char* const kReal = "488b05b9386a028bca488b84c8c8020000c3";

Pattern P(const char* text = kPattern)
{
    Pattern p;
    CHECK(ParsePattern(text, p));
    return p;
}

// the real accessor with the first `changes` of the bytes 12..15 flipped
std::string Near(int changes)
{
    auto bytes = FromHex(kReal);
    for (int i = 0; i < changes; ++i)
        bytes[12 + i] ^= 0xff;
    return ToHex(bytes);
}

void TestDistances()
{
    const Pattern p = P();
    REQUIRE(p.fixedBytes() == 14);

    for (int changes = 1; changes <= 4; ++changes)
    {
        auto img = MakeImage();
        Plant(*img, 0x2000, kReal);
        Plant(*img, 0x2400, Near(changes));
        const auto m = MinDistance(*img, p, SearchIn::Code, { 0x2000 });
        CHECK(m.found && m.distance == static_cast<std::uint32_t>(changes) && m.rva == 0x2400 && m.sitesAtDistance == 1);
        // the resolver is happy (the pattern IS unique); only the margin tells
        Registry reg;
        AnchorSpec a;
        a.name = "a";
        a.pattern = kPattern;
        reg.seams.push_back({ "m.s", SeamKind::Function, { a } });
        const auto rb = ResolveImage(*img, reg);
        CHECK(rb.Find("m.s") != nullptr && rb.Find("m.s")->status == SeamStatus::Found);
        CHECK(MarginOk(m, kRequiredMargin) == (changes >= 3)); // 1 and 2 bytes apart are too close, 3 and 4 are fine
    }

    // nothing near: no site within the cap
    auto alone = MakeImage();
    Plant(*alone, 0x2000, kReal);
    const auto none = MinDistance(*alone, p, SearchIn::Code, { 0x2000 });
    CHECK(!none.found && none.distance == kDefaultMarginCap + 1 && MarginOk(none, kRequiredMargin));
    // a site beyond the cap does not count, one inside it does
    Plant(*alone, 0x2400, Near(4));
    CHECK(MinDistance(*alone, p, SearchIn::Code, { 0x2000 }, 3).found == false);
    CHECK(MinDistance(*alone, p, SearchIn::Code, { 0x2000 }, 4).distance == 4);

    // without the exclusion the real hit itself is the nearest site (distance 0)
    const auto self = MinDistance(*alone, p, SearchIn::Code, {});
    CHECK(self.found && self.distance == 0 && self.rva == 0x2000);

    // two sites at the same distance are both counted; a closer one wins
    auto many = MakeImage();
    Plant(*many, 0x2000, kReal);
    Plant(*many, 0x2400, Near(2));
    Plant(*many, 0x2800, Near(2));
    auto m2 = MinDistance(*many, p, SearchIn::Code, { 0x2000 });
    CHECK(m2.distance == 2 && m2.sitesAtDistance == 2);
    Plant(*many, 0x2c00, Near(1));
    m2 = MinDistance(*many, p, SearchIn::Code, { 0x2000 });
    CHECK(m2.distance == 1 && m2.rva == 0x2c00 && m2.sitesAtDistance == 1);

    // a pair of real hits (one pattern for two sites): both are excluded
    auto pair = MakeImage();
    Plant(*pair, 0x2000, kReal);
    Plant(*pair, 0x2400, kReal);
    CHECK(MinDistance(*pair, p, SearchIn::Code, { 0x2000 }).distance == 0);
    CHECK(!MinDistance(*pair, p, SearchIn::Code, { 0x2000, 0x2400 }).found);

    // minMargin 0 = the predicates disambiguate: always accepted, even with a distance 0 site
    CHECK(MarginOk(self, 0) && !MarginOk(self, 1));
    CHECK(AnchorSpec {}.minMargin == kRequiredMargin);
}

void TestMasks()
{
    // wildcard bytes never count: a different displacement is an identical site (distance 0)
    auto img = MakeImage();
    Plant(*img, 0x2000, kReal);
    Plant(*img, 0x2400, "488b05ffffffff8bca488b84c8c8020000c3");
    CHECK(MinDistance(*img, P(), SearchIn::Code, { 0x2000 }).distance == 0);

    // a nibble pattern counts a byte as different only in its fixed nibble
    auto n = MakeImage();
    Plant(*n, 0x2000, "418b");
    Plant(*n, 0x2100, "518b");
    Plant(*n, 0x2200, "5a8b");
    const Pattern nib = P("4? 8B");
    const auto r = MinDistance(*n, nib, SearchIn::Code, { 0x2000 });
    CHECK(r.found && r.distance == 1); // 0x51 differs from 4? in the high nibble: one byte
    Plant(*n, 0x2300, "428b");         // matches 4? exactly: distance 0
    CHECK(MinDistance(*n, nib, SearchIn::Code, { 0x2000 }).distance == 0);
}

void TestScope()
{
    const Pattern p = P();
    // only the searched class counts: a near copy in .rdata is not a competitor for a code anchor (and vice versa)
    auto img = MakeImage();
    Plant(*img, 0x2000, kReal);
    Plant(*img, kRdataRva + 0x100, Near(1));
    CHECK(!MinDistance(*img, p, SearchIn::Code, { 0x2000 }).found);
    CHECK(MinDistance(*img, p, SearchIn::ReadOnlyData, {}).distance == 1);

    // a site that straddles a window border is measured
    for (std::size_t chunk : { std::size_t(64), std::size_t(100) })
    {
        for (std::uint32_t at = 0x2000 + 40; at < 0x2000 + 140; at += 9)
        {
            auto s = MakeImage();
            Plant(*s, 0x3000, kReal);
            Plant(*s, at, Near(2));
            const auto m = MinDistance(*s, p, SearchIn::Code, { 0x3000 }, kDefaultMarginCap, chunk);
            CHECK(m.found && m.distance == 2 && m.rva == at);
        }
    }

    // no read leaves a section
    auto bounds = MakeImage();
    std::uint32_t maxEnd = 0;
    bounds->readObserver = [&](std::uint32_t rva, std::size_t n) { maxEnd = std::max<std::uint32_t>(maxEnd, rva + static_cast<std::uint32_t>(n)); };
    MinDistance(*bounds, p, SearchIn::Code, {}, kDefaultMarginCap, 256);
    CHECK(maxEnd == kTextRva + kTextSize);

    // an unreadable image is reported and never passes
    struct Failing : ImageView
    {
        explicit Failing(const DenseImage& i) { _info = i.Info(); }
        bool Read(std::uint32_t, void*, std::size_t) const override { return false; }
    } failing(*bounds);
    const auto u = MinDistance(failing, p, SearchIn::Code, {});
    CHECK(u.unreadable && !MarginOk(u, kRequiredMargin));
}

void TestAnchorMargin()
{
    // the helper that takes a resolved anchor: the candidates the resolver kept are excluded, everything else competes
    const Registry reg = BuildRegistry();
    const SeamSpec* chara = nullptr;
    for (const SeamSpec& s : reg.seams)
        chara = std::string_view(s.id) == "chara.accessor" ? &s : chara;
    REQUIRE(chara != nullptr);

    auto img = MakeImage();
    Plant(*img, 0x2000, kReal);
    // the real accessor's target (0x026a38b9 + 0x2007) must lie in .bss of the small layout for the predicate: patch the displacement
    const std::int32_t disp = static_cast<std::int32_t>(0x9100 - (0x2000 + 7));
    img->Put(0x2003, &disp, 4);
    Plant(*img, 0x2400, Near(2));
    const auto rb = ResolveImage(*img, reg);
    const SeamResult* s = rb.Find("chara.accessor");
    REQUIRE(s != nullptr && s->status == SeamStatus::Found);
    const auto m = AnchorMargin(*img, chara->anchors[0], s->anchors[0]);
    CHECK(m.found && m.distance == 2 && m.rva == 0x2400);
    CHECK(!MarginOk(m, chara->anchors[0].minMargin)); // 2 < 3: this anchor would be flagged

    // a guard anchor is not scanned: no margin
    const SeamSpec* mgr = nullptr;
    for (const SeamSpec& sp : reg.seams)
        mgr = std::string_view(sp.id) == "chara.managerPointer" ? &sp : mgr;
    REQUIRE(mgr != nullptr);
    const auto g = AnchorMargin(*img, mgr->anchors[0], rb.Find("chara.managerPointer")->anchors[0]);
    CHECK(!g.found && MarginOk(g, mgr->anchors[0].minMargin));
}
} // namespace

int main()
{
    TestDistances();
    TestMasks();
    TestScope();
    TestAnchorMargin();
    return TEST_RESULT("resolve_margin");
}
