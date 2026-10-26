// The jitter-source group against the PREVIOUS build (B, stamp 0x6ac3dc96), on the bytes that were really recorded from it.
//
// What exists for B (recorded rows, confidence "B-read" or "B-read-partial" only, see old_build_b_jitter.h):
//   * the generator's first 20 bytes at B 0x2ebae0,
//   * 48 bytes of the phases block at B 0x25f6a1 with 4 unknown displacement bytes (maskB),
//   * the 5-byte `mov edx,-1` at B 0x25f6b2.
// Everything else of the group has NO B bytes (getter, camera head, strength, deviceSize, producer call: RVA-only or not recorded), and the
// derived-from-C / derived-from-B-RVAs rows are refused. So the B evidence here is a PREFIX evidence and nothing more: the strict patterns of
// the generator and the phases block START with exactly the recorded bytes. This test says nothing about the rest of those blocks, and it
// invents no B evidence: with every B row planted the registry resolves no seam of the group.
#include "resolve_jitter_image.h"

using namespace jt;

namespace
{
const AnchorSpec& OnlyAnchor(const Registry& reg, const char* seam)
{
    static const AnchorSpec none;
    const SeamSpec* s = FindSeam(reg, seam);
    if (s == nullptr || s->anchors.empty())
    {
        std::printf("FAIL: no seam %s\n", seam);
        ++g_failures;
        return none;
    }
    return s->anchors[0];
}

bool PrefixMatches(const DenseImage& img, std::uint32_t rva, const std::string& pattern, std::size_t tokens)
{
    Pattern p;
    if (!ParsePattern(PrefixOf(pattern, tokens), p))
        return false;
    std::vector<std::uint8_t> bytes(p.size());
    return img.Read(rva, bytes.data(), bytes.size()) && MatchAt(bytes.data(), p);
}

void TestFixtureDiscipline()
{
    CHECK(std::string(kGeneratorB.confidence) == "B-read" && std::string(kPhasesB.confidence) == "B-read-partial" &&
          std::string(kFallbackImmediateB.confidence) == "B-read");
    auto img = MakeBImage();
    REQUIRE(img != nullptr);
    CHECK(PlantBRow(*img, kGeneratorB) && PlantBRow(*img, kPhasesB) && PlantBRow(*img, kFallbackImmediateB));

    // maskB: the four unknown displacement bytes of the phases row (index 3..6) are NOT planted
    std::uint8_t unknown[4] = { 0xff, 0xff, 0xff, 0xff };
    REQUIRE(img->Read(0x25f6a1 + 3, unknown, 4));
    CHECK(unknown[0] == 0 && unknown[1] == 0 && unknown[2] == 0 && unknown[3] == 0);
    std::uint8_t known = 0;
    REQUIRE(img->Read(0x25f6a1 + 10, &known, 1));
    CHECK(known == 0x48 && FromHex(kPhasesB.bytes)[10] == 0x48); // an unmasked byte is

    // the derived tiers are refused: the camera getter's bytes carried over from C to B are not evidence
    const Row derivedGetter = { "jitterSource.cameraGetter", 0x25f590, "488b8178020000c3", "", "derived-from-C" };
    const Row derivedCall = { "jitterSource.strength", 0x25f719, "e8c2c30800", "", "derived-from-B-RVAs" };
    CHECK(!PlantBRow(*img, derivedGetter) && !PlantBRow(*img, derivedCall));
    std::uint8_t probe[8] = {};
    REQUIRE(img->Read(0x25f590, probe, sizeof(probe)));
    CHECK(probe[0] == 0 && probe[7] == 0);
}

void TestPrefixesOfTheStrictPatterns()
{
    const Registry reg = JitterRegistry();
    auto img = MakeBImage();
    REQUIRE(img != nullptr);
    REQUIRE(PlantBRow(*img, kGeneratorB) && PlantBRow(*img, kPhasesB) && PlantBRow(*img, kFallbackImmediateB));

    // generator: the 20 recorded bytes are the first 20 bytes of the strict pattern, all fixed (no wildcard hides a B byte), and the same
    // 20 bytes are at C 0x2ebd90 (prologue unchanged B to C: only the address moved, +0x2b0)
    const std::string generator = OnlyAnchor(reg, "jitterSource.generatorShape").pattern;
    CHECK(Tokens(PrefixOf(generator, 20)).size() == 20);
    for (const std::string& t : Tokens(PrefixOf(generator, 20)))
        CHECK(t != "??");
    CHECK(PrefixMatches(*img, 0x2ebae0, generator, 20));
    CHECK(ToHex(std::vector<std::uint8_t>(Materialize(PrefixOf(generator, 20)))) == std::string(kGeneratorB.bytes));
    CHECK(std::string(kGeneratorC.bytes).rfind(kGeneratorB.bytes, 0) == 0); // the C row starts with the B row
    CHECK(0x2ebd90 - 0x2ebae0 == 0x2b0);
    // not one byte further: the recorded B bytes end at 20 (a 21st token must not be claimed by this test)
    CHECK(FromHex(kGeneratorB.bytes).size() == 20);

    // phases: the loose anchor (without its SameFunctionAs predicate, the camera head has no B bytes) finds the recorded block, once
    AnchorSpec loose = OnlyAnchor(reg, "jitterSource.phases");
    loose.predicates.clear();
    Registry t;
    t.seams.push_back({ "t.phases", SeamKind::CodeSite, { loose } });
    const ResolvedBuild rb = ResolveImage(*img, t);
    const SeamResult& s = Get(rb, "t.phases");
    CHECK(s.status == SeamStatus::Found && s.rva == 0x25f6a1 && s.anchors[0].patternHits == 1);
    CHECK(rb.timeDateStamp == kStampB && !rb.knownBuild);
    CHECK(0x25f721 - 0x25f6a1 == 0x80); // B to C: +0x80

    // the strict phases pattern: its first 48 tokens match the recorded bytes exactly where they were recorded; tokens 3..6 are wildcards,
    // which is precisely the unrecorded part (maskB)
    const std::string phases = OnlyAnchor(reg, "jitterSource.phasesShape").pattern;
    const auto tokens = Tokens(phases);
    REQUIRE(tokens.size() == 120);
    for (std::size_t i = 0; i < 48; ++i)
        CHECK((tokens[i] == "??") == (i >= 3 && i <= 6));
    CHECK(PrefixMatches(*img, 0x25f6a1, phases, 48));

    // mov edx,-1: at +0x11 of the block in B (0x25f6b2) and in C (0x25f732), the same five bytes, tokens 17..21 of the pattern
    CHECK(tokens[17] == "BA" && tokens[18] == "FF" && tokens[19] == "FF" && tokens[20] == "FF" && tokens[21] == "FF");
    CHECK(0x25f6b2 - 0x25f6a1 == 0x11 && 0x25f732 - 0x25f721 == 0x11);
    std::uint8_t imm[5] = {};
    REQUIRE(img->Read(0x25f6b2, imm, sizeof(imm)));
    CHECK(imm[0] == 0xba && imm[1] == 0xff && imm[2] == 0xff && imm[3] == 0xff && imm[4] == 0xff);
    CHECK(std::string(kFallbackImmediateB.bytes) == "baffffffff");
}

void TestNothingIsInventedForB()
{
    // every B row planted: the registry of the group resolves NO seam on B (the blocks beyond the recorded prefixes have no B bytes), and no Rva
    // or guard field holds a C value (the reuse trap: C 0x25f610 is the camera getter, B 0x25f610 was the camera update)
    const Registry reg = JitterRegistry();
    auto img = MakeBImage();
    REQUIRE(img != nullptr);
    REQUIRE(PlantBRow(*img, kGeneratorB) && PlantBRow(*img, kPhasesB) && PlantBRow(*img, kFallbackImmediateB));
    const ResolvedBuild rb = ResolveImage(*img, reg);
    for (const SeamResult& s : rb.seams)
    {
        if (s.status != SeamStatus::NotFound)
            std::printf("FAIL: %s is %s on B (%s)\n", s.id.c_str(), StatusName(s.status), s.reason.c_str());
        CHECK(!rb.Usable(s.id) && !s.hasValue);
    }
    const auto& j = rb.build.jitterSource;
    CHECK(j.generatorRva == 0 && j.cameraGetterRva == 0 && j.cameraHeadRva == 0 && j.phasesRva == 0 && j.strengthRva == 0 && j.deviceSizeRva == 0);
    CHECK(j.producerCallRva == 0 && j.producerReturnRva == 0 && j.generatorCallReturnRva == 0);
    CHECK(j.settingsPointerRva == 0 && j.rendererPointerRva == 0 && j.devicePointerRva == 0);
    CHECK(j.generatorInstructions[0] == 0 && j.phasesInstructions[0] == 0 && j.cameraGetterInstructions[0] == 0);
    CHECK(!rb.knownBuild && rb.timeDateStamp == kStampB);

    // and a table of C handed to a B image is ignored (the identity gate): not one table value appears
    ResolveOptions opts;
    opts.knownTable = &ext::ffxiv::kCurrentBuild;
    const ResolvedBuild withTable = ResolveImage(*img, reg, opts);
    CHECK(!withTable.knownBuild && withTable.build.jitterSource.generatorRva == 0 && withTable.build.jitterSource.cameraGetterRva == 0);
}
} // namespace

int main()
{
    TestFixtureDiscipline();
    TestPrefixesOfTheStrictPatterns();
    TestNothingIsInventedForB();
    return TEST_RESULT("resolve_jitter_b_fixture");
}
