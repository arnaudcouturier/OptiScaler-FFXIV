// The jitter-source group (P2c) on committed bytes only (no executable):
//   1. the pins: every anchor of the group (pattern text, decode, predicates, margins, kinds) as string literals, independent of GameBuild.h,
//      and the bindings / mustDiffer entries,
//   2. the build-C layout image with the recorded blocks: every seam Found, the values equal the table, the entry text equals the table text,
//      the guard fields equal the table arrays, the return addresses come from valueOffset,
//   3. THE SAFETY GAP: on an unknown build the guard fields hold live bytes, so the hooks' own byte compare (VerifyAnchors) checks nothing by
//      itself. Here the real VerifyAnchors runs over a resolved build: the unmodified blocks pass, and flipping any byte the consumer relies on
//      makes it fail (or the seam unusable); masked bytes (RIP displacements, call targets, the TLS slot immediate) may change freely.
#include "resolve_jitter_shape_data.h"

#include "ext/ffxiv/jitter/FfxivJitterSourceAnchors.h"
#include "ext/ffxiv/resolve/Entry.h"
#include "ext/ffxiv/resolve/Registry.h"

#include <cstring>

using namespace jt;

namespace
{
struct SeamPin
{
    const char* id;
    SeamKind kind;
    std::vector<const char*> anchors;
};

// clang-format off
const std::vector<SeamPin>& Pins()
{
    static const std::vector<SeamPin> pins = {
        { "jitterSource.cameraHead", SeamKind::Function,
          {
              "headBody | 48 8B 01 48 8B F9 48 89 9C 24 B8 02 00 00 FF 50 38 48 8B 05 ?? ?? ?? ?? 32 D2 44 0F B6 40 54 41 80 | code hits 1/0 | from - | value function+0x0 | decode none(0,0,0,0) | entry 0 margin 3 |",
              "headExtents | 0F B7 81 00 07 00 00 66 0F 6E C8 0F B7 81 04 07 00 00 | code hits 1/0 | from - | value function+0x0 | decode none(0,0,0,0) | entry 0 margin 3 |",
          } },
        { "jitterSource.cameraHeadShape", SeamKind::CodeSite,
          {
              "shape | 40 55 57 48 8D AC 24 68 FE FF FF 48 81 EC 98 02 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 D0 00 00 00 48 8B 01 48 8B F9 48 89 9C 24 B8 02 00 00 FF 50 38 48 8B 05 ?? ?? ?? ?? 32 D2 44 0F B6 40 54 41 80 F8 02 74 06 83 78 2C 02 75 ?? | code hits 1/0 | from jitterSource.cameraHead@0x0 | value match+0x0 | decode none(0,0,0,0) | entry 0 margin 3 |",
          } },
        { "jitterSource.generator", SeamKind::Function,
          {
              "generatorBody | 48 8B F2 0F 29 78 D8 33 D2 44 0F 29 40 C8 49 8B F8 44 0F 29 48 B8 0F 57 FF F3 44 0F 10 0D ?? ?? ?? ?? 0F 57 F6 44 0F 29 50 A8 41 8B C1 F7 B4 24 A0 00 00 00 F3 44 0F 10 15 ?? ?? ?? ?? 45 0F 28 C2 8D 5A 01 8B CB | code hits 1/0 | from - | value function+0x0 | decode none(0,0,0,0) | entry 0 margin 3 |",
              "generatorCaller | 44 8B 4B 10 4C 8D 43 04 89 54 24 20 48 8D 4C 24 30 48 8B D3 E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 4C 8D 45 D0 | code hits 1/0 | from - | value target+0x0 | decode rel32(20,0,0,0) | entry 0 margin 3 |",
          } },
        { "jitterSource.generatorShape", SeamKind::CodeSite,
          {
              "shape | 48 8B C4 48 89 58 08 48 89 70 10 57 48 83 EC 70 0F 29 70 E8 48 8B F2 0F 29 78 D8 33 D2 44 0F 29 40 C8 49 8B F8 44 0F 29 48 B8 0F 57 FF F3 44 0F 10 0D ?? ?? ?? ?? 0F 57 F6 44 0F 29 50 A8 41 8B C1 F7 B4 24 A0 00 00 00 F3 44 0F 10 15 ?? ?? ?? ?? 45 0F 28 C2 8D 5A 01 8B CB 85 DB 0F 84 93 00 00 00 0F 57 C0 F3 45 0F 59 C1 8B C1 83 E0 01 F3 48 0F 2A C0 8B C1 F3 41 0F 59 C0 F3 0F 58 F0 0F 57 C0 F3 48 0F 2A C0 F3 41 0F 59 C1 E8 ?? ?? ?? ?? F3 48 0F 2C C8 85 C9 75 C8 F3 44 0F 10 05 ?? ?? ?? ?? 0F 1F 40 00 66 0F 1F 84 00 00 00 00 00 0F 57 C0 B8 AB AA AA AA F7 E3 F3 45 0F 5E D0 8B CB D1 EA 8D 04 52 2B C8 8B C1 F3 48 0F 2A C0 8B C3 F3 41 0F 59 C2 F3 0F 58 F8 0F 57 C0 F3 48 0F 2A C0 F3 41 0F 5E C0 E8 ?? ?? ?? ?? F3 48 0F 2C D8 85 DB 75 BB 4C 8D 5C 24 70 49 8B 5B 10 F3 41 0F 5C F1 45 0F 28 43 D0 F3 41 0F 5C F9 45 0F 28 4B C0 45 0F 28 53 B0 F3 0F 11 36 49 8B 73 18 0F 28 74 24 60 F3 0F 11 3F 0F 28 7C 24 50 49 8B E3 5F C3 | code hits 1/0 | from jitterSource.generator@0x0 | value match+0x0 | decode none(0,0,0,0) | entry 0 margin 3 |",
          } },
        { "jitterSource.cameraGetter", SeamKind::Function,
          {
              "getterBytes | 48 8B 81 78 02 00 00 C3 | code hits 1/0 | from - | value match+0x0 | decode none(0,0,0,0) | entry 0 margin 0 | within(jitterSource.cameraHead,-0x1000,-0x1)",
              "getterCaller | 4C 89 BE 18 01 00 00 E8 ?? ?? ?? ?? 0F 57 C0 41 BD ?? ?? ?? ?? 0F 57 C9 48 8B 08 48 89 8E 20 01 00 00 | code hits 1/0 | from - | value target+0x0 | decode rel32(7,0,0,0) | entry 0 margin 3 |",
          } },
        { "jitterSource.cameraGetterShape", SeamKind::CodeSite,
          {
              "shape | 48 8B 81 78 02 00 00 C3 | code hits 1/0 | from jitterSource.cameraGetter@0x0 | value match+0x0 | decode none(0,0,0,0) | entry 0 margin 3 |",
          } },
        { "jitterSource.producerCall", SeamKind::CodeSite,
          {
              "producerCall | E8 ?? ?? ?? ?? 0F 57 C0 41 BD ?? ?? ?? ?? 0F 57 C9 48 8B 08 | code hits 1/0 | from - | value match+0x0 | decode rel32(0,0,0,0) | entry 0 margin 3 | calls(jitterSource.cameraGetter)",
              "producerPre | 48 8B 88 28 30 00 00 48 8B 86 68 01 00 00 49 89 06 48 8B 86 78 01 00 00 48 89 86 00 01 00 00 48 8B 87 50 06 00 00 48 89 86 08 01 00 00 48 8B 47 70 48 89 86 10 01 00 00 4C 89 BE 18 01 00 00 E8 ?? ?? ?? ?? | code hits 1/0 | from - | value match+0x3f | decode rel32(63,0,0,0) | entry 0 margin 3 | calls(jitterSource.cameraGetter)",
          } },
        { "jitterSource.producerCallShape", SeamKind::CodeSite,
          {
              "shape | 48 8B 05 ?? ?? ?? ?? 48 8B 3D ?? ?? ?? ?? 48 8B 1D ?? ?? ?? ?? 48 8B 88 28 30 00 00 48 8B 86 68 01 00 00 49 89 06 48 8B 86 78 01 00 00 48 89 86 00 01 00 00 48 8B 87 50 06 00 00 48 89 86 08 01 00 00 48 8B 47 70 48 89 86 10 01 00 00 4C 89 BE 18 01 00 00 E8 ?? ?? ?? ?? 0F 57 C0 41 BD ?? ?? ?? ?? 0F 57 C9 48 8B 08 48 89 8E 20 01 00 00 | code hits 1/0 | from jitterSource.producerCall@-0x54 | value match+0x54 | decode none(0,0,0,0) | entry 0 margin 3 |",
          } },
        { "jitterSource.phases", SeamKind::CodeSite,
          {
              "phasesImmediate | 48 8B 05 ?? ?? ?? ?? 0F 57 F6 48 8B 9F 78 02 00 00 BA FF FF FF FF 48 8B 88 28 04 00 00 | code hits 1/0 | from - | value match+0x0 | decode none(0,0,0,0) | entry 0 margin 3 | sameFunction(jitterSource.cameraHead)",
              "phasesBranch | 41 80 F8 02 74 33 48 8B 05 ?? ?? ?? ?? 0F 57 C0 F3 0F 10 0D ?? ?? ?? ?? 8B 88 8C 00 00 00 F3 48 0F 2A C1 F3 0F 5E C6 E8 ?? ?? ?? ?? | code hits 1/0 | from - | value match+-0x2b | decode none(0,0,0,0) | entry 0 margin 3 | sameFunction(jitterSource.cameraHead)",
          } },
        { "jitterSource.phasesShape", SeamKind::CodeSite,
          {
              "shape | 48 8B 05 ?? ?? ?? ?? 0F 57 F6 48 8B 9F 78 02 00 00 BA FF FF FF FF 48 8B 88 28 04 00 00 48 8B F1 8B C1 48 C1 EE 20 F3 48 0F 2A F0 41 80 F8 02 74 33 48 8B 05 ?? ?? ?? ?? 0F 57 C0 F3 0F 10 0D ?? ?? ?? ?? 8B 88 8C 00 00 00 F3 48 0F 2A C1 F3 0F 5E C6 E8 ?? ?? ?? ?? F3 0F 59 05 ?? ?? ?? ?? F3 48 0F 2C D0 44 8B 4B 10 4C 8D 43 04 89 54 24 20 48 8D 4C 24 30 48 8B D3 | code hits 1/0 | from jitterSource.phases@0x0 | value match+0x0 | decode none(0,0,0,0) | entry 0 margin 3 |",
          } },
        { "jitterSource.strength", SeamKind::CodeSite,
          {
              "strengthCall | E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 4C 8D 45 D0 48 8B 03 48 8D 57 50 48 89 44 24 38 | code hits 1/0 | from - | value match+0x0 | decode rel32(0,0,0,0) | entry 0 margin 3 | calls(jitterSource.generator) sameFunction(jitterSource.cameraHead)",
              "strengthArgs | 44 8B 4B 10 4C 8D 43 04 89 54 24 20 48 8D 4C 24 30 48 8B D3 E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 4C 8D 45 D0 | code hits 1/0 | from - | value match+0x14 | decode rel32(20,0,0,0) | entry 0 margin 3 | calls(jitterSource.generator) sameFunction(jitterSource.cameraHead)",
          } },
        { "jitterSource.strengthShape", SeamKind::CodeSite,
          {
              "shape | E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 4C 8D 45 D0 48 8B 03 48 8D 57 50 48 89 44 24 38 0F B7 81 00 07 00 00 66 0F 6E C8 0F B7 81 04 07 00 00 48 8B 8F 78 02 00 00 0F 5B C9 66 0F 6E C0 48 8B 05 ?? ?? ?? ?? 0F 5B C0 F3 0F 10 50 78 F3 0F 5C 15 ?? ?? ?? ?? F3 0F 5E C8 0F 57 C0 F3 0F 59 48 4C F3 48 0F 2A C6 F3 0F 59 C9 F3 0F 59 D1 F3 0F 58 15 ?? ?? ?? ?? F3 0F 58 D2 0F 28 CA F3 0F 59 54 24 3C F3 0F 59 4C 24 38 0F 57 15 ?? ?? ?? ?? F3 0F 5E D0 0F 28 05 ?? ?? ?? ?? F3 0F 5E CE 0F 29 45 D0 0F 28 05 ?? ?? ?? ?? F3 0F 11 4C 24 38 0F 28 0D ?? ?? ?? ?? 0F 29 4D E0 0F 29 45 F0 F3 0F 11 54 24 3C 48 8B 44 24 38 48 89 41 08 | code hits 1/0 | from jitterSource.strength@0x0 | value match+0x0 | decode none(0,0,0,0) | entry 0 margin 3 |",
          } },
        { "jitterSource.deviceSize", SeamKind::CodeSite,
          {
              "outputPair | 48 8B 0D ?? ?? ?? ?? 44 8B 81 90 00 00 00 8B 91 8C 00 00 00 | code hits 2/0 | from - | value match+0x0 | decode rip(0,3,7,0) | entry 0 margin 3 | targetIs(jitterSource.devicePointer)",
          } },
        { "jitterSource.settingsPointer", SeamKind::Global,
          {
              "settingsInHead | 48 8B 05 ?? ?? ?? ?? 32 D2 44 0F B6 40 54 | code hits 1/0 | from jitterSource.cameraHeadShape@0x34 | value target+0x0 | decode rip(0,3,7,0) | entry 0 margin 3 | targetIn(4)",
              "settingsInStrength | 48 8B 05 ?? ?? ?? ?? 0F 5B C0 F3 0F 10 50 78 | code hits 1/0 | from jitterSource.strengthShape@0x3c | value target+0x0 | decode rip(0,3,7,0) | entry 0 margin 3 | targetIn(4)",
          } },
        { "jitterSource.rendererPointer", SeamKind::Global,
          {
              "rendererInPhases | 48 8B 05 ?? ?? ?? ?? 0F 57 F6 48 8B 9F 78 02 00 00 | code hits 1/0 | from jitterSource.phasesShape@0x0 | value target+0x0 | decode rip(0,3,7,0) | entry 0 margin 3 | targetIn(4)",
              "rendererInStrength | 48 8B 0D ?? ?? ?? ?? 4C 8D 45 D0 | code hits 1/0 | from jitterSource.strengthShape@0x5 | value target+0x0 | decode rip(0,3,7,0) | entry 0 margin 3 | targetIn(4)",
              "rendererInProducer | 48 8B 3D ?? ?? ?? ?? 48 8B 1D | code hits 1/0 | from jitterSource.producerCallShape@-0x4d | value target+0x0 | decode rip(0,3,7,0) | entry 0 margin 3 | targetIn(4)",
          } },
        { "jitterSource.devicePointer", SeamKind::Global,
          {
              "deviceInPhases | 48 8B 05 ?? ?? ?? ?? 0F 57 C0 F3 0F 10 0D | code hits 1/0 | from jitterSource.phasesShape@0x31 | value target+0x0 | decode rip(0,3,7,0) | entry 0 margin 3 | targetIn(4)",
              "deviceInProducer | 48 8B 1D ?? ?? ?? ?? 48 8B 88 28 30 00 00 | code hits 1/0 | from jitterSource.producerCallShape@-0x46 | value target+0x0 | decode rip(0,3,7,0) | entry 0 margin 3 | targetIn(4)",
          } },
        { "jitterSource.cameraGlobal", SeamKind::Global,
          {
              "cameraInProducer | 48 8B 05 ?? ?? ?? ?? 48 8B 3D | code hits 1/0 | from jitterSource.producerCallShape@-0x54 | value target+0x0 | decode rip(0,3,7,0) | entry 0 margin 3 | targetIn(4)",
          } },
        { "jitterSource.settingsModeOffset", SeamKind::Offset,
          {
              "modeByte | 44 0F B6 40 ?? 41 80 F8 02 | code hits 1/0 | from jitterSource.cameraHeadShape@0x3d | value target+0x0 | decode field(4,0,0,1) | entry 0 margin 3 | valueIn(0x10,0x7f)",
          } },
        { "jitterSource.settingsScaleOffset", SeamKind::Offset,
          {
              "scale | F3 0F 59 48 ?? F3 48 0F 2A C6 | code hits 1/0 | from jitterSource.strengthShape@0x5a | value target+0x0 | decode field(4,0,0,1) | entry 0 margin 3 | valueIn(0x10,0x7f)",
          } },
        { "jitterSource.settingsAmountOffset", SeamKind::Offset,
          {
              "amount | F3 0F 10 50 ?? F3 0F 5C 15 | code hits 1/0 | from jitterSource.strengthShape@0x46 | value target+0x0 | decode field(4,0,0,1) | entry 0 margin 3 | valueIn(0x10,0x7f)",
          } },
        { "jitterSource.rendererSceneSizeOffset", SeamKind::Offset,
          {
              "sceneInPhases | 48 8B 88 ?? ?? 00 00 48 8B F1 | code hits 1/0 | from jitterSource.phasesShape@0x16 | value target+0x0 | decode field(3,0,0,4) | entry 0 margin 3 | valueIn(0x100,0xfff)",
              "sceneInProducer | 8B 87 ?? ?? 00 00 4C 89 BC 24 90 00 00 00 89 84 24 90 00 00 00 8B 87 ?? ?? 00 00 89 84 24 94 00 00 00 | code hits 1/0 | from jitterSource.producerCallShape@0x62 | value target+0x0 | decode field(2,0,0,4) | entry 0 margin 3 | valueIn(0x100,0xfff)",
          } },
        { "jitterSource.rendererExtentOffset", SeamKind::Offset,
          {
              "extents | 0F B7 81 ?? ?? 00 00 66 0F 6E C8 0F B7 81 ?? ?? 00 00 | code hits 1/0 | from jitterSource.strengthShape@0x1c | value target+0x0 | decode field(3,0,0,4) | entry 0 margin 3 | valueIn(0x100,0xfff)",
          } },
        { "jitterSource.deviceOutputSizeOffset", SeamKind::Offset,
          {
              "widthInPhases | 8B 88 ?? ?? 00 00 F3 48 0F 2A C1 | code hits 1/0 | from jitterSource.phasesShape@0x43 | value target+0x0 | decode field(2,0,0,4) | entry 0 margin 3 | valueIn(0x10,0xff)",
              "widthInDeviceSize | 8B 91 ?? ?? 00 00 | code hits 1/0 | from jitterSource.deviceSize@0xe | value target+0x0 | decode field(2,0,0,4) | entry 0 margin 3 | valueIn(0x10,0xff)",
          } },
        { "jitterSource.strengthFloor", SeamKind::Offset,
          {
              "floorInSubss | F3 0F 5C 15 ?? ?? ?? ?? | code hits 1/0 | from jitterSource.strengthShape@0x4b | value target+0x0 | decode deref(0,4,8,0) | entry 0 margin 3 | valueIn(0x3a83126f,0x3a83126f)",
              "floorInAddss | F3 0F 58 15 ?? ?? ?? ?? | code hits 1/0 | from jitterSource.strengthShape@0x6c | value target+0x0 | decode deref(0,4,8,0) | entry 0 margin 3 | valueIn(0x3a83126f,0x3a83126f)",
          } },
        { "jitterSource.ndcSignMask", SeamKind::Offset,
          {
              "signMask | 0F 57 15 ?? ?? ?? ?? | code hits 1/0 | from jitterSource.strengthShape@0x87 | value target+0x0 | decode deref(0,3,7,0) | entry 0 margin 3 | valueIn(0x80000000,0x80000000)",
          } },
    };
    return pins;
}
// clang-format on

std::string KindName(SeamKind k)
{
    switch (k)
    {
    case SeamKind::Function: return "Function";
    case SeamKind::CodeSite: return "CodeSite";
    case SeamKind::Global: return "Global";
    case SeamKind::Table: return "Table";
    case SeamKind::Offset: return "Offset";
    }
    return "?";
}

void Dump()
{
    const Registry reg = JitterRegistry();
    for (const SeamSpec& s : reg.seams)
    {
        std::printf("        { \"%s\", SeamKind::%s,\n          {\n", s.id.c_str(), KindName(s.kind).c_str());
        for (const AnchorSpec& a : s.anchors)
            std::printf("              \"%s\",\n", Summary(a).c_str());
        std::printf("          } },\n");
    }
}

void TestPins()
{
    const Registry reg = JitterRegistry();
    CHECK(ValidateRegistry(reg).empty());
    for (const auto& e : ValidateRegistry(reg))
        std::printf("    registry error: %s\n", e.c_str());

    const auto& pins = Pins();
    CHECK(reg.seams.size() == pins.size());
    for (const SeamPin& pin : pins)
    {
        const SeamSpec* s = FindSeam(reg, pin.id);
        if (s == nullptr)
        {
            std::printf("FAIL: seam %s is not registered\n", pin.id);
            CHECK(s != nullptr);
            continue;
        }
        CHECK(s->kind == pin.kind);
        CHECK(s->anchors.size() == pin.anchors.size());
        for (std::size_t i = 0; i < s->anchors.size() && i < pin.anchors.size(); ++i)
        {
            const std::string got = Summary(s->anchors[i]);
            if (got != pin.anchors[i])
                std::printf("FAIL: %s anchor %zu\n   got    %s\n   pinned %s\n", pin.id, i, got.c_str(), pin.anchors[i]);
            CHECK(got == pin.anchors[i]);
        }
    }
    // every seam id belongs to this group
    for (const SeamSpec& s : reg.seams)
        CHECK(s.id.rfind("jitterSource.", 0) == 0);

    // SeamSpec::failClosed, pinned per seam: exactly the seven shape seams carry it (a shape that does not match is NEVER filled from the table on
    // a known build: the table's Guard array is only a prefix of the strict pattern), no location, global, offset or constant seam does
    {
        const char* failClosed[] = { "jitterSource.cameraHeadShape", "jitterSource.generatorShape", "jitterSource.cameraGetterShape",
                                     "jitterSource.producerCallShape", "jitterSource.phasesShape", "jitterSource.strengthShape", "jitterSource.deviceSize" };
        for (const SeamSpec& s : reg.seams)
        {
            const bool expected = std::find_if(std::begin(failClosed), std::end(failClosed), [&](const char* id) { return s.id == id; }) != std::end(failClosed);
            if (s.failClosed != expected)
                std::printf("FAIL: %s failClosed is %d, expected %d\n", s.id.c_str(), s.failClosed ? 1 : 0, expected ? 1 : 0);
            CHECK(s.failClosed == expected);
        }
    }

    // the guard anchors of a SHAPE seam: exactly one (a second anchor would let a failing piece read FoundSingleAnchor = usable, see the header)
    for (const char* shape : { "jitterSource.cameraHeadShape", "jitterSource.generatorShape", "jitterSource.cameraGetterShape",
                               "jitterSource.producerCallShape", "jitterSource.phasesShape", "jitterSource.strengthShape" })
    {
        const SeamSpec* s = FindSeam(reg, shape);
        REQUIRE(s != nullptr && s->kind == SeamKind::CodeSite && s->anchors.size() == 1 && !s->anchors[0].fromSeam.empty());
    }
    // deviceSize is its own shape: one scanned anchor, 20 bytes, two template copies
    {
        const SeamSpec* d = FindSeam(reg, "jitterSource.deviceSize");
        REQUIRE(d != nullptr && d->anchors.size() == 1 && d->anchors[0].expectedHits == 2 && d->anchors[0].pickIndex == 0);
        CHECK(Tokens(d->anchors[0].pattern).size() == 20);
    }

    // the pattern sizes of the shapes (the strict patterns are what the safety design rests on)
    struct Size { const char* id; std::size_t bytes, fixed; };
    for (const Size& z : { Size { "jitterSource.generatorShape", 306, 286 }, Size { "jitterSource.cameraGetterShape", 8, 8 },
                           Size { "jitterSource.cameraHeadShape", 78, 69 }, Size { "jitterSource.phasesShape", 120, 100 },
                           Size { "jitterSource.strengthShape", 204, 168 }, Size { "jitterSource.producerCallShape", 111, 91 } })
    {
        Pattern p;
        const SeamSpec* s = FindSeam(reg, z.id);
        REQUIRE(s != nullptr && ParsePattern(s->anchors[0].pattern, p));
        CHECK(p.size() == z.bytes && p.fixedBytes() == z.fixed);
    }

    // the bindings: Rva fields on the LOCATION seams, Guard fields on the SHAPE seams, offsets on the Offset seams
    struct Bind { const char* key; const char* seam; std::int32_t guardOffset, valueOffset; };
    const Bind binds[] = {
        { "jitterSource.generatorRva", "jitterSource.generator", 0, 0 },
        { "jitterSource.cameraGetterRva", "jitterSource.cameraGetter", 0, 0 },
        { "jitterSource.producerCallRva", "jitterSource.producerCall", 0, 0 },
        { "jitterSource.producerReturnRva", "jitterSource.producerCall", 0, 5 },
        { "jitterSource.cameraHeadRva", "jitterSource.cameraHead", 0, 0 },
        { "jitterSource.phasesRva", "jitterSource.phases", 0, 0 },
        { "jitterSource.strengthRva", "jitterSource.strength", 0, 0 },
        { "jitterSource.generatorCallReturnRva", "jitterSource.strength", 0, 5 },
        { "jitterSource.deviceSizeRva", "jitterSource.deviceSize", 0, 0 },
        { "jitterSource.settingsPointerRva", "jitterSource.settingsPointer", 0, 0 },
        { "jitterSource.rendererPointerRva", "jitterSource.rendererPointer", 0, 0 },
        { "jitterSource.devicePointerRva", "jitterSource.devicePointer", 0, 0 },
        { "jitterSource.generatorInstructions", "jitterSource.generatorShape", 0, 0 },
        { "jitterSource.cameraGetterInstructions", "jitterSource.cameraGetterShape", 0, 0 },
        { "jitterSource.producerCallInstructions", "jitterSource.producerCallShape", 0, 0 },
        { "jitterSource.cameraHeadInstructions", "jitterSource.cameraHeadShape", 0, 0 },
        { "jitterSource.phasesInstructions", "jitterSource.phasesShape", 0, 0 },
        { "jitterSource.strengthInstructions", "jitterSource.strengthShape", 0, 0 },
        { "jitterSource.deviceSizeInstructions", "jitterSource.deviceSize", 0, 0 },
        { "jitterSource.settingsScaleOffset", "jitterSource.settingsScaleOffset", 0, 0 },
        { "jitterSource.settingsModeOffset", "jitterSource.settingsModeOffset", 0, 0 },
        { "jitterSource.settingsAmountOffset", "jitterSource.settingsAmountOffset", 0, 0 },
        { "jitterSource.rendererSceneSizeOffset", "jitterSource.rendererSceneSizeOffset", 0, 0 },
        { "jitterSource.rendererExtentOffset", "jitterSource.rendererExtentOffset", 0, 0 },
        { "jitterSource.deviceOutputSizeOffset", "jitterSource.deviceOutputSizeOffset", 0, 0 },
    };
    CHECK(reg.bindings.size() == sizeof(binds) / sizeof(binds[0]));
    for (const Bind& b : binds)
    {
        bool found = false;
        for (const Binding& have : reg.bindings)
            found = found || (have.key == b.key && have.seam == b.seam && have.guardOffset == b.guardOffset && have.valueOffset == b.valueOffset);
        if (!found)
            std::printf("FAIL: binding %s -> %s (%d, %d) is missing\n", b.key, b.seam, b.guardOffset, b.valueOffset);
        CHECK(found);
    }
    // every GameBuild field of the group is bound (FieldTable.h has 25 rows for jitterSource, all Rva, Guard or Offset; none is a constant)
    std::size_t groupFields = 0;
    for (const FieldRow& row : kFieldTable)
        groupFields += std::string(row.group) == "jitterSource" && row.kind != FieldKind::Constant && row.kind != FieldKind::Pattern ? 1 : 0;
    CHECK(groupFields == reg.bindings.size());

    // mustDiffer: the two Detoured functions
    CHECK(reg.mustDiffer.size() == 1 && reg.mustDiffer[0].size() == 2 && reg.mustDiffer[0][0] == "jitterSource.generator" &&
          reg.mustDiffer[0][1] == "jitterSource.cameraGetter");
}

// ---- the resolved build on the committed bytes ------------------------------------------------------------------------------

void TestResolveOnCommittedBytes()
{
    const Registry reg = JitterRegistry();
    auto img = MakeJitterImage();
    REQUIRE(img != nullptr);

    LogCapture log;
    ResolveOptions opts;
    opts.knownTable = &ext::ffxiv::kCurrentBuild;
    opts.sink = log.Sink();
    const ResolvedBuild known = ResolveImage(*img, reg, opts);
    CHECK(known.registryErrors.empty() && known.knownBuild);
    for (const SeamResult& s : known.seams)
    {
        if (s.status != SeamStatus::Found)
            std::printf("FAIL: %s is %s: %s\n", s.id.c_str(), StatusName(s.status), s.reason.c_str());
        CHECK(s.status == SeamStatus::Found);
        CHECK(!s.fellBackToTable && s.agreement != Agreement::Disagree);
    }
    CHECK(known.Count(SeamStatus::Found) == reg.seams.size());

    // pure pattern-search answer (no table): the entry text equals the table text of every bound field, and so does every field
    const ResolvedBuild pure = ResolveImage(*img, reg);
    CHECK(!pure.knownBuild && pure.Count(SeamStatus::Found) == reg.seams.size());
    CHECK(FormatEntry(pure, reg) == FormatTableEntry(ext::ffxiv::kCurrentBuild, reg));
    const auto& t = ext::ffxiv::kCurrentBuild.jitterSource;
    const auto& j = pure.build.jitterSource;
    CHECK(j.generatorRva == 0x2ebd90 && j.cameraGetterRva == 0x25f610 && j.producerCallRva == 0x37411d && j.cameraHeadRva == 0x25f690);
    CHECK(j.phasesRva == 0x25f721 && j.strengthRva == 0x25f799 && j.deviceSizeRva == 0xd3f73);
    CHECK(j.producerReturnRva == 0x374122 && j.generatorCallReturnRva == 0x25f79e); // call + 5, derived by valueOffset
    CHECK(j.settingsPointerRva == 0x290e470 && j.rendererPointerRva == 0x290e490 && j.devicePointerRva == 0x2907d00);
    // the camera global the producer loads is the quality group's jitter.cameraGlobalRva (decoded here under this group's id, no field bound)
    CHECK(Get(pure, "jitterSource.cameraGlobal").rva == 0x290dba0 && Get(pure, "jitterSource.cameraGlobal").rva == ext::ffxiv::kCurrentBuild.jitter.cameraGlobalRva);
    CHECK(j.settingsScaleOffset == 0x4c && j.settingsModeOffset == 0x54 && j.settingsAmountOffset == 0x78);
    CHECK(j.rendererSceneSizeOffset == 0x428 && j.rendererExtentOffset == 0x700 && j.deviceOutputSizeOffset == 0x8c);
    CHECK(j.generatorInstructions == t.generatorInstructions && j.cameraGetterInstructions == t.cameraGetterInstructions);
    CHECK(j.producerCallInstructions == t.producerCallInstructions && j.cameraHeadInstructions == t.cameraHeadInstructions);
    CHECK(j.phasesInstructions == t.phasesInstructions && j.strengthInstructions == t.strengthInstructions);
    CHECK(j.deviceSizeInstructions == t.deviceSizeInstructions);

    // the two constants that have no GameBuild field
    CHECK(Get(pure, "jitterSource.strengthFloor").rva == 0x3a83126f && Get(pure, "jitterSource.ndcSignMask").rva == 0x80000000u);
    float floor = 0;
    const std::uint32_t bits = Get(pure, "jitterSource.strengthFloor").rva;
    std::memcpy(&floor, &bits, sizeof(floor));
    CHECK(floor == 0.001f);

    // the hits per anchor (what the golden test measured on the executable, here on the committed bytes)
    CHECK(Get(pure, "jitterSource.cameraGetter").anchors[0].patternHits == 2 && Get(pure, "jitterSource.cameraGetter").anchors[0].candidates == 1);
    CHECK(Get(pure, "jitterSource.deviceSize").hits == 2);
    CHECK(Get(pure, "jitterSource.cameraHead").hits == 2 && Get(pure, "jitterSource.rendererPointer").hits == 3);
    CHECK(Get(pure, "jitterSource.strength").hits == 2 && Get(pure, "jitterSource.phases").hits == 2 && Get(pure, "jitterSource.producerCall").hits == 2);

    // Rva fields start at zero and only usable seams fill them: on an image with none of the blocks nothing is filled
    auto empty = DenseImage::Synthetic(0x1, kSizeC, { { ".text", 0x1000, 0x210bf12, 0x210c000, DenseImage::kText } });
    REQUIRE(empty != nullptr);
    const ResolvedBuild none = ResolveImage(*empty, reg);
    CHECK(none.Count(SeamStatus::Found) == 0 && none.build.jitterSource.generatorRva == 0 && none.build.jitterSource.cameraGetterRva == 0);
    CHECK(none.build.jitterSource.generatorInstructions[0] == 0 && none.build.jitterSource.phasesInstructions[5] == 0);
}

// ---- the safety gap -----------------------------------------------------------------------------------------------------------

struct Flip
{
    const char* what;       // the consumer assumption this byte carries
    std::uint32_t rva;
    std::uint8_t to;
    const char* block;      // the VerifyAnchors block that must refuse (ASSERTED), or "seam:<id>" for a seam without a Guard field
    bool locationDies = false; // the byte also lies in BOTH location anchors of its site: the location is dead too and the shape is not reached
};

// the hook site (location seam, shape seam, ...) whose VerifyAnchors block is `name`
const ext::ffxiv::resolve::JitterHookSite* SiteOfBlock(const std::string& name)
{
    for (const auto& s : ext::ffxiv::resolve::kJitterHookSites)
    {
        if (name == s.block)
            return &s;
    }
    return nullptr;
}

void TestSafetyGap()
{
    // unmodified: an unknown build (stamp differs from C's), the live bytes ARE the committed ones
    {
        auto img = MakeJitterImage(0x11111111);
        REQUIRE(img != nullptr);
        const Registry reg = JitterRegistry();
        const ResolvedBuild rb = ResolveImage(*img, reg);
        CHECK(!rb.knownBuild && rb.Count(SeamStatus::Found) == reg.seams.size());
        const auto ok = ext::ffxiv::jitter::source::VerifyAnchors(ReaderOf(*img), rb.build.jitterSource, kBase);
        CHECK(ok.ok);
        // the guard fields hold the bytes of the image (live bytes), including the masked displacements
        std::array<std::uint8_t, 16> g {};
        REQUIRE(img->Read(0x2ebd90, g.data(), g.size()));
        CHECK(rb.build.jitterSource.generatorInstructions == g);
    }

    // every byte the consumer reads, one at a time (the committed block with ONE byte changed): the hooks must refuse
    const Flip flips[] = {
        // generator: Detour boundary, argument roles, outputs
        { "generator prologue: sub rsp,0x70", 0x2ebd9f, 0x68, "generator" },
        { "generator: rsi = rdx (the x pointer)", 0x2ebda6, 0xf3, "generator" },
        { "generator: rdi = r8 (the y pointer)", 0x2ebdb4, 0xf9, "generator" },
        { "generator: r9d is the frame counter", 0x2ebdd0, 0xc2, "generator" },
        { "generator: div [rsp+0xa0] = the fifth argument", 0x2ebdd4, 0xa8, "generator" },
        { "generator: x stored through rsi", 0x2ebeaa, 0x3e, "generator" },
        { "generator: y stored through rdi", 0x2ebeb7, 0x37, "generator" },
        { "generator: ret", 0x2ebec1, 0xcb, "generator" },
        // camera getter: +0x278 and the leaf shape
        { "getter: mov rax,[rcx+0x278]", 0x25f613, 0x80, "cameraGetter" },
        { "getter: ret", 0x25f617, 0xcb, "cameraGetter" },
        // camera head: frame, settings mode byte and its compare
        { "head: sub rsp,0x298", 0x25f69e, 0x90, "cameraHead" },
        { "head: movzx r8d,byte [rax+0x54]", 0x25f6d1, 0x58, "cameraHead" },
        { "head: cmp r8b,2", 0x25f6d5, 0x03, "cameraHead" },
        { "head: call [rax+0x38]", 0x25f6c3, 0x58, "cameraHead" },
        // phases: jitter object, immediate, scene size, DLSS branch, generator arguments
        { "phases: mov rbx,[rdi+0x278] (the jitter object)", 0x25f72e, 0x7c, "phases" },
        { "phases: mov edx,-1 (the unbounded phase count)", 0x25f733, 0x08, "phases" },
        { "phases: mov rcx,[rax+0x428] (the scene size qword)", 0x25f73a, 0x2c, "phases" },
        { "phases: cmp r8b,2 (DLSS mode)", 0x25f74f, 0x03, "phases" },
        { "phases: device width [rax+0x8c]", 0x25f766, 0x90, "phases" },
        { "phases: mov r9d,[rbx+0x10] (the frame counter)", 0x25f788, 0x14, "phases" },
        { "phases: lea r8,[rbx+4] (the y pointer)", 0x25f78c, 0x08, "phases" },
        { "phases: mov [rsp+0x20],edx (the phase count argument)", 0x25f790, 0x24, "phases" },
        { "phases: lea rcx,[rsp+0x30] (the opaque argument)", 0x25f795, 0x34, "phases" },
        { "phases: mov rdx,rbx (the x pointer = the jitter object)", 0x25f798, 0xd2, "phases" },
        // strength: extents, amount, scale, doubling, NDC stores
        { "strength: extent0 [rcx+0x700]", 0x25f7b9, 0x08, "strength" },
        { "strength: extent2 [rcx+0x704]", 0x25f7c3, 0x08, "strength" },
        { "strength: jitter object reloaded from [rdi+0x278]", 0x25f7ca, 0x7c, "strength" },
        { "strength: amount [rax+0x78]", 0x25f7e3, 0x7c, "strength" },
        { "strength: scale [rax+0x4c]", 0x25f7f7, 0x50, "strength" },
        { "strength: strength squared", 0x25f800, 0xc8, "strength" },
        { "strength: doubling", 0x25f810, 0xd3, "strength" },
        { "strength: NDC x from [rsp+0x38]", 0x25f81f, 0x3c, "strength" },
        { "strength: NDC y from [rsp+0x3c]", 0x25f819, 0x40, "strength" },
        { "strength: the y sign flip (xorps)", 0x25f821, 0x58, "strength" },
        { "strength: NDC x divided by the width (xmm6)", 0x25f835, 0xcf, "strength" },
        { "strength: NDC stored through [rcx+8]", 0x25f864, 0x0c, "strength" },
        // producer: the three loads, the world camera, the call and the returned object
        { "producer: renderer load into rdi", 0x3740d2, 0x35, "producerCall" },
        { "producer: device load into rbx", 0x3740d9, 0x15, "producerCall" },
        { "producer: mov rcx,[rax+0x3028] (the world camera)", 0x3740e1, 0x2c, "producerCall" },
        { "producer: a write to rcx between it and the call", 0x37410d, 0x4f, "producerCall" },
        { "producer: the call itself (E8)", 0x37411d, 0xe9, "producerCall", true },
        { "producer: mov rcx,[rax] reads the returned object", 0x374130, 0x09, "producerCall" },
        // device size pair
        { "deviceSize: height [rcx+0x90]", 0xd3f7d, 0x94, "deviceSize" },
        { "deviceSize: width [rcx+0x8c]", 0xd3f83, 0x90, "deviceSize" },
        // constants (seams without a Guard field)
        { "NDC sign mask constant (0x80000000 -> 0x81000000)", 0x211e093, 0x81, "seam:jitterSource.ndcSignMask" },
        { "NDC sign mask constant (0x80000000 -> 0)", 0x211e093, 0x00, "seam:jitterSource.ndcSignMask" },
        { "strength floor constant 0.001f", 0x211dd67, 0x3f, "seam:jitterSource.strengthFloor" },
        { "strength floor 0.00122 (inside the old 0.000488..0.00195 window, not the 0.001f the core hard-codes)", 0x211dd66, 0xa0, "seam:jitterSource.strengthFloor" },
    };
    for (const Flip& f : flips)
    {
        auto img = MakeJitterImage(0x11111111);
        REQUIRE(img != nullptr);
        img->Bytes()[f.rva] = f.to;
        const Registry reg = JitterRegistry();
        const ResolvedBuild rb = ResolveImage(*img, reg);
        const auto r = ext::ffxiv::jitter::source::VerifyAnchors(ReaderOf(*img), rb.build.jitterSource, kBase);
        const std::string block = f.block;
        bool refused;
        if (block.rfind("seam:", 0) == 0)
            refused = !rb.Usable(block.substr(5)); // no Guard field holds these: the seam itself must be unusable (P3 gates on it)
        else
        {
            // the block column is ASSERTED: the hooks refuse through the block of the site the byte belongs to, and it is the SHAPE seam of that site
            // that failed (not usable, Guard zero) while its location seam is still found, unless the byte is also in both location anchors
            const auto* site = SiteOfBlock(block);
            REQUIRE(site != nullptr);
            refused = !r.ok && std::strcmp(r.failed, f.block) == 0;
            const bool shapeDead = !rb.Usable(site->shape) && GuardIsZero(rb, site->guardKey);
            const bool sameSeam = std::strcmp(site->location, site->shape) == 0;
            const bool locationOk = sameSeam || (f.locationDies ? !rb.Usable(site->location) : rb.Usable(site->location));
            if (!shapeDead || !locationOk)
                std::printf("FAIL: flipping %s at 0x%x: shape dead %d, location %s (expected %s)\n", f.what, f.rva, shapeDead ? 1 : 0,
                            rb.Usable(site->location) ? "usable" : "dead", f.locationDies ? "dead" : "usable");
            CHECK(shapeDead && locationOk);
        }
        if (!refused)
            std::printf("FAIL: flipping %s at 0x%x to 0x%02x does NOT stop the hooks through block %s (a masked byte?)\n", f.what, f.rva, f.to, f.block);
        else
            std::printf("    refused: %-62s -> %s\n", f.what, block.rfind("seam:", 0) == 0 ? block.c_str() : r.failed);
        CHECK(refused);
    }

    // MASKED bytes may change freely: nothing the consumer reads lives there
    const Flip masked[] = {
        { "head: stack cookie load displacement", 0x25f6a5, 0x00, "" },
        { "head: the final jne displacement (75 0F: 15 bytes of the gate behind the shape, not read by the hooks)", 0x25f6dd, 0x20, "" },
        { "generator: float constant displacement (0.5)", 0x2ebdc2, 0x77, "" },
        { "generator: call to the float-to-int helper", 0x2ebe1d, 0x21, "" },
        { "phases: the call that computes the other modes' phase count", 0x25f774, 0x42, "" },
        { "producer: thread-local slot immediate of mov r13d", 0x374127, 0x07, "" },
    };
    for (const Flip& f : masked)
    {
        auto img = MakeJitterImage(0x11111111);
        REQUIRE(img != nullptr);
        img->Bytes()[f.rva] = f.to;
        const Registry reg = JitterRegistry();
        const ResolvedBuild rb = ResolveImage(*img, reg);
        const auto r = ext::ffxiv::jitter::source::VerifyAnchors(ReaderOf(*img), rb.build.jitterSource, kBase);
        if (!r.ok)
            std::printf("FAIL: the masked byte (%s) at 0x%x stopped the hooks: %s\n", f.what, f.rva, r.failed);
        CHECK(r.ok);
        // masked displacements of RIP loads move the decoded global: those that move a Global/Offset anchor target may only demote
        // that anchor (the second derivation still holds); every location and shape seam stays Found
        for (const char* id : { "jitterSource.generator", "jitterSource.cameraGetter", "jitterSource.cameraHead", "jitterSource.producerCall", "jitterSource.phases",
                                "jitterSource.strength", "jitterSource.generatorShape", "jitterSource.cameraGetterShape", "jitterSource.cameraHeadShape",
                                "jitterSource.producerCallShape", "jitterSource.phasesShape", "jitterSource.strengthShape" })
            CHECK(Get(rb, id).status == SeamStatus::Found);
    }
}

// The displacements the shapes mask are NOT unchecked: the globals are derived from them by two or three independent loads, and a displacement
// that moves one load's target makes the derivations disagree: Conflict (inert, the field stays zero), never a quietly wrong global.
//
// But the hooks read memory THROUGH the three globals and VerifyAnchors does not look at them: with ONE moved displacement the real VerifyAnchors
// still PASSES for the settings and the renderer global (the field is zero, ReadInputs would read base + 0, the PE header, as a pointer); only the
// device global is caught, indirectly (deviceSize is tied to it with TargetIs). This test RECORDS that result for every global (so a change of
// VerifyAnchors in P3 shows up here) and applies the gate the header of AnchorsJitter.h requires from P3: a zero Rva of any of the three refuses.
// Core files are not changed by this group.
bool GateRefusesZeroGlobal(const ResolvedBuild& rb)
{
    for (const auto& g : ext::ffxiv::resolve::kJitterGlobals)
    {
        const FieldRow* row = FindField(g.rvaKey);
        if (row == nullptr || *FieldU32(rb.build, *row) == 0)
            return true;
    }
    return false;
}

// the stricter form of the same gate: a global whose seam is not usable refuses (also when its Rva was filled from the table on a known build)
bool GateRefusesUnusableGlobal(const ResolvedBuild& rb)
{
    for (const auto& g : ext::ffxiv::resolve::kJitterGlobals)
    {
        if (!rb.Usable(g.seam))
            return true;
    }
    return false;
}

void TestMovedDisplacementIsAConflict()
{
    struct Case
    {
        const char* what;
        std::uint32_t rva;
        std::uint8_t to;
        const char* seam;
        const char* rvaKey;       // the Rva field the Conflict leaves at zero
        bool verifyAnchorsAlone;  // what the REAL VerifyAnchors says today (true = the hooks would install)
    };
    for (const Case& c : { Case { "settings load in the camera head", 0x25f6c7, 0x11, "jitterSource.settingsPointer", "jitterSource.settingsPointerRva", true },
                           Case { "renderer load in the strength block", 0x25f7a1, 0x02, "jitterSource.rendererPointer", "jitterSource.rendererPointerRva", true },
                           Case { "device load in the phases block", 0x25f755, 0x01, "jitterSource.devicePointer", "jitterSource.devicePointerRva", false } })
    {
        auto img = MakeJitterImage(0x11111111);
        REQUIRE(img != nullptr);
        img->Bytes()[c.rva] = c.to;
        const ResolvedBuild rb = ResolveImage(*img, JitterRegistry());
        const SeamResult& s = Get(rb, c.seam);
        if (s.status != SeamStatus::Conflict)
            std::printf("FAIL: %s: %s is %s (%s)\n", c.what, c.seam, StatusName(s.status), s.reason.c_str());
        CHECK(s.status == SeamStatus::Conflict && s.rva == 0);
        const FieldRow* row = FindField(c.rvaKey);
        REQUIRE(row != nullptr);
        CHECK(*FieldU32(rb.build, *row) == 0);

        // the record: the real VerifyAnchors over the resolved build
        const auto r = ext::ffxiv::jitter::source::VerifyAnchors(ReaderOf(*img), rb.build.jitterSource, kBase);
        if (r.ok != c.verifyAnchorsAlone)
            std::printf("FAIL: %s: VerifyAnchors alone %s, the record says %s (P3 changed it? update this row and the header gate)\n", c.what,
                        r.ok ? "passes" : "refuses", c.verifyAnchorsAlone ? "passes" : "refuses");
        CHECK(r.ok == c.verifyAnchorsAlone);
        std::printf("    moved displacement: %-38s %-30s Conflict, Rva 0, VerifyAnchors alone: %s\n", c.what, c.seam, r.ok ? "PASSES (hooks would install)" : "refuses");

        // the gate the header requires: refuses in all three cases, on top of (or instead of) VerifyAnchors, in both forms
        CHECK(GateRefusesZeroGlobal(rb) && GateRefusesUnusableGlobal(rb));
    }
    // KNOWN build (build C's stamp and size, the table passed): the globals are not failClosed, so a Conflict global is filled FROM THE TABLE: the
    // Rva is non-zero (the table value), the seam is not usable. The zero-Rva check alone passes it, only `Usable(seam)` refuses: the two forms of
    // the gate are not equivalent (header, gate rule 2).
    {
        auto img = MakeJitterImage(kStampC);
        REQUIRE(img != nullptr);
        img->Bytes()[0x25f6c7] = 0x11; // the settings load in the camera head
        ResolveOptions opts;
        opts.knownTable = &ext::ffxiv::kCurrentBuild;
        const ResolvedBuild rb = ResolveImage(*img, JitterRegistry(), opts);
        const SeamResult& s = Get(rb, "jitterSource.settingsPointer");
        CHECK(rb.knownBuild && s.status == SeamStatus::Conflict && s.fellBackToTable && !rb.Usable("jitterSource.settingsPointer"));
        CHECK(rb.build.jitterSource.settingsPointerRva == ext::ffxiv::kCurrentBuild.jitterSource.settingsPointerRva && rb.build.jitterSource.settingsPointerRva != 0);
        CHECK(!GateRefusesZeroGlobal(rb));    // the minimum does NOT catch it on a known build ...
        CHECK(GateRefusesUnusableGlobal(rb)); // ... the Usable form does
    }
    // the unmodified image passes the gate (a gate that always refuses would pass the loop above)
    {
        auto img = MakeJitterImage(0x11111111);
        REQUIRE(img != nullptr);
        const ResolvedBuild rb = ResolveImage(*img, JitterRegistry());
        CHECK(!GateRefusesZeroGlobal(rb) && !GateRefusesUnusableGlobal(rb));
        CHECK(ext::ffxiv::jitter::source::VerifyAnchors(ReaderOf(*img), rb.build.jitterSource, kBase).ok);
    }
    // ... and the device reader, tied to the device global with TargetIs, fails closed when that global is in doubt: its guard stays zero
    {
        auto img = MakeJitterImage(0x11111111);
        REQUIRE(img != nullptr);
        img->Bytes()[0x25f755] = 0x01;
        const ResolvedBuild rb = ResolveImage(*img, JitterRegistry());
        CHECK(!rb.Usable("jitterSource.deviceSize") && rb.build.jitterSource.deviceSizeRva == 0 && rb.build.jitterSource.deviceSizeInstructions[0] == 0);
        const auto r = ext::ffxiv::jitter::source::VerifyAnchors(ReaderOf(*img), rb.build.jitterSource, kBase);
        CHECK(!r.ok);
    }
}
} // namespace

int main(int argc, char** argv)
{
    if (argc > 1 && std::string(argv[1]) == "--dump")
    {
        Dump();
        return 0;
    }
    TestPins();
    TestResolveOnCommittedBytes();
    TestSafetyGap();
    TestMovedDisplacementIsAConflict();
    return TEST_RESULT("resolve_jitter_anchors");
}
