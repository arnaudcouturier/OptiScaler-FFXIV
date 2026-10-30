// The simulation-tick anchors on committed bytes only (no executable): a synthetic image that carries the two interior blocks of the tick, the
// prologue, the Framework instance load and the exception directory that gives the enclosing function.
//
//   1. the pins: the seam ids, kinds, anchor names and bindings of the group, and the fact that both interior anchors take the function start from
//      .pdata instead of a fixed offset back to the entry,
//   2. both builds' counter offsets resolve from the same instruction text (the 2024 build is 8 bytes below the 2026 one in all three), and the
//      install's layout check accepts both,
//   3. the decoys the design's safety rests on: a copy of the prologue in another function is filtered by SameFunctionAs (one inside the same
//      function is Ambiguous), one broken interior block still finds the tick (FoundSingleAnchor) and costs only the two counter offsets, both
//      broken turn the seam NotFound, an out-of-range counter is dropped by ValueIn, an instance load into initialized .data instead of the bss tail
//      is dropped by TargetIn, and the two wildcarded short-branch displacements of the teardown block are free.
#include "resolve_test_util.h"

#include "ext/ffxiv/resolve/Entry.h"
#include "ext/ffxiv/resolve/Registry.h"
#include "ext/latency/TickLeaseCore.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace rt;
using namespace ext::ffxiv;
using namespace ext::ffxiv::resolve;

namespace
{
// The synthetic layout of resolve_test_util.h: .text 0x1000..0x5000, .rdata 0x6000..0x8000, .data 0x8000..0xB000 (initialized up to 0x9000,
// bss above), .pdata 0xB000..0xC000.
constexpr std::uint32_t kTick = 0x2000;         // the tick function's entry
constexpr std::uint32_t kTickEnd = 0x2300;      // its .pdata range
constexpr std::uint32_t kClock = kTick + 0x1a;  // the clock block, at the displacement the two real builds have
constexpr std::uint32_t kTeardown = 0x2200;     // the Destroy / Free tail inside the same range
constexpr std::uint32_t kOtherFn = 0x3000;      // a second function, for the SameFunctionAs decoy
constexpr std::uint32_t kOtherEnd = 0x3100;
constexpr std::uint32_t kUnwind = 0x6000;       // UNWIND_INFO blocks in .rdata
constexpr std::uint32_t kInstanceLoad = 0x2800; // outside the tick function on purpose: it needs no function
constexpr std::uint32_t kInstanceBss = 0x9100;  // bss tail of .data
constexpr std::uint32_t kInstanceInit = 0x8800; // initialized part of .data

// The 2024 benchmark build's counter layout and the 2026 one's: the same instruction text, different displacements.
struct Offsets
{
    const char* name;
    std::uint32_t frameCounter, perfValue, perfFreq;
};
constexpr Offsets kOld { "2024", 0x16c8, 0x16a8, 0x16a0 };
constexpr Offsets kNew { "2026", 0x16d0, 0x16b0, 0x16a8 };

std::vector<std::uint8_t> Prologue(std::uint32_t counterOffset)
{
    auto b = FromHex("40 53 48 83 EC 20 FF 81 00 00 00 00 48 8B D9 48 8D 4C 24 30 FF 15");
    std::memcpy(&b[8], &counterOffset, 4);
    return b;
}

std::vector<std::uint8_t> ClockBlock(std::uint32_t valueOffset, std::uint32_t freqOffset)
{
    auto b = FromHex("48 8B 44 24 30 0F 57 C9 48 2B 83 00 00 00 00 0F 57 C0 F2 48 0F 2A 83 00 00 00 00 F2 48 0F 2A C8 F2 0F 5E C8 0F 57 C0 "
                     "66 0F 5A D1");
    std::memcpy(&b[11], &valueOffset, 4);
    std::memcpy(&b[23], &freqOffset, 4);
    return b;
}

// The two short branches carry a displacement of 0x20 here; the pattern wildcards both, so any value must do.
std::vector<std::uint8_t> Teardown() { return FromHex("80 7B 08 00 74 20 48 8B 03 48 8B CB FF 50 10 84 C0 74 20 48 8B 03 48 8B CB FF 50 18 32 C0 48 83 C4 20 5B C3"); }

std::vector<std::uint8_t> InstanceLoad(std::uint32_t at, std::uint32_t target)
{
    auto b = FromHex("48 8B 1D 00 00 00 00 8B 7C 24");
    const auto disp = static_cast<std::int32_t>(target - (at + 7));
    std::memcpy(&b[3], &disp, 4);
    return b;
}

// The image: two functions in .pdata, the clock block and the tail of the tick, the prologue, the instance load.
std::unique_ptr<DenseImage> MakeImage2()
{
    auto img = MakeImage(0x11223344, 2 * 12);
    if (!img)
        return nullptr;
    PutRuntimeFunction(*img, 0, kTick, kTickEnd, kUnwind);
    PutRuntimeFunction(*img, 1, kOtherFn, kOtherEnd, kUnwind + 0x40);
    PutUnwind(*img, kUnwind, 0, 0);
    PutUnwind(*img, kUnwind + 0x40, 0, 0);
    return img;
}

void PlantTick(DenseImage& img, const Offsets& o)
{
    Plant(img, kTick, ToHex(Prologue(o.frameCounter)));
    Plant(img, kClock, ToHex(ClockBlock(o.perfValue, o.perfFreq)));
    Plant(img, kTeardown, ToHex(Teardown()));
    Plant(img, kInstanceLoad, ToHex(InstanceLoad(kInstanceLoad, kInstanceBss)));
}

// Registry::Find returns a SeamResult; the specs are needed for the pins.
const SeamSpec* FindSeam(const Registry& reg, const std::string& id)
{
    for (const SeamSpec& s : reg.seams)
    {
        if (s.id == id)
            return &s;
    }
    return nullptr;
}

const SeamResult* Get(const ResolvedBuild& rb, const char* id, bool required = true)
{
    const SeamResult* s = rb.Find(id);
    if (s == nullptr && required)
    {
        std::printf("FAIL: seam %s is not in the registry\n", id);
        ++g_failures;
    }
    return s;
}

void CheckValue(const ResolvedBuild& rb, const char* id, std::uint32_t want)
{
    const SeamResult* s = Get(rb, id);
    if (s == nullptr)
        return;
    if (s->status != SeamStatus::Found || s->rva != want)
        std::printf("FAIL: %s is %s at 0x%x, expected Found at 0x%x\n", id, StatusName(s->status), s->rva, want);
    CHECK(s->status == SeamStatus::Found);
    CHECK(s->rva == want);
}

void TestPins()
{
    const Registry reg = BuildRegistry();
    CHECK(ValidateRegistry(reg).empty());

    struct Pin
    {
        const char* id;
        SeamKind kind;
        const char* anchors[2];
        std::size_t anchorCount;
        const char* key;
    };
    const Pin pins[] = {
        { "latency.tick", SeamKind::CodeSite, { "tickClock", "tickTeardown" }, 2, "latency.tickRva" },
        { "latency.frameworkInstance", SeamKind::Global, { "instanceLoad", nullptr }, 1, "latency.frameworkInstanceRva" },
        { "latency.frameCounterOffset", SeamKind::Offset, { "incFrameCounter", nullptr }, 1, "latency.frameCounterOffset" },
        { "latency.perfCounterValueOffset", SeamKind::Offset, { "perfCounterValue", nullptr }, 1, "latency.perfCounterValueOffset" },
        { "latency.perfCounterFreqOffset", SeamKind::Offset, { "perfCounterFreq", nullptr }, 1, "latency.perfCounterFreqOffset" },
    };
    std::size_t seams = 0, bindings = 0;
    for (const Pin& pin : pins)
    {
        const SeamSpec* s = FindSeam(reg, pin.id);
        if (s == nullptr)
        {
            std::printf("FAIL: seam %s is not registered\n", pin.id);
            ++g_failures;
            continue;
        }
        ++seams;
        CHECK(s->kind == pin.kind && !s->failClosed);
        CHECK(s->anchors.size() == pin.anchorCount);
        for (std::size_t i = 0; i < s->anchors.size() && i < pin.anchorCount; ++i)
            CHECK(s->anchors[i].name == pin.anchors[i]);
        // a table row is bound to it
        bool bound = false;
        for (const Binding& b : reg.bindings)
            bound = bound || (b.key == pin.key && b.seam == pin.id && b.valueOffset == 0);
        if (!bound)
            std::printf("FAIL: no binding %s -> %s\n", pin.key, pin.id);
        CHECK(bound);
        ++bindings;
    }
    CHECK(seams == 5);

    // Both interior anchors take the function start from .pdata: no fixed offset back to the entry, and neither starts at the entry (which a
    // foreign inline hook would hide, and which Resolver.h refuses for a CodeSite anyway).
    const SeamSpec* tick = FindSeam(reg, "latency.tick");
    REQUIRE(tick != nullptr);
    for (const AnchorSpec& a : tick->anchors)
    {
        CHECK(a.valueFrom == ValueFrom::EnclosingFunction && a.valueOffset == 0 && !a.entryAnchor && a.fromSeam.empty());
        CHECK(a.pattern.rfind("48", 0) == 0 || a.pattern.rfind("80", 0) == 0); // the clock block / the tail, never the push rbx of the entry
    }
    // the three offsets are decoded, with a sanity range, from instruction text of the tick
    for (const char* id : { "latency.frameCounterOffset", "latency.perfCounterValueOffset", "latency.perfCounterFreqOffset" })
    {
        const SeamSpec* s = FindSeam(reg, id);
        REQUIRE(s != nullptr);
        CHECK(s->anchors[0].decode.kind == DecodeKind::Field && s->anchors[0].decode.width == 4);
        CHECK(s->anchors[0].valueFrom == ValueFrom::Target);
        bool ranged = false, sameFunction = false;
        for (const Predicate& p : s->anchors[0].predicates)
        {
            // a sanity window that contains the offsets both builds have (0x16a0 .. 0x16d0) and stops far short of a wild decode
            ranged = ranged || (p.kind == PredicateKind::ValueIn && p.valueLo <= 0x1600 && p.valueHi >= 0x1700 && p.valueHi < 0x10000);
            sameFunction = sameFunction || (p.kind == PredicateKind::SameFunctionAs && p.seam == "latency.tick");
        }
        CHECK(ranged && sameFunction);
    }
}

void TestBothBuilds()
{
    const Registry reg = BuildRegistry();
    for (const Offsets& o : { kOld, kNew })
    {
        auto img = MakeImage2();
        REQUIRE(img != nullptr);
        PlantTick(*img, o);
        const ResolvedBuild rb = ResolveImage(*img, reg);
        CHECK(rb.registryErrors.empty() && !rb.scanFailed);
        CheckValue(rb, "latency.tick", kTick);
        CheckValue(rb, "latency.frameworkInstance", 0x9100);
        CheckValue(rb, "latency.frameCounterOffset", o.frameCounter);
        CheckValue(rb, "latency.perfCounterValueOffset", o.perfValue);
        CheckValue(rb, "latency.perfCounterFreqOffset", o.perfFreq);
        CHECK(rb.build.latency.tickRva == kTick && rb.build.latency.frameworkInstanceRva == 0x9100);
        CHECK(rb.build.latency.frameCounterOffset == o.frameCounter);
        CHECK(ext::latency::LayoutConsistent(rb.build.latency.frameCounterOffset, rb.build.latency.perfCounterValueOffset,
                                             rb.build.latency.perfCounterFreqOffset));
        std::printf("  %s offsets 0x%x / 0x%x / 0x%x resolve from the same text\n", o.name, o.frameCounter, o.perfValue, o.perfFreq);
    }
    // every planted displacement the same: a broken layout is what the install refuses (the check itself is a unit test of TickLeaseCore.h)
    auto broken = MakeImage2();
    REQUIRE(broken != nullptr);
    Plant(*broken, kTick, ToHex(Prologue(0x16d0)));
    Plant(*broken, kClock, ToHex(ClockBlock(0x16a8, 0x16a0)));
    Plant(*broken, kTeardown, ToHex(Teardown()));
    const ResolvedBuild rb = ResolveImage(*broken, reg);
    CHECK(rb.Usable("latency.tick"));
    CHECK(!ext::latency::LayoutConsistent(rb.build.latency.frameCounterOffset, rb.build.latency.perfCounterValueOffset,
                                          rb.build.latency.perfCounterFreqOffset));
}

void TestDecoys()
{
    const Registry reg = BuildRegistry();

    // (1) SameFunctionAs: a copy of the prologue in ANOTHER function is filtered; inside the SAME function it is a second candidate
    {
        auto img = MakeImage2();
        REQUIRE(img != nullptr);
        PlantTick(*img, kNew);
        Plant(*img, kOtherFn, ToHex(Prologue(kNew.frameCounter)));
        ResolvedBuild rb = ResolveImage(*img, reg);
        CheckValue(rb, "latency.frameCounterOffset", kNew.frameCounter);

        Plant(*img, kTick + 0x100, ToHex(Prologue(kNew.frameCounter)));
        rb = ResolveImage(*img, reg);
        const SeamResult* s = Get(rb, "latency.frameCounterOffset");
        REQUIRE(s != nullptr);
        CHECK(s->status == SeamStatus::Ambiguous && s->anchors[0].candidates == 2);
        CHECK(rb.Usable("latency.tick")); // the tick itself is unaffected
    }

    // (2) one interior block broken: the other anchor still finds the function, and only the two offsets decoded from the broken block are lost
    {
        auto img = MakeImage2();
        REQUIRE(img != nullptr);
        PlantTick(*img, kNew);
        const std::vector<std::uint8_t> broken = { 0x00 };
        img->Put(kClock + 1, broken);
        const ResolvedBuild rb = ResolveImage(*img, reg);
        const SeamResult* tick = Get(rb, "latency.tick");
        REQUIRE(tick != nullptr);
        CHECK(tick->status == SeamStatus::FoundSingleAnchor && tick->rva == kTick);
        CHECK(tick->anchors[0].outcome == AnchorOutcome::Zero && tick->anchors[1].outcome == AnchorOutcome::Unique);
        CHECK(!rb.Usable("latency.perfCounterValueOffset") && !rb.Usable("latency.perfCounterFreqOffset"));
        CHECK(rb.Usable("latency.frameCounterOffset")); // its own anchor, at the entry
    }

    // (3) both interior blocks broken: NotFound, and the frame-counter anchor (at the entry) is the only one left
    {
        auto img = MakeImage2();
        REQUIRE(img != nullptr);
        PlantTick(*img, kNew);
        const std::vector<std::uint8_t> broken = { 0x00 };
        img->Put(kClock + 1, broken);
        img->Put(kTeardown + 1, broken);
        const ResolvedBuild rb = ResolveImage(*img, reg);
        const SeamResult* tick = Get(rb, "latency.tick");
        REQUIRE(tick != nullptr);
        CHECK(tick->status == SeamStatus::NotFound);
        // the frame-counter anchor's SameFunctionAs has no value to compare with, so it is NotFound as well
        CHECK(!rb.Usable("latency.frameCounterOffset"));
    }

    // (4) a counter offset outside the sanity range is dropped, and the tick is not
    {
        auto img = MakeImage2();
        REQUIRE(img != nullptr);
        Plant(*img, kTick, ToHex(Prologue(0x5000)));
        Plant(*img, kClock, ToHex(ClockBlock(kNew.perfValue, kNew.perfFreq)));
        Plant(*img, kTeardown, ToHex(Teardown()));
        const ResolvedBuild rb = ResolveImage(*img, reg);
        CHECK(!rb.Usable("latency.frameCounterOffset"));
        CHECK(rb.Usable("latency.tick") && rb.Usable("latency.perfCounterValueOffset"));
    }

    // (5) the instance load: the bss tail passes TargetIn(Bss), the initialized part of .data does not
    {
        auto img = MakeImage2();
        REQUIRE(img != nullptr);
        PlantTick(*img, kNew);
        Plant(*img, kInstanceLoad, ToHex(InstanceLoad(kInstanceLoad, kInstanceBss)));
        CHECK(ResolveImage(*img, reg).Usable("latency.frameworkInstance"));
        img->Put(kInstanceLoad, InstanceLoad(kInstanceLoad, kInstanceInit));
        const ResolvedBuild init = ResolveImage(*img, reg);
        CHECK(!init.Usable("latency.frameworkInstance"));
        // and a target in .rdata is refused too (it is initialized data as well)
        img->Put(kInstanceLoad, InstanceLoad(kInstanceLoad, 0x6100));
        CHECK(!ResolveImage(*img, reg).Usable("latency.frameworkInstance"));
    }

    // (6) the two short-branch displacements of the tail are wildcards: any value of the two 0x74 bytes' operands matches
    {
        auto img = MakeImage2();
        REQUIRE(img != nullptr);
        PlantTick(*img, kNew);
        auto tail = Teardown();
        tail[5] = 0x7f;
        tail[18] = 0x02;
        img->Put(kTeardown, tail);
        // the clock block is still there, so the tick resolves from both anchors - and the tail anchor still matched although its two
        // short-branch displacements are no longer 0x20
        const ResolvedBuild rb = ResolveImage(*img, reg); // kept in a variable: the results are pointers into it
        const SeamResult* tick = Get(rb, "latency.tick");
        REQUIRE(tick != nullptr);
        CHECK(tick->status == SeamStatus::Found);
        CHECK(tick->anchors.size() == 2 && tick->anchors[1].name == "tickTeardown" && tick->anchors[1].outcome == AnchorOutcome::Unique);
    }
}
} // namespace

int main()
{
    TestPins();
    TestBothBuilds();
    TestDecoys();
    return TEST_RESULT("resolve_latency_old_build");
}
