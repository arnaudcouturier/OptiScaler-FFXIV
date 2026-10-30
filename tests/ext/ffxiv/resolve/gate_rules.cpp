// The gate (ext/ffxiv/resolve/Gate.h) on synthetic ResolvedBuilds and on real ResolveImage results. Always on, no executable needed.
//
// One test (or more) per rule of the gate checklist:
//   * the exe-name check and the Group 1 rule (any ffxiv_dx11.exe), the rollback key (exactly the pre-P3 rules), the unknown-build key;
//   * ONE RULE for every hook site: Usable(location) && Usable(shape) on every build, never UsableOrTable for a shape (including a REAL
//     ResolveImage run where a table-filled location still yields an unusable shape);
//   * quality: the `entry` anchor, plain Found for contextCacheOffset, every bound seam, the snapshot bound;
//   * jitter source: sites, the three globals (a table-filled Conflict global refuses), strengthFloor / ndcSignMask / cameraGlobal, zero fields;
//   * companion: location + shape, batch32Shape directly, callerEpilogue and nativeContextOffset on an unknown build, header all-or-nothing + bounds;
//   * CharaView, EntryPatched (distinct from NotFound), the four cross-group checks, the not-self-healing line, the log block.
// The gate runs on synthetic ResolvedBuilds with every seam Found (AllFound) that a test then breaks one seam at a time.
#include "gate_fixture.h"

#include "ext/ffxiv/ActiveBuild.h"
#include "ext/ffxiv/resolve/Entry.h"
#include "ext/ffxiv/resolve/Gate.h"
#include "ext/ffxiv/resolve/GateLog.h"
#include "ext/ffxiv/resolve/Registry.h"
#include "ext/ffxiv/resolve/ResolveCache.h"
#include "ext/ffxiv/resolve/Resolver.h"

#include "resolve_jitter_image.h"

#include <algorithm>
#include <cstdio>
#include <set>
#include <cstring>
#include <stdexcept>
#include <string>

using namespace rt;
using namespace ext::ffxiv;
using namespace gf;

namespace
{
// ---- exe name ----------------------------------------------------------------------------------------------------------------------------

void TestExeName()
{
    CHECK(IsFfxivExeName(std::string_view("ffxiv_dx11.exe")));
    CHECK(IsFfxivExeName(std::string_view("FFXIV_DX11.EXE")));
    CHECK(IsFfxivExeName(std::string_view("Ffxiv_Dx11.exe")));
    CHECK(IsFfxivExeName(std::string_view("C:\\Games\\game\\ffxiv_dx11.exe")));
    CHECK(IsFfxivExeName(std::string_view("/games/ffxiv_dx11.exe")));
    CHECK(IsFfxivExeName(std::wstring_view(L"C:\\Games\\FFXIV\\game\\FFXIV_DX11.exe")));
    CHECK(IsFfxivExeName(std::wstring_view(L"ffxiv_dx11.exe")));
    CHECK(!IsFfxivExeName(std::string_view("")));
    CHECK(!IsFfxivExeName(std::string_view("ffxiv.exe")));
    CHECK(!IsFfxivExeName(std::string_view("ffxiv_dx11.exe.bak")));
    CHECK(!IsFfxivExeName(std::string_view("xffxiv_dx11.exe")));
    CHECK(!IsFfxivExeName(std::string_view("ffxiv_dx11.exe ")));
    CHECK(!IsFfxivExeName(std::string_view("a/ffxiv_dx11.exe/b")));
    CHECK(!IsFfxivExeName(std::string_view("ffxiv_dx11.ex")));
    CHECK(!IsFfxivExeName(std::wstring_view(L"notepad.exe")));
}

// ---- the three modes ----------------------------------------------------------------------------------------------------------------------

void TestGroup1AndModes()
{
    // not the game: nothing at all, whatever else is true
    {
        const ResolvedBuild rb = AllFound(true);
        const GateReport g = Run(&rb, Settings(false, true, false), false, true);
        CHECK(AllGroup1(g, false) && AllGroup2(g, false));
        const GateReport l = Run(&rb, Settings(true), false, true);
        CHECK(AllGroup1(l, false) && AllGroup2(l, false));
    }
    // Group 1 = the exe name and nothing else: no resolve at all, an unknown build, the key off
    {
        const GateReport g = Run(nullptr, Settings(), true, false);
        CHECK(AllGroup1(g, true));
        CHECK(AllGroup2(g, false)); // no resolve: fail open
        ResolvedBuild unknown = AllFound(false);
        for (SeamResult& s : unknown.seams)
            s.status = SeamStatus::NotFound; // the resolver found nothing at all
        const GateReport u = Run(&unknown, Settings(), true, false);
        CHECK(AllGroup1(u, true) && AllGroup2(u, false));
        // DecideGroup1 is the same answer for one feature
        CHECK(DecideGroup1(Feature::Lighting, true, false, Settings()).enabled);
        CHECK(!DecideGroup1(Feature::Lighting, false, false, Settings()).enabled);
        CHECK(!DecideGroup1(Feature::Lighting, true, false, Settings(true)).enabled);
        CHECK(DecideGroup1(Feature::Lighting, true, true, Settings(true)).enabled);
    }
    // the rollback key: exactly the pre-P3 rules (exact build: everything on and the old guards decide; any other build: nothing runs)
    {
        const GateReport exact = Run(nullptr, Settings(true), true, true);
        CHECK(exact.legacy && exact.known && AllGroup1(exact, true) && AllGroup2(exact, true));
        const GateReport other = Run(nullptr, Settings(true), true, false);
        CHECK(other.legacy && !other.known && AllGroup1(other, false) && AllGroup2(other, false));
        // the other two keys are ignored under the rollback key
        const GateReport ignoredKeys = Run(nullptr, Settings(true, true, true), true, false);
        CHECK(AllGroup1(ignoredKeys, false) && AllGroup2(ignoredKeys, false) && !ignoredKeys.forcedUnknown);
        // a resolved build is not consulted in legacy mode: a broken one cannot turn an exact build off
        ResolvedBuild broken = AllFound(true);
        for (SeamResult& s : broken.seams)
            s.status = SeamStatus::Conflict;
        CHECK(AllGroup2(Run(&broken, Settings(true), true, true), true));
        // and a perfect one cannot turn an unknown build on
        const ResolvedBuild perfect = AllFound(false);
        CHECK(AllGroup2(Run(&perfect, Settings(true, true), true, false), false));
    }
    // the default design: a known build runs everything; an unknown build runs Group 1, Group 2 only with the key
    {
        const ResolvedBuild known = AllFound(true);
        const GateReport k = Run(&known, Settings());
        CHECK(k.known && !k.legacy && AllGroup1(k, true) && AllGroup2(k, true));
        const ResolvedBuild unknown = AllFound(false);
        const GateReport off = Run(&unknown, Settings());
        CHECK(!off.known && AllGroup1(off, true) && AllGroup2(off, false));
        for (const Feature f : kGroup2)
            CHECK(Contains(off.Reason(f), "FfxivUnknownBuildHooks") && Contains(off.Reason(f), "seams allow it"));
        const GateReport on = Run(&unknown, Settings(false, true));
        CHECK(AllGroup1(on, true) && AllGroup2(on, true));
        // the key changes nothing on a known build
        CHECK(AllGroup2(Run(&known, Settings(false, true)), true));
    }
    // the debug key: the resolver ran without the table (unknown). Group 2 then also needs the unknown-build key.
    {
        const ResolvedBuild forced = AllFound(false);
        const GateReport noKey = Run(&forced, Settings(false, false, true));
        CHECK(noKey.forcedUnknown && AllGroup1(noKey, true) && AllGroup2(noKey, false));
        const GateReport withKey = Run(&forced, Settings(false, true, true));
        CHECK(withKey.forcedUnknown && AllGroup1(withKey, true) && AllGroup2(withKey, true));
        // the resolver must have treated the build as unknown: a "forced" run that came back known is refused
        const ResolvedBuild inconsistent = AllFound(true);
        const GateReport bad = Run(&inconsistent, Settings(false, true, true));
        CHECK(AllGroup1(bad, true) && AllGroup2(bad, false) && Contains(bad.Reason(Feature::QualityHooks), "FfxivForceUnknownBuild"));
    }
}

void TestFailOpen()
{
    // The UNKNOWN-build halves of the fail-open cases (on the exact known build an infrastructure failure falls back to the legacy decision instead:
    // TestInfrastructureFailureFallsBack). No resolve (the image could not be read, startup not reached): Group 2 off, Group 1 on
    const GateReport none = Run(nullptr, Settings(false, true), true, false);
    CHECK(AllGroup1(none, true) && AllGroup2(none, false));
    CHECK(Contains(none.Reason(Feature::JitterSource), "fail open"));
    // a resolver that reported errors (a broken registry, an exception): every seam NotFound, Group 2 off
    ResolvedBuild failed = AllFound(false);
    failed.registryErrors.push_back("resolver exception: bad_alloc");
    const GateReport f = Run(&failed, Settings(false, true), true, false);
    CHECK(AllGroup1(f, true) && AllGroup2(f, false) && Contains(f.Reason(Feature::CompanionNative), "bad_alloc"));
    // an empty result (no seams at all)
    ResolvedBuild empty;
    CHECK(AllGroup2(Run(&empty, Settings(false, true), true, false), false));
    // the real resolver on an image that has nothing in it: every seam NotFound, a known build too (the table is only the cross-check)
    auto img = MakeImage();
    const ResolvedBuild real = ResolveImage(*img, Reg());
    const GateReport r = Run(&real, Settings(false, true));
    CHECK(AllGroup2(r, false));

    // a BROKEN registry (a pattern that does not parse): the resolver resolves nothing and reports the error; the gate fails open, key or not
    Registry broken = Reg();
    broken.seams.front().anchors.front().pattern = "ZZ not a pattern";
    ResolveOptions known;
    known.knownTable = &kCurrentBuild;
    const ResolvedBuild b = ResolveImage(*img, broken, known);
    CHECK(!b.registryErrors.empty() && b.Count(SeamStatus::NotFound) == broken.seams.size());
    GateInput in;
    in.exeIsFfxiv = true;
    in.exactKnownBuild = false;
    in.rb = &b;
    in.registry = &broken;
    const GateReport gb = Decide(in, Settings(false, true));
    CHECK(AllGroup1(gb, true) && AllGroup2(gb, false) && Contains(gb.Reason(Feature::QualityHooks), "resolver failed"));

    // a resolver that THROWS (the image reader throws): ResolveCache turns it into an all-NotFound result with the exception text; the gate fails open
    struct Throwing : ImageView
    {
        explicit Throwing(const DenseImage& i) { _info = i.Info(); }
        bool Read(std::uint32_t, void*, std::size_t) const override { throw std::runtime_error("boom"); }
    } bad(*img);
    ResolveCache cache;
    const ResolvedBuild& thrown = cache.Get(bad, Reg(), known);
    CHECK(!thrown.registryErrors.empty() && thrown.Count(SeamStatus::NotFound) == Reg().seams.size());
    in.rb = &thrown;
    in.registry = &Reg();
    const GateReport gt = Decide(in, Settings(false, true));
    CHECK(AllGroup1(gt, true) && AllGroup2(gt, false) && Contains(gt.Reason(Feature::JitterSource), "boom"));
    // and the published state of such a failure keeps the game unaffected: the build the consumers read has every Rva / Guard field zero
    CHECK(thrown.build.jitterSource.generatorRva == 0 && thrown.build.nativeQuality.querySizeRva == 0 && thrown.build.companion.batch16Rva == 0);
}

// ---- the registry-driven rules ------------------------------------------------------------------------------------------------------------

struct Site
{
    Feature feature;
    std::string location, shape;
};

// The hook sites as LITERALS (location, shape): a test that read them from the tables under test would follow a mutation of those tables. The
// tables kQualitySites / kJitterHookSites / kCompanionSites are pinned equal to these below (TestSiteTablesAreLiteral).
std::vector<Site> AllSites()
{
    return {
        { Feature::QualityHooks, "quality.querySize", "quality.querySizeShape" },
        { Feature::QualityHooks, "quality.rendererUpdate", "quality.rendererUpdateShape" },
        { Feature::QualityHooks, "quality.resizeCallbacks", "quality.resizeCallbacksShape" },
        { Feature::JitterSource, "jitterSource.generator", "jitterSource.generatorShape" },
        { Feature::JitterSource, "jitterSource.cameraGetter", "jitterSource.cameraGetterShape" },
        { Feature::JitterSource, "jitterSource.producerCall", "jitterSource.producerCallShape" },
        { Feature::JitterSource, "jitterSource.cameraHead", "jitterSource.cameraHeadShape" },
        { Feature::JitterSource, "jitterSource.phases", "jitterSource.phasesShape" },
        { Feature::JitterSource, "jitterSource.strength", "jitterSource.strengthShape" },
        { Feature::CompanionNative, "companion.batch16", "companion.batch16Shape" },
        { Feature::CompanionNative, "companion.batch32", "companion.batch32Shape" },
        { Feature::CompanionNative, "companion.enqueue", "companion.enqueueShape" },
        { Feature::CompanionNative, "companion.bindState", "companion.bindStateShape" },
    };
}

// The seams that stay STRICT (plain Usable, never filled from the table) on a KNOWN build too, as a LITERAL set (a list read from Gate.h would follow
// a mutation of it, the P3 lesson): every hook site, location AND shape (13 pairs plus the jitter deviceSize that is its own shape), and these, from
// the contract text of the three Anchors headers: the quality write path (AnchorsQuality.h item 2: managerPointer, managerContextOffset,
// contextFlagsOffset, settingsPointer, the settings mode offset, contextCacheOffset) and the settings read path's own shape (item 3), the three
// jitter globals, strengthFloor, ndcSignMask and the camera global (AnchorsJitter.h). Every other seam a feature reads is table-fillable on a known
// build: a foreign hook over a helper function must not turn a feature off when the verified table value is at hand.
const std::set<std::string>& StrictOnKnownLiteral()
{
    static const std::set<std::string> strict = []
    {
        std::set<std::string> v;
        for (const Site& s : AllSites())
        {
            v.insert(s.location);
            v.insert(s.shape);
        }
        v.insert("jitterSource.deviceSize");
        for (const char* id : { "quality.settingsInstructions", "quality.settingsPointer", "quality.settingsModeOffset", "quality.managerPointer",
                                "quality.managerContextOffset", "quality.contextFlagsOffset", "quality.contextCacheOffset", "jitterSource.settingsPointer",
                                "jitterSource.rendererPointer", "jitterSource.devicePointer", "jitterSource.strengthFloor", "jitterSource.ndcSignMask",
                                "jitterSource.cameraGlobal" })
            v.insert(id);
        return v;
    }();
    return strict;
}

// Seams a feature needs as PLAIN Found (one anchor does not pin them): quality.contextCacheOffset on every build except where a KNOWN build
// takes it single-anchor through a weak dependency (F1b, tested separately); companion.nativeContextOffset on an unknown build only.
std::vector<std::string> PlainFoundOnly(Feature f, bool knownBuild)
{
    std::vector<std::string> v;
    if (f == Feature::QualityHooks)
        v.push_back("quality.contextCacheOffset");
    if (f == Feature::CompanionNative && !knownBuild)
        v.push_back("companion.nativeContextOffset");
    return v;
}

void TestEveryRequiredSeam()
{
    std::size_t total = 0;
    CHECK(StrictOnKnownLiteral().size() == 40);
    for (const Feature f : kGroup2)
    {
        const std::vector<std::string> seams = RequiredSeams(f, Reg());
        CHECK(!seams.empty());
        total += seams.size();
        for (const bool known : { true, false })
        {
            ResolvedBuild baseline = AllFound(known);
            CHECK(Run(&baseline, Settings(false, true)).Enabled(f));
            for (const std::string& id : seams)
            {
                // table fill is accepted on a KNOWN build, for the seams no contract names (F1a); everything else is strict
                const bool tableFillable = known && StrictOnKnownLiteral().count(id) == 0;
                // an unusable status refuses the feature, also when a known build filled the field from the table, except for a table-fillable seam
                for (const SeamStatus st : { SeamStatus::NotFound, SeamStatus::Ambiguous, SeamStatus::Conflict, SeamStatus::EntryPatched })
                {
                    for (const bool fell : { false, true })
                    {
                        // (an UNKNOWN build never has the flag; a stray one must be ignored there, so it is tried too: expectOn is false for it)
                        ResolvedBuild rb = AllFound(known);
                        SetStatus(rb, id, st, fell && st != SeamStatus::EntryPatched);
                        const GateReport g = Run(&rb, Settings(false, true));
                        const bool expectOn = fell && tableFillable && st != SeamStatus::EntryPatched; // EntryPatched is never filled
                        if (g.Enabled(f) != expectOn)
                            std::printf("FAIL: %s on=%d with %s %s (known=%d fell=%d), expected on=%d\n", FeatureName(f), g.Enabled(f), id.c_str(), StatusName(st), known,
                                        fell, expectOn);
                        CHECK(g.Enabled(f) == expectOn);
                    }
                }
                // a single-anchor seam is usable, except where the rule says plain Found
                ResolvedBuild single = AllFound(known);
                SetStatus(single, id, SeamStatus::FoundSingleAnchor);
                const std::vector<std::string> plain = PlainFoundOnly(f, known);
                const bool needsPlain = std::find(plain.begin(), plain.end(), id) != plain.end();
                const bool on = Run(&single, Settings(false, true)).Enabled(f);
                if (on == needsPlain)
                    std::printf("FAIL: %s with %s FoundSingleAnchor on=%d (plain Found required=%d, known=%d)\n", FeatureName(f), id.c_str(), on, needsPlain, known);
                CHECK(on == !needsPlain);
            }
        }
    }
    // the lists follow the registry: a pinned size per feature (a P2 change that binds one more field changes it deliberately)
    CHECK(RequiredSeams(Feature::QualityRead, Reg()).size() == 6);
    CHECK(RequiredSeams(Feature::QualityHooks, Reg()).size() == 19);
    CHECK(RequiredSeams(Feature::JitterProbe, Reg()).size() == 3);
    CHECK(RequiredSeams(Feature::JitterSource, Reg()).size() == 25);
    CHECK(RequiredSeams(Feature::CompanionNative, Reg()).size() == 27);
    CHECK(RequiredSeams(Feature::CharaViewAccessor, Reg()).size() == 3);
    CHECK(total == 83);
}

// a seam of one feature's list does not switch off an unrelated feature (the groups are independent)
void TestFeaturesAreIndependent()
{
    ResolvedBuild rb = AllFound(true);
    SetStatus(rb, "companion.layoutSlotTable", SeamStatus::NotFound);
    const GateReport g = Run(&rb, Settings());
    CHECK(!g.Enabled(Feature::CompanionNative));
    CHECK(g.Enabled(Feature::QualityRead) && g.Enabled(Feature::QualityHooks) && g.Enabled(Feature::JitterProbe) && g.Enabled(Feature::JitterSource) &&
          g.Enabled(Feature::CharaViewAccessor));
    CHECK(AllGroup1(g, true));
    ResolvedBuild q = AllFound(true);
    SetStatus(q, "quality.managerContextOffset", SeamStatus::Conflict);
    const GateReport gq = Run(&q, Settings());
    CHECK(!gq.Enabled(Feature::QualityHooks) && gq.Enabled(Feature::QualityRead) && gq.Enabled(Feature::CompanionNative) && gq.Enabled(Feature::JitterSource));
}

// ---- the one rule for hook sites ----------------------------------------------------------------------------------------------------------

void TestSiteTablesAreLiteral()
{
    std::vector<std::pair<std::string, std::string>> fromTables;
    for (const SitePair& s : kQualitySites)
        fromTables.emplace_back(s.location, s.shape);
    for (const JitterHookSite& s : kJitterHookSites)
        if (std::string(s.location) != s.shape)
            fromTables.emplace_back(s.location, s.shape);
    for (const SitePair& s : kCompanionSites)
        fromTables.emplace_back(s.location, s.shape);
    std::vector<std::pair<std::string, std::string>> literal;
    for (const Site& s : AllSites())
        literal.emplace_back(s.location, s.shape);
    CHECK(fromTables == literal);
    // deviceSize is its own shape (location == shape): the 7th jitter row
    bool deviceSize = false;
    for (const JitterHookSite& s : kJitterHookSites)
        deviceSize = deviceSize || (std::string(s.location) == "jitterSource.deviceSize" && std::string(s.shape) == "jitterSource.deviceSize");
    CHECK(deviceSize && sizeof(kJitterHookSites) / sizeof(kJitterHookSites[0]) == 7);
}

// The seams each feature requires, as LITERAL sets (a same-size swap in the lists of Gate.h changes the set, not the count).
void TestRequiredSeamListsAreLiteral()
{
    auto set = [](const std::vector<std::string>& v) { return std::set<std::string>(v.begin(), v.end()); };
    const std::set<std::string> read = { "quality.settingsInstructions", "quality.settingsPointer", "quality.settingsModeOffset",
                                         "quality.settingsQualityOffset", "quality.settingsDlaaOffset", "quality.settingsSelectorOffset" };
    std::set<std::string> hooks = read;
    for (const char* id : { "quality.querySize", "quality.querySizeShape", "quality.rendererUpdate", "quality.rendererUpdateShape", "quality.resizeCallbacks",
                            "quality.resizeCallbacksShape", "quality.managerPointer", "quality.devicePointer", "quality.managerContextOffset",
                            "quality.deviceCallbacksOffset", "quality.rendererSceneSizeOffset", "quality.contextFlagsOffset", "quality.contextCacheOffset" })
        hooks.insert(id);
    CHECK(set(RequiredSeams(Feature::QualityRead, Reg())) == read);
    CHECK(set(RequiredSeams(Feature::QualityHooks, Reg())) == hooks);
    CHECK(set(RequiredSeams(Feature::JitterProbe, Reg())) == std::set<std::string>({ "camera.global", "camera.cameraOffset", "camera.jitterPointerOffset" }));
    std::set<std::string> source;
    for (const char* id : { "generator", "generatorShape", "cameraGetter", "cameraGetterShape", "producerCall", "producerCallShape", "cameraHead", "cameraHeadShape", "phases",
                            "phasesShape", "strength", "strengthShape", "deviceSize", "settingsPointer", "rendererPointer", "devicePointer", "strengthFloor",
                            "ndcSignMask", "cameraGlobal", "settingsModeOffset", "settingsScaleOffset", "settingsAmountOffset", "rendererSceneSizeOffset",
                            "rendererExtentOffset", "deviceOutputSizeOffset" })
        source.insert(std::string("jitterSource.") + id);
    CHECK(set(RequiredSeams(Feature::JitterSource, Reg())) == source);
    std::set<std::string> companion;
    for (const char* id : { "batch16", "batch16Shape", "batch32", "batch32Shape", "enqueue", "enqueueShape", "bindState", "bindStateShape", "nameplateModulePointer",
                            "layoutSlotTable", "nativeContextOffset", "rendererBatchListOffset", "rendererBatchCountOffset", "rendererIndexBufferOffset",
                            "rendererVertexAllocationOffset", "rendererVertexStrideOffset", "rendererVertexOffsetOffset", "batchFormatOffset", "batchExtensionOffset",
                            "drawStateFlagOffset", "moduleStatePointerOffset", "stateQueuePointerOffset", "indexBufferCapacityOffset", "indexBufferFlagsOffset",
                            "indexBufferCpuStartOffset", "indexBufferCpuEndOffset", "entryPointerOffset" })
        companion.insert(std::string("companion.") + id);
    CHECK(set(RequiredSeams(Feature::CompanionNative, Reg())) == companion);
    CHECK(set(RequiredSeams(Feature::CharaViewAccessor, Reg())) == std::set<std::string>({ "chara.accessor", "chara.managerPointer", "chara.targetsOffset" }));
}

void TestOneRuleForEverySite()
{
    const std::vector<Site> sites = AllSites();
    CHECK(sites.size() == 3 + 6 + 4); // deviceSize is its own shape (location == shape) and is covered by the seam loop above
    for (const bool known : { true, false })
    {
        for (const Site& s : sites)
        {
            // location usable, shape NOT: refused (the shape is the only strict check of the bytes the hook overwrites)
            for (const SeamStatus st : { SeamStatus::NotFound, SeamStatus::Conflict, SeamStatus::Ambiguous })
            {
                ResolvedBuild rb = AllFound(known);
                SetStatus(rb, s.shape, st);
                CHECK(!Run(&rb, Settings(false, true)).Enabled(s.feature));
            }
            // a shape on a KNOWN build is never rescued by the table: a location that fell back to the table (UsableOrTable true, Usable false)
            // with the shape missing is refused, and so is the opposite, a table-filled location with a perfect shape
            if (known)
            {
                ResolvedBuild a = AllFound(true);
                SetStatus(a, s.location, SeamStatus::NotFound, true);
                SetStatus(a, s.shape, SeamStatus::NotFound);
                CHECK(a.UsableOrTable(s.location) && !a.UsableOrTable(s.shape) && !Run(&a, Settings()).Enabled(s.feature));
                ResolvedBuild b = AllFound(true);
                SetStatus(b, s.location, SeamStatus::Conflict, true);
                CHECK(b.UsableOrTable(s.location) && b.UsableOrTable(s.shape) && !Run(&b, Settings()).Enabled(s.feature));
            }
            // a location that is EntryPatched: never hooked
            ResolvedBuild ep = AllFound(known);
            SetStatus(ep, s.location, SeamStatus::EntryPatched);
            CHECK(!Run(&ep, Settings(false, true)).Enabled(s.feature));
        }
    }
}

// The same rule on REAL resolver output: a known build, the location seam cannot be found, the resolver fills it from the table, and the shape
// (a guard at the location's value, failClosed) is NOT usable: the gate refuses.
void TestTableFilledLocationYieldsUnusableShape()
{
    using namespace jt;
    auto img = MakeJitterImage();
    REQUIRE(img != nullptr);
    Registry reg = JitterRegistry();
    // break every anchor of the location seam: it cannot be found any more
    SeamSpec* loc = FindSeam(reg, "jitterSource.cameraGetter");
    REQUIRE(loc != nullptr);
    for (AnchorSpec& a : loc->anchors)
        a.pattern = "DE AD BE EF 00 11 22 33";
    ResolveOptions o;
    o.knownTable = &kCurrentBuild;
    const ResolvedBuild rb = ResolveImage(*img, reg, o);
    CHECK(rb.knownBuild);
    const SeamResult& l = Get(rb, "jitterSource.cameraGetter");
    const SeamResult& s = Get(rb, "jitterSource.cameraGetterShape");
    CHECK(l.status == SeamStatus::NotFound && l.fellBackToTable);
    CHECK(rb.UsableOrTable("jitterSource.cameraGetter") && !rb.UsableOrTable("jitterSource.cameraGetterShape") && !rb.Usable("jitterSource.cameraGetterShape"));
    GateInput in;
    in.exeIsFfxiv = true;
    in.exactKnownBuild = true;
    in.rb = &rb;
    in.registry = &reg;
    CHECK(!Decide(in, Settings()).Enabled(Feature::JitterSource));
    // control: the untouched image resolves, the gate allows the jitter source (the other groups have nothing in this image)
    const ResolvedBuild good = ResolveImage(*img, JitterRegistry(), o);
    in.rb = &good;
    Registry jr = JitterRegistry();
    in.registry = &jr;
    const GateReport g = Decide(in, Settings());
    CHECK(g.Enabled(Feature::JitterSource) && !g.Enabled(Feature::CompanionNative));
    // and the same image as an UNKNOWN build with the key: the same verdict from resolver values only
    auto unknownImg = MakeJitterImage(0x6bbbbbbbu);
    const ResolvedBuild un = ResolveImage(*unknownImg, JitterRegistry(), o);
    CHECK(!un.knownBuild);
    in.rb = &un;
    in.exactKnownBuild = false;
    const GateReport gu = Decide(in, Settings(false, true));
    // This registry holds only the jitter seams, so the four cross-group pairs cannot be compared on an UNKNOWN build and the jitter
    // source (which depends on all four) is refused for exactly that reason; every seam of its own is usable. The full registry on the real
    // exe (gate_real_exe) proves the enabled verdict, with the pairs compared.
    CHECK(!gu.Enabled(Feature::JitterSource) && Contains(gu.Reason(Feature::JitterSource), "cross-check not confirmed"));
    CHECK(!Decide(in, Settings()).Enabled(Feature::JitterSource));
}

// ---- quality ------------------------------------------------------------------------------------------------------------------------------

void TestQualityRules()
{
    for (const bool known : { true, false })
    {
        // the detours need the anchor named `entry` Unique (query size and renderer update), not the status of the location
        for (const char* seam : { "quality.querySize", "quality.rendererUpdate" })
        {
            ResolvedBuild rb = AllFound(known);
            for (AnchorResult& a : Mut(rb, seam).anchors)
            {
                if (a.name == "entry")
                    a.outcome = AnchorOutcome::Zero;
            }
            const GateReport g = Run(&rb, Settings(false, true));
            CHECK(!g.Enabled(Feature::QualityHooks) && g.Enabled(Feature::QualityRead));
            CHECK(Contains(g.Reason(Feature::QualityHooks), "'entry'"));
            // Multiple is not Unique either
            for (AnchorResult& a : Mut(rb, seam).anchors)
            {
                if (a.name == "entry")
                    a.outcome = AnchorOutcome::Multiple;
            }
            CHECK(!Run(&rb, Settings(false, true)).Enabled(Feature::QualityHooks));
        }
        // resizeCallbacks has no entry rule: its entry anchor lost (two anchors, one left) keeps the site usable
        ResolvedBuild rc = AllFound(known);
        SetStatus(rc, "quality.resizeCallbacks", SeamStatus::FoundSingleAnchor);
        for (AnchorResult& a : Mut(rc, "quality.resizeCallbacks").anchors)
            if (a.name == "entry")
                a.outcome = AnchorOutcome::Zero;
        CHECK(Run(&rc, Settings(false, true)).Enabled(Feature::QualityHooks));
    }
    // the DLAA decision from a snapshot is bounds-checked: the mode byte (+0x54) is 2 and the DLAA byte (+0x45) is 0, and an index outside the bytes
    // that were read is never touched
    {
        const NativeQualityBuild q;
        unsigned char snap[0x56] = {};
        snap[q.settingsModeOffset] = 2;
        CHECK(DlaaRequiredFromSnapshot(snap, sizeof(snap), q));
        snap[q.settingsDlaaOffset] = 1;
        CHECK(!DlaaRequiredFromSnapshot(snap, sizeof(snap), q));
        snap[q.settingsDlaaOffset] = 0;
        snap[q.settingsModeOffset] = 3;
        CHECK(!DlaaRequiredFromSnapshot(snap, sizeof(snap), q));
        snap[q.settingsModeOffset] = 2;
        CHECK(!DlaaRequiredFromSnapshot(snap, q.settingsModeOffset, q));     // the mode byte lies just outside the bytes that were read
        CHECK(DlaaRequiredFromSnapshot(snap, q.settingsModeOffset + 1, q));
        CHECK(!DlaaRequiredFromSnapshot(snap, q.settingsDlaaOffset, q));
        CHECK(!DlaaRequiredFromSnapshot(nullptr, sizeof(snap), q));
        NativeQualityBuild healed = q;
        healed.settingsModeOffset = 0x60; // a healed offset past the end of this snapshot
        CHECK(!DlaaRequiredFromSnapshot(snap, sizeof(snap), healed));
    }
    // the settings snapshot: every settings offset must fit the fixed buffer; the known build needs 0x56 bytes
    CHECK(SettingsSnapshotBytes(kCurrentBuild.nativeQuality) == 0x56);
    CHECK(kMaxSettingsSnapshotBytes == 0x100);
    for (std::uint32_t NativeQualityBuild::*field : { &NativeQualityBuild::settingsModeOffset, &NativeQualityBuild::settingsQualityOffset,
                                                      &NativeQualityBuild::settingsDlaaOffset, &NativeQualityBuild::settingsRendererOffset,
                                                      &NativeQualityBuild::settingsAdditionalOffset, &NativeQualityBuild::settingsSelectorOffset })
    {
        ResolvedBuild ok = AllFound(false);
        ok.build.nativeQuality.*field = 0xff;
        CHECK(Run(&ok, Settings(false, true)).Enabled(Feature::QualityRead));
        CHECK(SettingsSnapshotBytes(ok.build.nativeQuality) == 0x100);
        ResolvedBuild bad = AllFound(false);
        bad.build.nativeQuality.*field = 0x100;
        const GateReport g = Run(&bad, Settings(false, true));
        CHECK(!g.Enabled(Feature::QualityRead) && !g.Enabled(Feature::QualityHooks));
        CHECK(Contains(g.Reason(Feature::QualityRead), "snapshot buffer"));
    }
}

// ---- jitter source / probe ----------------------------------------------------------------------------------------------------------------

void TestJitterRules()
{
    for (const bool known : { true, false })
    {
        // the three globals: a Conflict global is table-filled on a known build (non-zero Rva) and the zero-Rva minimum alone would pass it
        for (const JitterGlobal& g : kJitterGlobals)
        {
            ResolvedBuild rb = AllFound(known);
            SetStatus(rb, g.seam, SeamStatus::Conflict, known);
            CHECK(*FieldU32(rb.build, *FindField(g.rvaKey)) != 0); // the field still holds a value: only the Usable form refuses
            CHECK(!Run(&rb, Settings(false, true)).Enabled(Feature::JitterSource));
        }
        // the seams that have no field of their own: the constants the core assumes, and the camera global of the world tie
        for (const char* id : kJitterGateSeams)
        {
            ResolvedBuild rb = AllFound(known);
            SetStatus(rb, id, SeamStatus::NotFound, known);
            const GateReport g = Run(&rb, Settings(false, true));
            CHECK(!g.Enabled(Feature::JitterSource));
            CHECK(Contains(g.Reason(Feature::JitterSource), id));
        }
        // every global field zero: refused even with usable seams (defence in depth)
        for (std::uint32_t JitterSourceBuild::*field :
             { &JitterSourceBuild::settingsPointerRva, &JitterSourceBuild::rendererPointerRva, &JitterSourceBuild::devicePointerRva })
        {
            ResolvedBuild rb = AllFound(known);
            rb.build.jitterSource.*field = 0;
            CHECK(!Run(&rb, Settings(false, true)).Enabled(Feature::JitterSource));
        }
    }
    // the table fields the core reads at run time are the constants the seams check
    CHECK(JitterSourceBuild {}.strengthFloorBits == 0x3a83126fu && JitterSourceBuild {}.ndcSignMaskBits == 0x80000000u);
    // the probe is its own feature: it needs the camera chain, not the source hooks
    ResolvedBuild rb = AllFound(false);
    SetStatus(rb, "jitterSource.generator", SeamStatus::NotFound);
    const GateReport g = Run(&rb, Settings(false, true));
    CHECK(!g.Enabled(Feature::JitterSource) && g.Enabled(Feature::JitterProbe));
}

// ---- companion ----------------------------------------------------------------------------------------------------------------------------

void TestCompanionRules()
{
    // batch32 is gated on its own shape (batchInstructions is bound at batch16 only)
    for (const bool known : { true, false })
    {
        ResolvedBuild rb = AllFound(known);
        SetStatus(rb, "companion.batch32Shape", SeamStatus::NotFound);
        const GateReport g = Run(&rb, Settings(false, true));
        CHECK(!g.Enabled(Feature::CompanionNative) && Contains(g.Reason(Feature::CompanionNative), "companion.batch32Shape"));
    }
    // enqueue on an UNKNOWN build needs the caller-side anchor Unique (not the hit sum); a known build does not
    {
        ResolvedBuild rb = AllFound(false);
        SeamResult& e = Mut(rb, "companion.enqueue");
        e.status = SeamStatus::FoundSingleAnchor;
        for (AnchorResult& a : e.anchors)
            if (a.name == "callerEpilogue")
                a.outcome = AnchorOutcome::Zero;
        e.hits = 5; // the SUM would suggest evidence: the rule looks at the anchor
        const GateReport g = Run(&rb, Settings(false, true));
        CHECK(!g.Enabled(Feature::CompanionNative) && Contains(g.Reason(Feature::CompanionNative), "callerEpilogue"));
        for (AnchorResult& a : e.anchors)
            if (a.name == "callerEpilogue")
                a.outcome = AnchorOutcome::Multiple;
        CHECK(!Run(&rb, Settings(false, true)).Enabled(Feature::CompanionNative));
        ResolvedBuild kn = AllFound(true);
        SeamResult& ek = Mut(kn, "companion.enqueue");
        ek.status = SeamStatus::FoundSingleAnchor;
        for (AnchorResult& a : ek.anchors)
            if (a.name == "callerEpilogue")
                a.outcome = AnchorOutcome::Zero;
        CHECK(Run(&kn, Settings()).Enabled(Feature::CompanionNative));
    }
    // bindState: on an unknown build the native context offset must be plain Found (it is dereferenced as a COM pointer)
    {
        ResolvedBuild rb = AllFound(false);
        SetStatus(rb, "companion.nativeContextOffset", SeamStatus::FoundSingleAnchor);
        const GateReport g = Run(&rb, Settings(false, true));
        CHECK(!g.Enabled(Feature::CompanionNative) && Contains(g.Reason(Feature::CompanionNative), "plain Found"));
        ResolvedBuild kn = AllFound(true);
        SetStatus(kn, "companion.nativeContextOffset", SeamStatus::FoundSingleAnchor);
        CHECK(Run(&kn, Settings()).Enabled(Feature::CompanionNative));
        // F1a: no contract names it for a known build, whose table value is verified offline: table-filled it stays usable; not filled, refused
        SetStatus(kn, "companion.nativeContextOffset", SeamStatus::NotFound, true);
        CHECK(Run(&kn, Settings()).Enabled(Feature::CompanionNative));
        SetStatus(kn, "companion.nativeContextOffset", SeamStatus::NotFound, false);
        CHECK(!Run(&kn, Settings()).Enabled(Feature::CompanionNative));
        ResolvedBuild un = AllFound(false);
        SetStatus(un, "companion.nativeContextOffset", SeamStatus::NotFound, true); // an unknown build never table-fills (the flag cannot be set there)
        CHECK(!Run(&un, Settings(false, true)).Enabled(Feature::CompanionNative));
    }
    // the header group heals only format and extension: all or nothing, and every index stays inside the 64-byte header / the queue entry
    {
        const CompanionBuild d {};
        CHECK(CompanionOffsetsInBounds(d));
        struct Case
        {
            std::uint32_t CompanionBuild::*field;
            std::uint32_t okValue, badValue;
        };
        const Case cases[] = {
            { &CompanionBuild::batchFormatOffset, 0x3c, 0x3d },    { &CompanionBuild::batchExtensionOffset, 0x3e, 0x3f },
            { &CompanionBuild::batchKindOffset, 0x3c, 0x3d },      { &CompanionBuild::batchCountOffset, 0x3c, 0x3d },
            { &CompanionBuild::batchVerticesOffset, 0x38, 0x39 },  { &CompanionBuild::batchWrapperOffset, 0x38, 0x39 },
            { &CompanionBuild::entryPointerOffset, 8, 16 },
        };
        for (const Case& c : cases)
        {
            CompanionBuild ok = d, bad = d;
            ok.*(c.field) = c.okValue;
            bad.*(c.field) = c.badValue;
            CHECK(CompanionOffsetsInBounds(ok));
            CHECK(!CompanionOffsetsInBounds(bad));
            ResolvedBuild rb = AllFound(false);
            rb.build.companion = bad;
            const GateReport g = Run(&rb, Settings(false, true));
            CHECK(!g.Enabled(Feature::CompanionNative) && Contains(g.Reason(Feature::CompanionNative), "header"));
        }
        CompanionBuild unaligned = d;
        unaligned.entryPointerOffset = 4;
        CHECK(!CompanionOffsetsInBounds(unaligned));
        CompanionBuild noHeader = d;
        noHeader.nativeBatchHeaderSize = 0;
        CHECK(!CompanionOffsetsInBounds(noHeader));
    }
}

void TestCharaRules()
{
    for (const bool known : { true, false })
    {
        ResolvedBuild rb = AllFound(known);
        SetStatus(rb, "chara.managerPointer", SeamStatus::NotFound, false);
        const GateReport g = Run(&rb, Settings(false, true));
        CHECK(!g.Enabled(Feature::CharaViewAccessor) && Contains(g.Reason(Feature::CharaViewAccessor), "chara.managerPointer"));
        CHECK(g.Enabled(Feature::QualityHooks));
        // table-filled on a known build (F1a: no contract names it, the cross-check compares only usable sides): the accessor stays on
        ResolvedBuild filled = AllFound(known);
        SetStatus(filled, "chara.managerPointer", SeamStatus::NotFound, known);
        CHECK(Run(&filled, Settings(false, true)).Enabled(Feature::CharaViewAccessor) == known);
    }
}

// ---- EntryPatched is not NotFound ---------------------------------------------------------------------------------------------------------

void TestEntryPatchedIsDistinct()
{
    ResolvedBuild patched = AllFound(false);
    SetStatus(patched, "quality.querySize", SeamStatus::EntryPatched);
    const std::string p = Run(&patched, Settings(false, true)).Reason(Feature::QualityHooks);
    ResolvedBuild missing = AllFound(false);
    SetStatus(missing, "quality.querySize", SeamStatus::NotFound);
    const std::string m = Run(&missing, Settings(false, true)).Reason(Feature::QualityHooks);
    CHECK(Contains(p, "EntryPatched") && Contains(p, "foreign hook") && !Contains(p, "NotFound"));
    CHECK(Contains(m, "NotFound") && !Contains(m, "foreign hook") && !Contains(m, "EntryPatched"));
    // also on a known build, where the table does not fill it (fail closed like the old byte guards)
    ResolvedBuild known = AllFound(true);
    SetStatus(known, "jitterSource.generator", SeamStatus::EntryPatched);
    const GateReport g = Run(&known, Settings());
    CHECK(!g.Enabled(Feature::JitterSource) && Contains(g.Reason(Feature::JitterSource), "foreign hook"));
}

// ---- cross-group consistency --------------------------------------------------------------------------------------------------------------

void TestCrossChecks()
{
    CHECK(sizeof(kCrossPairs) / sizeof(kCrossPairs[0]) == 4);
    for (const bool known : { true, false })
    {
        {
            const ResolvedBuild ok = AllFound(known);
            const GateReport g = Run(&ok, Settings(false, true));
            CHECK(g.crossChecks.size() == 4);
            for (const CrossCheckResult& c : g.crossChecks)
                CHECK(c.compared && c.agree && c.left != 0 && c.left == c.right);
        }
        // the pairs and the features each one turns off, as LITERALS (a test that read them from kCrossPairs would follow a mutation of the table)
        struct LiteralPair
        {
            const char* left;
            const char* right;
            std::vector<Feature> affected;
        };
        const LiteralPair literal[] = {
            { "chara.managerPointer", "jitterSource.rendererPointer", { Feature::CharaViewAccessor, Feature::JitterSource } },
            { "camera.global", "jitterSource.cameraGlobal", { Feature::JitterProbe, Feature::JitterSource } },
            { "quality.settingsPointer", "jitterSource.settingsPointer", { Feature::QualityRead, Feature::QualityHooks, Feature::JitterSource } },
            { "quality.devicePointer", "jitterSource.devicePointer", { Feature::QualityHooks, Feature::JitterSource } },
        };
        for (std::size_t i = 0; i < 4; ++i)
        {
            CHECK(std::string(kCrossPairs[i].left) == literal[i].left && std::string(kCrossPairs[i].right) == literal[i].right);
            std::vector<Feature> inTable;
            for (const Feature f : kCrossPairs[i].affects)
                if (f != Feature::Count)
                    inTable.push_back(f);
            CHECK(inTable == literal[i].affected);
        }
        for (const LiteralPair& lp : literal)
        {
            struct
            {
                const char* left;
                const char* right;
            } pair = { lp.left, lp.right };
            ResolvedBuild rb = AllFound(known);
            Mut(rb, pair.right).rva += 0x10;
            const GateReport g = Run(&rb, Settings(false, true));
            const std::vector<Feature>& affected = lp.affected;
            for (const Feature f : kGroup2)
            {
                const bool isAffected = std::find(affected.begin(), affected.end(), f) != affected.end();
                if (g.Enabled(f) == isAffected)
                    std::printf("FAIL: %s mismatch: %s enabled=%d expected affected=%d\n", pair.left, FeatureName(f), g.Enabled(f), isAffected);
                CHECK(g.Enabled(f) == !isAffected);
                if (isAffected)
                    CHECK(Contains(g.Reason(f), "cross-check failed") && Contains(g.Reason(f), pair.left) && Contains(g.Reason(f), pair.right));
            }
            CHECK(AllGroup1(g, true));
            int mismatches = 0;
            for (const CrossCheckResult& c : g.crossChecks)
                mismatches += c.compared && !c.agree ? 1 : 0;
            CHECK(mismatches == 1);
            // the mismatch is a mismatch whichever side moved
            ResolvedBuild left = AllFound(known);
            Mut(left, pair.left).rva += 0x10;
            CHECK(!Run(&left, Settings(false, true)).Enabled(affected.front()));
        }
        // compared only when BOTH sides are usable. KNOWN build: the side that is down is off by its own rule, the other side keeps running (today's
        // behaviour). UNKNOWN build: a pair that cannot be compared is no proof, so the other side is off too
        // (TestUnknownBuildCrossCheckNeedsBothSides covers every pair)
        ResolvedBuild half = AllFound(known);
        SetStatus(half, "jitterSource.cameraGlobal", SeamStatus::NotFound);
        Mut(half, "camera.global").rva += 0x10; // would mismatch if it were compared
        const GateReport g = Run(&half, Settings(false, true));
        CHECK(!g.Enabled(Feature::JitterSource) && g.Enabled(Feature::JitterProbe) == known);
        bool found = false;
        for (const CrossCheckResult& c : g.crossChecks)
            if (c.name.rfind("camera.global", 0) == 0)
            {
                found = true;
                CHECK(!c.compared);
            }
        CHECK(found);
    }
}

// ---- not self-healing + the log block --------------------------------------------------------------------------------------------------------

void TestNotSelfHealing()
{
    const ResolvedBuild rb = AllFound(false);
    const std::vector<std::string> list = UnhealedOffsets(rb, Reg());
    std::size_t unboundOffsets = 0;
    for (const FieldRow& row : kFieldTable)
    {
        if (row.kind != FieldKind::Offset)
            continue;
        char key[96];
        FieldKey(row, key, sizeof(key));
        bool bound = false;
        for (const Binding& b : Reg().bindings)
            bound = bound || b.key == key;
        unboundOffsets += bound ? 0 : 1;
    }
    CHECK(list.size() == unboundOffsets && !list.empty());
    auto has = [&](const std::string& key) { return std::any_of(list.begin(), list.end(), [&](const std::string& s) { return s.rfind(key + "=0x", 0) == 0; }); };
    for (const char* key : { "nativeQuality.settingsRendererOffset", "nativeQuality.settingsAdditionalOffset", "charaView.transparentOffset", "charaView.depthOffset0",
                             "charaView.entryWidthOffset", "jitter.jitterXOffset", "jitter.counterOffset", "companion.queueHeadOffset", "companion.queueNextBlockOffset",
                             "companion.batchKindOffset", "companion.resourceWrapperOffset", "companion.indexCountOffset" })
        CHECK(has(key));
    for (const char* key : { "companion.nativeContextOffset", "companion.batchFormatOffset", "nativeQuality.managerContextOffset", "jitterSource.settingsModeOffset" })
        CHECK(!has(key));
    CHECK(has("companion.queueHeadOffset") && std::any_of(list.begin(), list.end(), [](const std::string& s) { return s == "companion.queueHeadOffset=0x50"; }));
    // a bound offset whose seam is down joins the list, marked
    ResolvedBuild down = AllFound(false);
    SetStatus(down, "companion.nativeContextOffset", SeamStatus::NotFound);
    const std::vector<std::string> list2 = UnhealedOffsets(down, Reg());
    CHECK(list2.size() == list.size() + 1);
    CHECK(std::any_of(list2.begin(), list2.end(), [](const std::string& s) { return s.rfind("companion.nativeContextOffset=0x17b8", 0) == 0 && Contains(s, "(seam unusable)"); }));
    // the line: one line, names the build kind
    const std::string line = FormatNotSelfHealing(rb, Reg());
    CHECK(line.find('\n') == std::string::npos && line.rfind("[ffxiv-gate] not self-healing: ", 0) == 0 && Contains(line, "UNKNOWN build"));
    CHECK(Contains(FormatNotSelfHealing(AllFound(true), Reg()), "known build, table verified offline"));
}

void TestLogBlock()
{
    const ResolvedBuild known = AllFound(true);
    const GateReport k = Run(&known, Settings());
    const auto lines = FormatGateBlock(k, &known, Reg(), 0x6ad10bfd, 0x381f000, 1.5, "ffxiv_dx11.exe");
    REQUIRE(!lines.empty());
    CHECK(lines[0].text ==
          "[ffxiv-gate] build stamp=0x6ad10bfd size=0x381f000 known=yes forced_unknown=no legacy=no unknown_build_hooks=off exe=ffxiv_dx11.exe resolve_ms=1.500");
    CHECK(lines[0].level == LogLevel::Info);
    // bounded: head + group1 + 6 features + 4 checks (+ the not-self-healing line on an unknown build)
    CHECK(lines.size() == 1 + 1 + 6 + 4);
    for (const GateLine& l : lines)
    {
        CHECK(l.text.rfind("[ffxiv-gate] ", 0) == 0 && l.text.find('\n') == std::string::npos);
        CHECK(l.level == LogLevel::Info); // everything is on and agrees: no warning
    }
    CHECK(Contains(lines[1].text, "group1 ON") && Contains(lines[1].text, "lighting"));
    CHECK(Contains(lines[2].text, "feature qualityRead ON"));

    const ResolvedBuild unknown = AllFound(false);
    const GateReport u = Run(&unknown, Settings());
    const auto ul = FormatGateBlock(u, &unknown, Reg(), 0x6bbbbbbb, 0x381f000, 2.0, "ffxiv_dx11.exe");
    CHECK(ul.size() == 1 + 1 + 6 + 4 + 1);
    CHECK(ul[0].text.find("known=no") != std::string::npos && ul[0].text.find("unknown_build_hooks=off") != std::string::npos);
    CHECK(ul[2].level == LogLevel::Warn && Contains(ul[2].text, "feature qualityRead OFF"));
    CHECK(ul.back().level == LogLevel::Warn && Contains(ul.back().text, "not self-healing"));
    CHECK(ul[1].level == LogLevel::Info && Contains(ul[1].text, "group1 ON"));

    // a mismatch is a Warn line naming both values
    ResolvedBuild bad = AllFound(true);
    Mut(bad, "camera.global").rva = 0x290dba0;
    Mut(bad, "jitterSource.cameraGlobal").rva = 0x290dba8;
    const auto bl = FormatGateBlock(Run(&bad, Settings()), &bad, Reg(), 1, 2, 0, "x");
    bool seen = false;
    for (const GateLine& l : bl)
        if (Contains(l.text, "cross-check camera.global == jitterSource.cameraGlobal: MISMATCH (0x290dba0 vs 0x290dba8)"))
        {
            seen = true;
            CHECK(l.level == LogLevel::Warn);
        }
    CHECK(seen);
    // the legacy block names the key and has no not-self-healing line
    const GateReport leg = Run(nullptr, Settings(true), true, true);
    const auto ll = FormatGateBlock(leg, nullptr, Reg(), 0x6ad10bfd, 0x381f000, 0, "ffxiv_dx11.exe");
    CHECK(Contains(ll[0].text, "legacy=yes") && ll.size() == 1 + 1 + 6 + 0);
}

// ---- The known build's table fill, the quality cache offset -------------------------------------------------------------

// A third party hooks a helper function the fork never touches (companion.indexBufferMap, camera.getter): the seams whose anchors sit in that
// helper go NotFound and the resolver fills their fields from the table. On the KNOWN build the table value is verified offline and no contract names
// those seams, so the feature stays on; the same loss on an unknown build refuses it. (The real-exe version is in gate_real_exe.)
// ---- the log block on every gate state ----------------------------------------------------------------------------------------------------

// The block keeps one shape per state: the fallback warning only where the legacy decision runs on the exact known build, the
// not-self-healing line only where a resolve ran and the gate does not call the build known (or the fallback runs on the
// exact known build, where it names the known build the legacy decision runs on). A dropped disjunct in that gate prints the
// line on the wrong builds or drops it where the player needs it.
void TestGateBlockSixStates()
{
    const GateSettings def = Settings();
    auto hasLine = [](const std::vector<GateLine>& lines, const std::string& needle)
    {
        for (const GateLine& l : lines)
            if (Contains(l.text, needle))
                return true;
        return false;
    };
    // ordinary known build: head, group1, 6 features, 4 checks; everything on and agreeing, no warning anywhere
    {
        const ResolvedBuild known = AllFound(true);
        const GateReport k = Run(&known, def);
        const auto lines = FormatGateBlock(k, &known, Reg(), 0x6ad10bfd, 0x381f000, 1.5, "ffxiv_dx11.exe");
        CHECK(lines.size() == 1 + 1 + 6 + 4);
        for (const GateLine& l : lines)
            CHECK(l.level == LogLevel::Info);
        CHECK(!hasLine(lines, "falling back") && !hasLine(lines, "not self-healing"));
    }
    // ordinary unknown build: one more line, and it names the UNKNOWN build (a moved offset is not followed there)
    {
        const ResolvedBuild unknown = AllFound(false);
        const GateReport u = Run(&unknown, def);
        const auto lines = FormatGateBlock(u, &unknown, Reg(), 0x6bbbbbbb, 0x381f000, 2.0, "ffxiv_dx11.exe");
        CHECK(lines.size() == 1 + 1 + 6 + 4 + 1);
        CHECK(lines.back().level == LogLevel::Warn && Contains(lines.back().text, "not self-healing"));
        CHECK(Contains(lines.back().text, "UNKNOWN build") && lines.back().text.find("known build, table verified offline") == std::string::npos);
    }
    // the resolver failed on the exact known build: the fallback warning plus the not-self-healing line, and that line
    // names the KNOWN build the legacy decision runs on (the gate's verdict, not the failed result's unknown mark)
    {
        ResolvedBuild exception = AllFound(true);
        exception.registryErrors.push_back("resolver exception: bad_alloc");
        const GateReport g = Run(&exception, def, true, true);
        REQUIRE(g.legacyFallback && g.known);
        const auto lines = FormatGateBlock(g, &exception, Reg(), 0x6ad10bfd, 0x381f000, 0, "ffxiv_dx11.exe");
        CHECK(lines.size() == 1 + 1 + 1 + 6 + 1);
        CHECK(lines[1].level == LogLevel::Warn &&
              Contains(lines[1].text, "resolver infrastructure failure on the exact known build (resolver exception: bad_alloc)") &&
              Contains(lines[1].text, "falling back to the legacy decision"));
        for (std::size_t i = 3; i < 3 + 6; ++i)
            CHECK(lines[i].level == LogLevel::Info && Contains(lines[i].text, "falling back to the legacy decision") &&
                  Contains(lines[i].text, "the exact known build runs on the compile-time table"));
        CHECK(lines.back().level == LogLevel::Warn && Contains(lines.back().text, "not self-healing"));
        CHECK(Contains(lines.back().text, "known build, table verified offline") && !hasLine(lines, "UNKNOWN build"));
    }
    // the forced-unknown refusal on the exact build: Warn lines, but no not-self-healing line (the gate calls it known)
    {
        const ResolvedBuild known = AllFound(true);
        const GateReport g = Run(&known, Settings(false, false, true), true, true);
        CHECK(g.known && !g.legacyFallback);
        const auto lines = FormatGateBlock(g, &known, Reg(), 0x6ad10bfd, 0x381f000, 0, "ffxiv_dx11.exe");
        CHECK(lines.size() == 1 + 1 + 6 && !hasLine(lines, "not self-healing") && !hasLine(lines, "falling back"));
    }
    // the rollback key on the exact build: no not-self-healing line even with a resolve result in hand
    {
        const ResolvedBuild known = AllFound(true);
        const GateReport g = Run(nullptr, Settings(true), true, true);
        const auto lines = FormatGateBlock(g, &known, Reg(), 0x6ad10bfd, 0x381f000, 0, "ffxiv_dx11.exe");
        CHECK(lines.size() == 1 + 1 + 6 + 0 && !hasLine(lines, "not self-healing") && !hasLine(lines, "falling back"));
    }
    // no resolve ran on an unknown build: fail open, no not-self-healing line without a result
    {
        const GateReport g = Run(nullptr, def, true, false);
        CHECK(!g.known && !g.legacyFallback);
        const auto lines = FormatGateBlock(g, nullptr, Reg(), 0x6ad10bfd, 0x381f000, 0, "ffxiv_dx11.exe");
        CHECK(lines.size() == 1 + 1 + 6 + 0 && !hasLine(lines, "not self-healing"));
    }
}

// Group 1 names the identity it ran on: the stamp and size on the exact known build (never the file name, so a renamed
// copy reads the same), the exe name and the shader fingerprints anywhere else. A swapped reason would name the file on
// the build whose rule never reads it.
void TestGroup1ReasonNamesIdentity()
{
    const ResolvedBuild known = AllFound(true);
    const GateReport k = Run(&known, Settings(), true, true);
    for (std::size_t i = 0; i < static_cast<std::size_t>(kFirstGroup2Feature); ++i)
    {
        CHECK(Contains(k.features[i].reason, "exact known build (PE stamp and size)") &&
              Contains(k.features[i].reason, "identified without the exe name"));
        CHECK(k.features[i].reason.find("ffxiv_dx11.exe") == std::string::npos);
    }
    const ResolvedBuild unknown = AllFound(false);
    const GateReport u = Run(&unknown, Settings(), true, false);
    for (std::size_t i = 0; i < static_cast<std::size_t>(kFirstGroup2Feature); ++i)
        CHECK(Contains(u.features[i].reason, "ffxiv_dx11.exe: identified by name and shader CRC"));
    // the single-feature entry the game selector asks gives the same answer
    CHECK(Contains(DecideGroup1(Feature::Lighting, true, true, Settings()).reason, "identified without the exe name"));
    CHECK(Contains(DecideGroup1(Feature::Lighting, true, false, Settings()).reason, "ffxiv_dx11.exe: identified by name and shader CRC"));
    // the emitted block carries the reason on its group1 line
    const auto kl = FormatGateBlock(k, &known, Reg(), 0x6ad10bfd, 0x381f000, 0, "ffxiv_dx11.exe");
    CHECK(Contains(kl[1].text, "exact known build (PE stamp and size)") && kl[1].text.find("ffxiv_dx11.exe") == std::string::npos);
    const auto ul = FormatGateBlock(u, &unknown, Reg(), 0x6bbbbbbb, 0x381f000, 0, "ffxiv_dx11.exe");
    CHECK(Contains(ul[1].text, "ffxiv_dx11.exe: identified by name and shader CRC"));
}

void TestHelperHookOnKnownBuild()
{
    const char* const indexBuffer[] = { "companion.indexBufferCapacityOffset", "companion.indexBufferFlagsOffset", "companion.indexBufferCpuStartOffset",
                                        "companion.indexBufferCpuEndOffset" };
    ResolvedBuild known = AllFound(true);
    for (const char* id : indexBuffer)
        SetStatus(known, id, SeamStatus::NotFound, true);
    const GateReport k = Run(&known, Settings());
    CHECK(AllGroup2(k, true));
    ResolvedBuild unknown = AllFound(false);
    for (const char* id : indexBuffer)
        SetStatus(unknown, id, SeamStatus::NotFound);
    const GateReport u = Run(&unknown, Settings(false, true), true, false);
    CHECK(!u.Enabled(Feature::CompanionNative) && Contains(u.Reason(Feature::CompanionNative), "indexBuffer"));
    CHECK(u.Enabled(Feature::QualityHooks) && u.Enabled(Feature::JitterSource));
    // one of them lost WITHOUT the table fill (the resolver found a Conflict and the table did not win): refused on the known build as well
    ResolvedBuild lost = AllFound(true);
    SetStatus(lost, indexBuffer[0], SeamStatus::Conflict, false);
    CHECK(!Run(&lost, Settings()).Enabled(Feature::CompanionNative));
    // EntryPatched is never filled from the table (the resolver cannot produce the combination; the gate refuses it anyway): a relaxed seam stays down
    ResolvedBuild patched = AllFound(true);
    {
        SeamResult& s = Mut(patched, "companion.indexBufferFlagsOffset");
        s.status = SeamStatus::EntryPatched;
        s.hasValue = false;
        s.fellBackToTable = true;
    }
    CHECK(!Run(&patched, Settings()).Enabled(Feature::CompanionNative));
    // camera.getter hooked: camera.jitterPointerOffset down, table-filled: the probe stays on on the known build, off on an unknown one
    ResolvedBuild cam = AllFound(true);
    SetStatus(cam, "camera.jitterPointerOffset", SeamStatus::NotFound, true);
    CHECK(Run(&cam, Settings()).Enabled(Feature::JitterProbe));
    ResolvedBuild camUnknown = AllFound(false);
    SetStatus(camUnknown, "camera.jitterPointerOffset", SeamStatus::NotFound);
    CHECK(!Run(&camUnknown, Settings(false, true), true, false).Enabled(Feature::JitterProbe));
    // the strict seams keep refusing a table fill on the known build: a jitter global, the camera global of the world tie, strengthFloor, a quality
    // write-path seam, a site location
    for (const char* id : { "jitterSource.settingsPointer", "jitterSource.cameraGlobal", "jitterSource.strengthFloor", "quality.managerContextOffset",
                            "quality.settingsModeOffset", "companion.batch16", "jitterSource.generator" })
    {
        ResolvedBuild rb = AllFound(true);
        SetStatus(rb, id, SeamStatus::NotFound, true);
        const GateReport g = Run(&rb, Settings());
        bool anyOff = false;
        for (const Feature f : kGroup2)
            anyOff = anyOff || !g.Enabled(f);
        if (!anyOff)
            std::printf("FAIL: a table fill of the strict seam %s was accepted on the known build\n", id);
        CHECK(anyOff);
    }
}

// The contract text of AnchorsQuality.h item 2 allows it: quality.contextCacheOffset on a KNOWN build may be
// FoundSingleAnchor through a weak dependency (a foreign hook over quality.cacheCompare 0x374f10; the +0x150/+0x154 pair is verified offline for that
// exact exe); on an unknown build it stays plain Found. A table-filled value never allows the write.
void TestContextCacheOffsetRule()
{
    auto weak = [](bool known)
    {
        ResolvedBuild rb = AllFound(known);
        SeamResult& s = Mut(rb, "quality.contextCacheOffset");
        s.status = SeamStatus::FoundSingleAnchor;
        s.viaWeakDependency = true;
        return rb;
    };
    const ResolvedBuild knownWeak = weak(true);
    const GateReport kw = Run(&knownWeak, Settings());
    CHECK(kw.Enabled(Feature::QualityHooks) && kw.Enabled(Feature::QualityRead));
    ResolvedBuild knownSingle = AllFound(true); // single anchor, NOT through a weak dependency: thinner evidence than the case the decision allows
    SetStatus(knownSingle, "quality.contextCacheOffset", SeamStatus::FoundSingleAnchor);
    CHECK(!Run(&knownSingle, Settings()).Enabled(Feature::QualityHooks));
    for (const SeamStatus st : { SeamStatus::NotFound, SeamStatus::Conflict, SeamStatus::Ambiguous, SeamStatus::EntryPatched })
    {
        ResolvedBuild filled = AllFound(true);
        SetStatus(filled, "quality.contextCacheOffset", st, st != SeamStatus::EntryPatched);
        CHECK(!Run(&filled, Settings()).Enabled(Feature::QualityHooks)); // a table-filled value does not allow the write
    }
    const ResolvedBuild unknownWeak = weak(false);
    const GateReport uw = Run(&unknownWeak, Settings(false, true), true, false);
    CHECK(!uw.Enabled(Feature::QualityHooks) && Contains(uw.Reason(Feature::QualityHooks), "plain Found") && uw.Enabled(Feature::QualityRead));
    const ResolvedBuild unknownPlain = AllFound(false);
    CHECK(Run(&unknownPlain, Settings(false, true), true, false).Enabled(Feature::QualityHooks));
    // an unknown build ignores a stray table-fill flag
    ResolvedBuild strayFill = AllFound(false);
    SetStatus(strayFill, "quality.contextCacheOffset", SeamStatus::NotFound, true);
    CHECK(!Run(&strayFill, Settings(false, true), true, false).Enabled(Feature::QualityHooks));
}

// ---- A resolver INFRASTRUCTURE failure on the exact known build falls back to the legacy decision ---------------------------------

// An image whose every read fails (a live module whose pages cannot be read): the scan aborts, every seam is NotFound.
struct UnreadableImage : ImageView
{
    explicit UnreadableImage(const DenseImage& i) { _info = i.Info(); }
    bool Read(std::uint32_t, void*, std::size_t) const override { return false; }
};
struct ThrowingImage : ImageView
{
    explicit ThrowingImage(const DenseImage& i) { _info = i.Info(); }
    bool Read(std::uint32_t, void*, std::size_t) const override { throw std::runtime_error("boom"); }
};

void TestInfrastructureFailureFallsBack()
{
    const GateSettings def = Settings();
    // the image could not be opened: no resolve result at all, on the exact known build
    {
        const GateReport g = Run(nullptr, def, true, true);
        CHECK(g.legacyFallback && g.known && !g.legacy && AllGroup1(g, true) && AllGroup2(g, true));
        CHECK(Contains(g.fallbackWhy, "image") && Contains(g.Reason(Feature::QualityHooks), "falling back") && Contains(g.Reason(Feature::QualityHooks), "image"));
        // an unknown build: off, as before
        const GateReport u = Run(nullptr, def, true, false);
        CHECK(!u.legacyFallback && AllGroup1(u, true) && AllGroup2(u, false));
        // the debug key asked for the resolver's own values: no fallback to the table
        const GateReport forced = Run(nullptr, Settings(false, false, true), true, true);
        CHECK(!forced.legacyFallback && AllGroup2(forced, false));
        // the rollback key is the legacy branch itself, not a fallback
        const GateReport leg = Run(nullptr, Settings(true), true, true);
        CHECK(leg.legacy && !leg.legacyFallback && AllGroup2(leg, true));
        // not the game: nothing
        CHECK(AllGroup1(Run(nullptr, def, false, true), false) && AllGroup2(Run(nullptr, def, false, true), false));
    }
    // a resolve result that carries a failure: a cached exception / a registry error, no seams at all, a scan that aborted
    ResolvedBuild exception = AllFound(true);
    exception.registryErrors.push_back("resolver exception: bad_alloc");
    ResolvedBuild noSeams;
    noSeams.knownBuild = true;
    ResolvedBuild scan = AllFound(true);
    scan.scanFailed = true;
    scan.scanError = "a 1 MiB window at 0x1000 could not be read";
    const char* const why[] = { "bad_alloc", "no seams", "could not be read" };
    int i = 0;
    for (ResolvedBuild* rb : { &exception, &noSeams, &scan })
    {
        const GateReport g = Run(rb, def, true, true);
        CHECK(g.legacyFallback && g.known && AllGroup1(g, true) && AllGroup2(g, true) && Contains(g.fallbackWhy, why[i]));
        ResolvedBuild unknown = *rb;
        unknown.knownBuild = false;
        const GateReport u = Run(&unknown, Settings(false, true), true, false);
        CHECK(!u.legacyFallback && AllGroup1(u, true) && AllGroup2(u, false));
        CHECK(AllGroup2(Run(&unknown, Settings(false, true, true), true, true), false)); // forced unknown on the exact exe: stays off
        ++i;
    }
    // a resolve that RAN and found nothing is not an infrastructure failure: every seam NotFound stays off (the table is only the cross-check)
    ResolvedBuild blank = AllFound(true);
    for (SeamResult& s : blank.seams)
        s.status = SeamStatus::NotFound;
    const GateReport b = Run(&blank, def, true, true);
    CHECK(!b.legacyFallback && AllGroup2(b, false));
    // ONE warning in the log block
    const auto lines = FormatGateBlock(Run(nullptr, def, true, true), nullptr, Reg(), 0x6ad10bfd, 0x381f000, 0, "ffxiv_dx11.exe");
    int warnings = 0;
    for (const GateLine& l : lines)
    {
        if (l.level == LogLevel::Warn)
        {
            ++warnings;
            CHECK(Contains(l.text, "falling back") && Contains(l.text, "image"));
        }
    }
    CHECK(warnings == 1 && lines.size() == 1 + 1 + 1 + 6);

    // the REAL resolver: an unreadable live image (every read fails) and a throwing reader, on the table's build and on another one
    auto known = jt::MakeJitterImage();
    auto other = jt::MakeJitterImage(0x6bbbbbbbu);
    REQUIRE(known != nullptr && other != nullptr);
    ResolveOptions options;
    options.knownTable = &kCurrentBuild;
    {
        const UnreadableImage bad(*known);
        const ResolvedBuild rb = ResolveImage(bad, Reg(), options);
        CHECK(rb.knownBuild && rb.scanFailed && !rb.scanError.empty() && rb.registryErrors.empty());
        const GateReport g = Run(&rb, def, true, true);
        CHECK(g.legacyFallback && AllGroup2(g, true));
        const UnreadableImage badOther(*other);
        const ResolvedBuild un = ResolveImage(badOther, Reg(), options);
        CHECK(!un.knownBuild && un.scanFailed);
        const GateReport gu = Run(&un, Settings(false, true), true, false);
        CHECK(!gu.legacyFallback && AllGroup2(gu, false));
        // and an image that reads fine does not set the flag
        const ResolvedBuild fine = ResolveImage(*known, jt::JitterRegistry(), options);
        CHECK(!fine.scanFailed && fine.scanError.empty());
    }
    {
        const ThrowingImage bad(*known);
        ResolveCache cache;
        const ResolvedBuild& thrown = cache.Get(bad, Reg(), options);
        CHECK(!thrown.registryErrors.empty() && !thrown.scanFailed);
        const GateReport g = Run(&thrown, def, true, true);
        CHECK(g.legacyFallback && AllGroup2(g, true) && Contains(g.fallbackWhy, "boom"));
        ResolveCache cacheOther;
        const ThrowingImage badOther(*other);
        const GateReport gu = Run(&cacheOther.Get(badOther, Reg(), options), Settings(false, true), true, false);
        CHECK(!gu.legacyFallback && AllGroup2(gu, false) && Contains(gu.Reason(Feature::JitterSource), "boom"));
    }
}

// ---- On an UNKNOWN build every cross-group pair a feature depends on must be compared and agree ----------------------------------

void TestUnknownBuildCrossCheckNeedsBothSides()
{
    struct LiteralPair
    {
        const char* left;
        const char* right;
        std::vector<Feature> affected;
        std::vector<Feature> ownsLeft, ownsRight; // the features that require the seam itself (their own rule turns them off when it is down)
    };
    const LiteralPair pairs[] = {
        { "chara.managerPointer", "jitterSource.rendererPointer", { Feature::CharaViewAccessor, Feature::JitterSource }, { Feature::CharaViewAccessor }, { Feature::JitterSource } },
        { "camera.global", "jitterSource.cameraGlobal", { Feature::JitterProbe, Feature::JitterSource }, { Feature::JitterProbe }, { Feature::JitterSource } },
        { "quality.settingsPointer", "jitterSource.settingsPointer", { Feature::QualityRead, Feature::QualityHooks, Feature::JitterSource },
          { Feature::QualityRead, Feature::QualityHooks }, { Feature::JitterSource } },
        { "quality.devicePointer", "jitterSource.devicePointer", { Feature::QualityHooks, Feature::JitterSource }, { Feature::QualityHooks }, { Feature::JitterSource } },
    };
    auto has = [](const std::vector<Feature>& v, Feature f) { return std::find(v.begin(), v.end(), f) != v.end(); };
    for (const LiteralPair& p : pairs)
    {
        for (const bool leftSide : { true, false })
        {
            const char* broken = leftSide ? p.left : p.right;
            const std::vector<Feature>& owns = leftSide ? p.ownsLeft : p.ownsRight;
            for (const SeamStatus st : { SeamStatus::NotFound, SeamStatus::Conflict })
            {
                // UNKNOWN build: the pair cannot be compared, so no feature that depends on it has the second derivation: all of them off
                ResolvedBuild un = AllFound(false);
                SetStatus(un, broken, st);
                const GateReport g = Run(&un, Settings(false, true), true, false);
                for (const Feature f : kGroup2)
                {
                    const bool affected = has(p.affected, f);
                    if (g.Enabled(f) == affected)
                        std::printf("FAIL: unknown build, %s %s: %s on=%d expected on=%d\n", broken, StatusName(st), FeatureName(f), g.Enabled(f), !affected);
                    CHECK(g.Enabled(f) == !affected);
                    if (affected && !has(owns, f))
                        CHECK(Contains(g.Reason(f), "cross-check") && Contains(g.Reason(f), p.left) && Contains(g.Reason(f), p.right));
                }
                bool compared = true;
                for (const CrossCheckResult& c : g.crossChecks)
                    if (c.name.rfind(p.left, 0) == 0)
                        compared = c.compared;
                CHECK(!compared);
                // KNOWN build, the side not filled from the table: unchanged, only the features that own the seam are off (today's behaviour)
                ResolvedBuild kn = AllFound(true);
                SetStatus(kn, broken, st);
                const GateReport gk = Run(&kn, Settings());
                for (const Feature f : kGroup2)
                {
                    if (gk.Enabled(f) == has(owns, f))
                        std::printf("FAIL: known build, %s %s: %s on=%d expected on=%d\n", broken, StatusName(st), FeatureName(f), gk.Enabled(f), !has(owns, f));
                    CHECK(gk.Enabled(f) == !has(owns, f));
                }
            }
        }
    }
}

// ---- entryPointerOffset is exactly 8 (the consumers index entry[1] of {key, pointer}) ----------------------------------------------

void TestEntryPointerOffsetIsEight()
{
    CompanionBuild c {};
    CHECK(c.entryPointerOffset == 8 && CompanionOffsetsInBounds(c));
    for (const std::uint32_t v : { 0u, 4u, 16u, 24u, 0x40u })
    {
        c.entryPointerOffset = v;
        CHECK(!CompanionOffsetsInBounds(c));
        ResolvedBuild rb = AllFound(false);
        rb.build.companion = c;
        const GateReport g = Run(&rb, Settings(false, true), true, false);
        CHECK(!g.Enabled(Feature::CompanionNative) && Contains(g.Reason(Feature::CompanionNative), "queue entry"));
    }
}

// ---- An offset group with unhealed members -------------------------------------------------------------------------------------

// The batch header: format and extension are healed (decoded from instructions), kind, wrapper, count and vertices keep their table values. On an
// UNKNOWN build, if a healed member moved away from its table default the struct layout moved and the unhealed siblings are suspect: the consumer
// (CompanionNative) is refused with a reason naming the group.
void TestUnhealedSiblingsOfAMovedMember()
{
    // the group table as LITERALS
    CHECK(sizeof(kOffsetGroups) / sizeof(kOffsetGroups[0]) == 1);
    const OffsetGroup& g = kOffsetGroups[0];
    CHECK(std::string(g.name) == "companion batch header" && g.feature == Feature::CompanionNative);
    std::vector<std::string> healed, unhealed;
    for (const char* k : g.healed)
        if (k != nullptr)
            healed.push_back(k);
    for (const char* k : g.unhealed)
        if (k != nullptr)
            unhealed.push_back(k);
    CHECK((healed == std::vector<std::string> { "companion.batchFormatOffset", "companion.batchExtensionOffset" }));
    CHECK((unhealed == std::vector<std::string> { "companion.batchKindOffset", "companion.batchWrapperOffset", "companion.batchCountOffset",
                                                  "companion.batchVerticesOffset" }));
    // every healed member has a seam, every unhealed one has none (what the P2 reports list)
    for (const std::string& k : healed)
    {
        bool bound = false;
        for (const Binding& b : Reg().bindings)
            bound = bound || b.key == k;
        CHECK(bound);
    }
    for (const std::string& k : unhealed)
    {
        bool bound = false;
        for (const Binding& b : Reg().bindings)
            bound = bound || b.key == k;
        CHECK(!bound);
    }
    struct Move
    {
        std::uint32_t CompanionBuild::*field;
        std::uint32_t value;
        const char* key;
    };
    const Move moves[] = { { &CompanionBuild::batchFormatOffset, 0x2c, "companion.batchFormatOffset" },
                           { &CompanionBuild::batchExtensionOffset, 6, "companion.batchExtensionOffset" } };
    // control: nothing moved, an unknown build with the key: on
    const ResolvedBuild calm = AllFound(false);
    CHECK(AllGroup2(Run(&calm, Settings(false, true), true, false), true));
    for (const Move& m : moves)
    {
        ResolvedBuild rb = AllFound(false);
        rb.build.companion.*(m.field) = m.value;
        CHECK(CompanionOffsetsInBounds(rb.build.companion)); // the bounds rule alone would pass it: the group rule is what refuses
        const GateReport r = Run(&rb, Settings(false, true), true, false);
        CHECK(!r.Enabled(Feature::CompanionNative));
        CHECK(Contains(r.Reason(Feature::CompanionNative), "companion batch header") && Contains(r.Reason(Feature::CompanionNative), m.key));
        for (const Feature f : kGroup2)
            if (f != Feature::CompanionNative)
                CHECK(r.Enabled(f)); // only the consumer that reads the group is refused
        // one WARN line names the group
        const auto lines = FormatGateBlock(r, &rb, Reg(), 1, 2, 0, "x");
        bool named = false;
        for (const GateLine& l : lines)
            if (l.level == LogLevel::Warn && Contains(l.text, "companionNative OFF") && Contains(l.text, "companion batch header"))
                named = true;
        CHECK(named);
        // the rule is for an unknown build: on the known build the table is verified offline, nothing is refused
        ResolvedBuild kn = AllFound(true);
        kn.build.companion.*(m.field) = m.value;
        CHECK(Run(&kn, Settings()).Enabled(Feature::CompanionNative));
    }
    // both moved: still one refusal naming the group
    ResolvedBuild both = AllFound(false);
    both.build.companion.batchFormatOffset = 0x2c;
    both.build.companion.batchExtensionOffset = 6;
    CHECK(!Run(&both, Settings(false, true), true, false).Enabled(Feature::CompanionNative));
}

// ---- The build a consumer reads is asked per feature -----------------------------------------------------------------------------

void TestBuildForIsPerFeature()
{
    const GameBuild b = kCurrentBuild;
    for (std::size_t on = 0; on < kFeatureCount; ++on)
    {
        GateReport g;
        g.started = true;
        g.features[on].enabled = true;
        for (std::size_t asked = 0; asked < kFeatureCount; ++asked)
        {
            const GameBuild* p = BuildFor(g, b, static_cast<Feature>(asked));
            CHECK((p != nullptr) == (asked == on));
            if (p != nullptr)
                CHECK(p == &b);
        }
    }
}

// The published state is read ONCE: the gate and the build come from the same state (BuildFor(feature) = one acquire load handed to BuildForState).
// Two separate loads could pair the table with a gate published a moment later (the order of two function arguments is unspecified).
void TestBuildForStateIsOneState()
{
    // nothing published yet: no build for any feature
    for (std::size_t asked = 0; asked < kFeatureCount; ++asked)
        CHECK(BuildForState(nullptr, static_cast<Feature>(asked)) == nullptr);
    // a published state: its OWN build (not the compile-time table) and only for the feature its gate enabled
    ext::ffxiv::detail::ActiveState state;
    state.build = ZeroedBuild();
    state.build.timeDateStamp = 0x12345678;
    state.gate.started = true;
    state.gate.features[static_cast<std::size_t>(Feature::JitterProbe)].enabled = true;
    for (std::size_t asked = 0; asked < kFeatureCount; ++asked)
    {
        const GameBuild* p = BuildForState(&state, static_cast<Feature>(asked));
        CHECK((p != nullptr) == (asked == static_cast<std::size_t>(Feature::JitterProbe)));
        if (p != nullptr)
            CHECK(p == &state.build && p->timeDateStamp == 0x12345678 && p != &kCurrentBuild);
    }
    // the process's own state: not published in this test, so nothing for any feature
    for (std::size_t asked = 0; asked < kFeatureCount; ++asked)
        CHECK(BuildFor(static_cast<Feature>(asked)) == nullptr);
}

// ---- the registry facts the gate and the consumers rely on --------------------------------------------------------------------------------

void TestRegistryFacts()
{
    // every Rva / Guard field is bound: nothing a consumer reads through ActiveBuild() stays zero on a build that resolved every seam
    CHECK(UnboundFields(Reg()).empty());
    // the sizes the consumers keep as compile-time array bounds are Constants: never resolved, identical on every build
    for (const char* key : { "companion.nativeBatchHeaderSize", "companion.drawCommandSize", "companion.nativeBatchEntrySize", "companion.rendererQueueCount",
                             "companion.rendererQueueStride", "companion.layoutSlotEntrySize", "companion.rendererVertexSlotStride", "companion.drawCommandType",
                             "jitterSource.strengthFloorBits", "jitterSource.ndcSignMaskBits" })
    {
        const FieldRow* row = FindField(key);
        REQUIRE(row != nullptr);
        CHECK(row->kind == FieldKind::Constant);
        for (const Binding& b : Reg().bindings)
            CHECK(b.key != key);
    }
    // the two constants the core reads at run time equal the values the seams check (ValueEquals), field by field
    const SeamSpec* floor = nullptr;
    const SeamSpec* sign = nullptr;
    for (const SeamSpec& s : Reg().seams)
    {
        if (s.id == "jitterSource.strengthFloor")
            floor = &s;
        if (s.id == "jitterSource.ndcSignMask")
            sign = &s;
    }
    REQUIRE(floor != nullptr && sign != nullptr);
    for (const AnchorSpec& a : floor->anchors)
        for (const Predicate& p : a.predicates)
            if (p.kind == PredicateKind::ValueIn)
                CHECK(p.valueLo == kCurrentBuild.jitterSource.strengthFloorBits && p.valueHi == kCurrentBuild.jitterSource.strengthFloorBits);
    for (const AnchorSpec& a : sign->anchors)
        for (const Predicate& p : a.predicates)
            if (p.kind == PredicateKind::ValueIn)
                CHECK(p.valueLo == kCurrentBuild.jitterSource.ndcSignMaskBits && p.valueHi == kCurrentBuild.jitterSource.ndcSignMaskBits);
    float f = 0;
    const std::uint32_t bits = kCurrentBuild.jitterSource.strengthFloorBits;
    std::memcpy(&f, &bits, sizeof(f));
    CHECK(f == 0.001f);
    // the product registry the gate was written against
    CHECK(Reg().seams.size() == 81 && Reg().bindings.size() == 77); // the brief said "78 seams": the registry has 81 (the three shape seams counted)
    std::printf("registry: %zu seams, %zu bindings\n", Reg().seams.size(), Reg().bindings.size());
}

// ---- ActiveBuild --------------------------------------------------------------------------------------------------------------------------

void TestActiveBuild()
{
    // before the startup: the table, and a gate that says "not started" (every feature off)
    CHECK(!ActiveStarted());
    CHECK(&ActiveBuild() == &kCurrentBuild);
    CHECK(!ActiveGate().started);
    for (std::size_t i = 0; i < kFeatureCount; ++i)
        CHECK(!ActiveGate().features[i].enabled && Contains(ActiveGate().features[i].reason, "not run yet"));
    CHECK(!FeatureEnabled(Feature::QualityHooks));

    // publish: the build and the report become the process's; the first publish wins
    GameBuild build = kCurrentBuild;
    build.jitterSource.generatorRva = 0x123456;
    const ResolvedBuild rb = AllFound(true);
    const GateReport g = Run(&rb, Settings());
    CHECK(PublishActive(build, g));
    CHECK(ActiveStarted() && &ActiveBuild() != &kCurrentBuild);
    CHECK(ActiveBuild().jitterSource.generatorRva == 0x123456 && ActiveBuild().companion.batch16Rva == kCurrentBuild.companion.batch16Rva);
    CHECK(ActiveGate().started && FeatureEnabled(Feature::QualityHooks) && FeatureEnabled(Feature::CharaViewAccessor));
    // the one-state read hands out the published build itself, for the features its gate enabled
    CHECK(BuildFor(Feature::QualityHooks) == &ActiveBuild() && BuildFor(Feature::CharaViewAccessor) == &ActiveBuild());
    GameBuild other = kCurrentBuild;
    other.jitterSource.generatorRva = 0x654321;
    CHECK(!PublishActive(other, GateReport {}));
    CHECK(ActiveBuild().jitterSource.generatorRva == 0x123456 && FeatureEnabled(Feature::QualityHooks));
}
} // namespace

int main()
{
    TestExeName();
    TestGroup1AndModes();
    TestFailOpen();
    TestEveryRequiredSeam();
    TestFeaturesAreIndependent();
    TestSiteTablesAreLiteral();
    TestRequiredSeamListsAreLiteral();
    TestOneRuleForEverySite();
    TestTableFilledLocationYieldsUnusableShape();
    TestQualityRules();
    TestJitterRules();
    TestCompanionRules();
    TestCharaRules();
    TestEntryPatchedIsDistinct();
    TestCrossChecks();
    TestNotSelfHealing();
    TestLogBlock();
    TestGateBlockSixStates();
    TestGroup1ReasonNamesIdentity();
    TestHelperHookOnKnownBuild();
    TestContextCacheOffsetRule();
    TestInfrastructureFailureFallsBack();
    TestUnknownBuildCrossCheckNeedsBothSides();
    TestEntryPointerOffsetIsEight();
    TestUnhealedSiblingsOfAMovedMember();
    TestBuildForIsPerFeature();
    TestBuildForStateIsOneState(); // before TestActiveBuild: it needs the process state unpublished
    TestRegistryFacts();
    TestActiveBuild(); // last: it publishes the process-wide state
    return TEST_RESULT("gate_rules");
}
