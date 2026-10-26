// P2a: the quality anchors against the PREVIOUS build (B, stamp 0x6ac3dc96), using only rows recorded from the B file (confidence "B-read").
//   * the settings block was recorded in full (37 bytes): the anchor and everything derived from it resolve on a B image, to B's addresses
//     (block 0x374347, settings global 0x28f9470) and to B's offsets;
//   * the three function anchors (querySize, rendererUpdate, resizeCallbacks) are LONGER than the recorded prefixes: they are tested for prefix
//     compatibility, not resolved: every recorded B byte matches the pattern, every byte that changed between B and C is a masked byte;
//   * a wrong-build control: B's settings block planted at C's address is refused by TargetIn(Bss) (its global lies in C's initialised data);
//   * what is NOT evidence is said here and in the report: the bytes beyond the recorded prefixes, the bss globals (no bytes at all) and the
//     camera sites have no B bytes; nothing derived is planted.
#include "resolve_quality_util.h"

using namespace qt;

namespace
{
bool PrefixMatches(const Pattern& p, const std::vector<std::uint8_t>& bytes)
{
    if (bytes.size() > p.size())
        return false;
    for (std::size_t i = 0; i < bytes.size(); ++i)
    {
        if (((bytes[i] ^ p.bytes[i]) & p.mask[i]) != 0)
            return false;
    }
    return true;
}

std::vector<std::uint8_t> SiteBytes(const char* name, std::size_t from, std::size_t n)
{
    const auto all = FromHex(SiteNamed(name).hex);
    if (from + n > all.size())
    {
        std::printf("FAIL: site %s is too short\n", name);
        ++g_failures;
        return {};
    }
    return std::vector<std::uint8_t>(all.begin() + static_cast<std::ptrdiff_t>(from), all.begin() + static_cast<std::ptrdiff_t>(from + n));
}

struct PrefixCase
{
    const char* seam;
    std::size_t anchor;
    const Row* b;
    const char* cSite;
    std::size_t cFrom;
    std::vector<std::size_t> changed; // the indices at which B and C differ (the notes of the fixture: displacement bytes only)
    bool longer;                      // the pattern is longer than the recorded prefix
};

void TestPrefixCompatibility()
{
    const Registry reg = QualityRegistry();
    const PrefixCase cases[] = {
        { "chara.accessor", 0, &kCharaAccessorB, "chara.accessor", 0, { 3, 4, 5 }, false },
        { "quality.settingsInstructions", 0, &kSettingsB, "context.prelude+settingsBlock", 0x47, { 3, 4, 5 }, false },
        { "quality.querySize", 0, &kQuerySizeB, "querySize", 0, { 24, 25, 26 }, true },
        { "quality.rendererUpdate", 0, &kRendererUpdateB, "rendererUpdate.entry", 0, { 14, 15, 16 }, true },
        { "quality.resizeCallbacks", 1, &kResizeCallbacksB, "resizeEntry", 0, {}, true },
        // the SHAPE seams hold the strict entry text as their one anchor: B's recorded prefixes must be compatible with it as well, and the bytes that
        // differ between the builds are wildcards of it too (only displacements move: a build that only moved code keeps the shape)
        { "quality.querySizeShape", 0, &kQuerySizeB, "querySize", 0, { 24, 25, 26 }, true },
        { "quality.rendererUpdateShape", 0, &kRendererUpdateB, "rendererUpdate.entry", 0, { 14, 15, 16 }, true },
        { "quality.resizeCallbacksShape", 0, &kResizeCallbacksB, "resizeEntry", 0, {}, true },
    };
    for (const PrefixCase& c : cases)
    {
        const SeamSpec* seam = FindSeam(reg, c.seam);
        REQUIRE(seam != nullptr && c.anchor < seam->anchors.size());
        Pattern p;
        REQUIRE(ParsePattern(seam->anchors[c.anchor].pattern, p));
        const auto b = FromHex(c.b->bytes);
        const auto cc = SiteBytes(c.cSite, c.cFrom, b.size());
        REQUIRE(!b.empty() && cc.size() == b.size());
        CHECK(std::string(c.b->confidence) == "B-read" && std::string(c.b->mask).empty()); // recorded in full
        CHECK(PrefixMatches(p, b));  // B's bytes are compatible with the pattern
        CHECK(PrefixMatches(p, cc)); // and C's
        CHECK((p.size() > b.size()) == c.longer);
        // the bytes that differ between the two builds are exactly the documented ones, and every one is a wildcard of the pattern
        std::vector<std::size_t> diff;
        for (std::size_t i = 0; i < b.size(); ++i)
        {
            if (b[i] != cc[i])
                diff.push_back(i);
        }
        if (diff != c.changed)
            std::printf("FAIL: %s: B and C differ at %zu bytes, expected %zu\n", c.seam, diff.size(), c.changed.size());
        CHECK(diff == c.changed);
        for (std::size_t i : diff)
            CHECK(p.mask[i] == 0);
    }
}

void TestSettingsOnBuildB()
{
    const Registry reg = QualityRegistry();
    auto img = MakeBImage();
    REQUIRE(img != nullptr);
    CHECK(PlantBRow(*img, kSettingsB));

    LogCapture log;
    ResolveOptions opts;
    opts.sink = log.Sink();
    const ResolvedBuild rb = ResolveImage(*img, reg, opts);
    CHECK(rb.timeDateStamp == kStampB && rb.sizeOfImage == kSizeB && !rb.knownBuild);

    const SeamResult& site = Get(rb, "quality.settingsInstructions");
    CHECK(site.status == SeamStatus::Found && site.rva == 0x374347 && site.anchors[0].patternHits == 1);
    CHECK(0x3745d7 - site.rva == 0x290); // the B -> C shift the notes record

    // the global, decoded from B's own displacement bytes: only the settings block records it on B, so it is a single-anchor answer
    const SeamResult& g = Get(rb, "quality.settingsPointer");
    CHECK(g.status == SeamStatus::FoundSingleAnchor && g.rva == kSettingsGlobalB && g.rva == 0x28f9470);
    CHECK(g.anchors[0].outcome == AnchorOutcome::Unique && g.anchors[1].outcome == AnchorOutcome::Zero &&
          g.anchors[2].outcome == AnchorOutcome::DependencyMissing);
    CHECK(0x290e470 - g.rva == 0x15000); // +0x15000 for every global of the quality group

    // the offsets B's instructions carry are the same as C's
    for (const char* id : { "quality.settingsModeOffset", "quality.settingsQualityOffset", "quality.settingsDlaaOffset" })
        CHECK(Get(rb, id).status == SeamStatus::FoundSingleAnchor);
    CHECK(Get(rb, "quality.settingsSelectorOffset").status == SeamStatus::Found);
    CHECK(rb.build.nativeQuality.settingsModeOffset == 0x54 && rb.build.nativeQuality.settingsQualityOffset == 0x44 &&
          rb.build.nativeQuality.settingsDlaaOffset == 0x45 && rb.build.nativeQuality.settingsSelectorOffset == 0x55);

    // the fields: B's addresses and B's bytes (the guard is read from the image at the resolved address: B's displacement included)
    CHECK(rb.build.nativeQuality.settingsInstructionsRva == 0x374347 && rb.build.nativeQuality.settingsPointerRva == 0x28f9470);
    const auto bBytes = FromHex(kSettingsB.bytes);
    REQUIRE(bBytes.size() == rb.build.nativeQuality.settingsInstructions.size());
    CHECK(std::memcmp(bBytes.data(), rb.build.nativeQuality.settingsInstructions.data(), bBytes.size()) == 0);
    CHECK(rb.build.nativeQuality.settingsInstructions != ext::ffxiv::kCurrentBuild.nativeQuality.settingsInstructions); // not C's bytes

    // nothing else was recorded on B for this group: nothing else resolves, no field leaks the C value (the 0x25f610-style reuse trap)
    for (const char* id : { "quality.querySize", "quality.rendererUpdate", "quality.resizeCallbacks", "quality.managerPointer", "quality.devicePointer",
                            "quality.contextResolver", "quality.cacheCompare", "camera.global", "camera.getter", "chara.accessor",
                            "quality.querySizeShape", "quality.rendererUpdateShape", "quality.resizeCallbacksShape" })
        CHECK(Get(rb, id).status == SeamStatus::NotFound);
    CHECK(rb.build.nativeQuality.querySizeRva == 0 && rb.build.nativeQuality.rendererUpdateRva == 0 && rb.build.jitter.cameraGlobalRva == 0);
    // and no Guard field of the three hooked functions holds anything (no C bytes, no live bytes of some address)
    for (const char* key : { "nativeQuality.querySizeInstructions", "nativeQuality.rendererUpdateInstructions", "nativeQuality.resizeCallbacksInstructions" })
        CHECK(AllZero(GuardOf(rb.build, key)));
    // the WARN lines: the single-anchor seams are WARNs
    bool sawSingle = false;
    for (const auto& l : log.lines)
        sawSingle = sawSingle || (l.second.find("quality.settingsPointer FoundSingleAnchor") != std::string::npos && l.first == LogLevel::Warn);
    CHECK(sawSingle);
}

// B's settings block at C's address (a table of one build read against the other build's layout): the block matches (the displacement is
// masked) but the global it decodes lies in C's initialised data, so TargetIn(Bss) refuses it
void TestWrongBuildControl()
{
    const Registry reg = QualityRegistry();
    auto img = MakeQualityC();
    REQUIRE(img != nullptr);
    Row asC = kSettingsB;
    asC.rva = 0x3745d7;
    CHECK(PlantBRow(*img, asC));
    const ResolvedBuild rb = ResolveImage(*img, reg);
    CHECK(Get(rb, "quality.settingsInstructions").status == SeamStatus::Found && Get(rb, "quality.settingsInstructions").rva == 0x3745d7);
    const SeamResult& g = Get(rb, "quality.settingsPointer");
    CHECK(g.status == SeamStatus::NotFound);
    CHECK(g.anchors[0].patternHits == 1 && g.anchors[0].candidates == 0);
    CHECK(rb.build.nativeQuality.settingsPointerRva == 0);
    // a known-build C table fills the unresolved global (M11: the table is verified against THIS image), never with B's value
    ResolveOptions opts;
    opts.knownTable = &ext::ffxiv::kCurrentBuild;
    const ResolvedBuild known = ResolveImage(*img, reg, opts);
    CHECK(known.knownBuild && known.build.nativeQuality.settingsPointerRva == 0x290e470 && Get(known, "quality.settingsPointer").fellBackToTable);
}
} // namespace

int main()
{
    TestPrefixCompatibility();
    TestSettingsOnBuildB();
    TestWrongBuildControl();
    return TEST_RESULT("resolve_quality_b_fixture");
}
