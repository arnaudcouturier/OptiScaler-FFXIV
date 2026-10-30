// The gate on the REAL build-C executable (analysis\patch-20261016\ffxiv_dx11.exe, never committed): the "known build + default ini = identical
// to today" proof and the "survives a patch" proof, driven by real ResolveImage results.
//
//   1. KNOWN build, default keys: every seam is plain Found, nothing came from the table, the gate enables every feature (the full enabled set is a
//      literal below), and the build the consumers will read (ActiveBuild = ResolvedBuild::build) equals kCurrentBuild row by row for every field of
//      the field table, of every kind.
//   2. FORCED UNKNOWN (the resolver is given no table: the whole patch-day path, resolver values only): every seam resolves usable, the four
//      cross-group checks compare and agree, Group 2 is OFF with the key off and enables exactly the set of the known path with the key on, and the
//      resolved build equals the table field by field (Rva, Guard and Offset rows).
//   3. LEGACY: the rollback key enables the same set on the exact build and nothing on an unknown one.
//   4. Third-party hooks over helper functions the fork never touches (a 5-byte E9
//      over quality.cacheCompare; a hook over camera.getter), each on the known build and on the forced-unknown path; a resolver INFRASTRUCTURE failure
//      on the real image (an unreadable window, an unreadable image) falls back to the legacy decision; and the whole startup (RunStartup, a fake
//      environment around the REAL resolver) publishes the table row by row on the known build and after the fallback.
// Also printed: the resolve time of the unoptimised test build.
//
// If the executable is not there the test prints "SKIP:" and exits 0 (tools\test.ps1 has no skip state: a gate log must not contain "SKIP:").
#include "resolve_test_util.h"

#include "ext/ffxiv/ActiveBuild.h"
#include "ext/ffxiv/chara/CharaTable.h"
#include "ext/ffxiv/resolve/Entry.h"
#include "ext/ffxiv/resolve/Gate.h"
#include "ext/ffxiv/resolve/Registry.h"
#include "ext/ffxiv/resolve/Resolver.h"
#include "ext/ffxiv/resolve/StartupDecision.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

using namespace rt;
using namespace ext::ffxiv;

namespace
{
std::string EnabledSet(const GateReport& g)
{
    std::string s;
    for (std::size_t i = 0; i < kFeatureCount; ++i)
    {
        if (g.features[i].enabled)
            s += (s.empty() ? "" : ",") + std::string(FeatureName(static_cast<Feature>(i)));
    }
    return s;
}

// reads fail inside [lo, hi) (a window of the live module that cannot be read); everything else is the real image
struct FailingWindowImage : ImageView
{
    FailingWindowImage(const DenseImage& i, std::uint32_t from, std::uint32_t to) : inner(i), lo(from), hi(to) { _info = i.Info(); }
    bool Read(std::uint32_t rva, void* out, std::size_t bytes) const override
    {
        if (rva < hi && static_cast<std::uint64_t>(rva) + bytes > lo)
            return false;
        return inner.Read(rva, out, bytes);
    }
    const DenseImage& inner;
    std::uint32_t lo, hi;
};

// Patches `bytes` at `rva` for the duration of `f`, then restores the original bytes.
template <class F> void WithBytes(DenseImage& img, std::uint32_t rva, const std::vector<std::uint8_t>& bytes, F&& f)
{
    std::vector<std::uint8_t> saved(bytes.size());
    if (!img.Read(rva, saved.data(), saved.size()) || !img.Put(rva, bytes))
    {
        std::printf("FAIL: cannot patch %zu bytes at 0x%x\n", bytes.size(), rva);
        ++g_failures;
        return;
    }
    f();
    img.Put(rva, saved);
}

bool Contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

std::string Without(const std::string& all, const char* name)
{
    std::string out;
    std::size_t at = 0;
    while (at <= all.size())
    {
        std::size_t comma = all.find(',', at);
        if (comma == std::string::npos)
            comma = all.size();
        const std::string item = all.substr(at, comma - at);
        if (item != name && !item.empty())
            out += (out.empty() ? "" : ",") + item;
        at = comma + 1;
    }
    return out;
}

// every row of the field table, compared byte for byte (not memcmp over the struct: padding)
int DifferingRows(const GameBuild& a, const GameBuild& b, bool skipPattern, std::string* first)
{
    int n = 0;
    for (const FieldRow& row : kFieldTable)
    {
        if (skipPattern && row.kind == FieldKind::Pattern)
            continue;
        if (std::memcmp(FieldBytes(a, row), FieldBytes(b, row), row.size) != 0)
        {
            char key[96];
            FieldKey(row, key, sizeof(key));
            if (n++ == 0 && first != nullptr)
                *first = key;
        }
    }
    return n;
}
} // namespace

int main(int argc, char** argv)
{
    const std::string path = argc > 1 ? argv[1] : "ffxiv_dx11.exe";
    {
        std::ifstream probe(path, std::ios::binary);
        if (!probe)
        {
            std::printf("SKIP: exe not found: %s\n", path.c_str());
            return 0;
        }
    }
    std::string error;
    const auto image = DenseImage::FromFile(path, &error);
    if (!image)
    {
        std::printf("FAIL: cannot map %s: %s\n", path.c_str(), error.c_str());
        return 1;
    }
    const GameBuild& table = kCurrentBuild;
    if (image->TimeDateStamp() != table.timeDateStamp || image->SizeOfImage() != table.sizeOfImage)
    {
        std::printf("SKIP: %s is not the table's build (stamp 0x%x, size 0x%x)\n", path.c_str(), image->TimeDateStamp(), image->SizeOfImage());
        return 0;
    }

    const Registry registry = BuildRegistry();
    CHECK(ValidateRegistry(registry).empty());
    std::printf("registry: %zu seams\n", registry.seams.size());

    GateInput in;
    in.exeIsFfxiv = true;
    in.exactKnownBuild = true;
    in.registry = &registry;

    // ---- 1. known build, default keys
    ResolveOptions knownOptions;
    knownOptions.knownTable = &table;
    LogCapture knownLog;
    knownOptions.sink = knownLog.Sink();
    const auto t0 = std::chrono::steady_clock::now();
    const ResolvedBuild known = ResolveImage(*image, registry, knownOptions);
    const double knownMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("known resolve: %.1f ms (unoptimised test build; scan %.1f ms)\n", knownMs, known.scanMicros / 1000.0);
    CHECK(known.knownBuild && known.registryErrors.empty());
    CHECK(known.Count(SeamStatus::Found) == registry.seams.size());
    for (const SeamResult& s : known.seams)
    {
        if (s.status != SeamStatus::Found || s.fellBackToTable || s.agreement == Agreement::Disagree)
            std::printf("FAIL: seam %s is %s (fellBack=%d agreement=%d)\n", s.id.c_str(), StatusName(s.status), s.fellBackToTable, static_cast<int>(s.agreement));
        CHECK(s.status == SeamStatus::Found && !s.fellBackToTable && s.agreement != Agreement::Disagree);
    }
    // ActiveBuild on the known build = the table, every row of every kind
    std::string first;
    CHECK(DifferingRows(known.build, table, false, &first) == 0);
    if (!first.empty())
        std::printf("FAIL: the resolved build differs from the table at %s\n", first.c_str());
    CHECK(known.build.timeDateStamp == table.timeDateStamp && known.build.sizeOfImage == table.sizeOfImage);
    CHECK(FormatEntry(known, registry) == FormatTableEntry(table, registry));

    in.rb = &known;
    const GateReport knownGate = Decide(in, GateSettings {});
    const std::string kAll = "sceneHdr,lighting,uiHudless,geometry,markers,mipBind,quirkIsolatesShutdown,quirkBridgedNativeDx11,catalogHealth,qualityRead,"
                             "qualityHooks,jitterProbe,jitterSource,charaViewAccessor,latencyTick";
    CHECK(EnabledSet(knownGate) == kAll);
    if (EnabledSet(knownGate) != kAll)
    {
        for (std::size_t i = 0; i < kFeatureCount; ++i)
            if (!knownGate.features[i].enabled)
                std::printf("  off: %s: %s\n", FeatureName(static_cast<Feature>(i)), knownGate.features[i].reason.c_str());
    }
    CHECK(knownGate.known && !knownGate.legacy && !knownGate.forcedUnknown);
    CHECK(knownGate.crossChecks.size() == 4);
    for (const CrossCheckResult& c : knownGate.crossChecks)
        CHECK(c.compared && c.agree);
    // the unknown-build key changes nothing on the known build
    GateSettings keyOn;
    keyOn.unknownBuildHooks = true;
    CHECK(EnabledSet(Decide(in, keyOn)) == kAll);

    // ---- 2. the forced-unknown path: the resolver has no table
    ResolveOptions forcedOptions;
    LogCapture forcedLog;
    forcedOptions.sink = forcedLog.Sink();
    const auto t1 = std::chrono::steady_clock::now();
    const ResolvedBuild forced = ResolveImage(*image, registry, forcedOptions);
    const double forcedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
    std::printf("forced-unknown resolve: %.1f ms\n", forcedMs);
    CHECK(!forced.knownBuild && forced.registryErrors.empty());
    CHECK(forced.Count(SeamStatus::Found) == registry.seams.size()); // all of them, from the executable's bytes alone
    for (const SeamResult& s : forced.seams)
        CHECK(s.status == SeamStatus::Found && !s.fellBackToTable && !s.hasTable);
    std::string firstForced;
    const int diff = DifferingRows(forced.build, table, false, &firstForced);
    if (diff != 0)
        std::printf("FAIL: the forced-unknown build differs from the table in %d rows, first %s\n", diff, firstForced.c_str());
    CHECK(diff == 0);
    CHECK(FormatEntry(forced, registry) == FormatTableEntry(table, registry));

    GateSettings forcedKeyOff;
    forcedKeyOff.forceUnknownBuild = true;
    GateSettings forcedKeyOn = forcedKeyOff;
    forcedKeyOn.unknownBuildHooks = true;
    in.rb = &forced;
    const GateReport off = Decide(in, forcedKeyOff);
    const GateReport on = Decide(in, forcedKeyOn);
    CHECK(off.forcedUnknown && !off.known && on.forcedUnknown && !on.known);
    // key off: Group 1 on, Group 2 off. key on: the same enabled set as the known path ("survives a patch")
    for (std::size_t i = 0; i < kFeatureCount; ++i)
        CHECK(off.features[i].enabled == !IsGroup2(static_cast<Feature>(i)));
    CHECK(EnabledSet(on) == kAll);
    if (EnabledSet(on) != kAll)
    {
        for (std::size_t i = 0; i < kFeatureCount; ++i)
            if (!on.features[i].enabled)
                std::printf("  off: %s: %s\n", FeatureName(static_cast<Feature>(i)), on.features[i].reason.c_str());
    }
    CHECK(on.crossChecks.size() == 4);
    for (const CrossCheckResult& c : on.crossChecks)
        CHECK(c.compared && c.agree);
    // the not-self-healing line names the table offsets of an unknown build
    const std::string line = FormatNotSelfHealing(forced, registry);
    std::printf("%s\n", line.c_str());

    // ---- CharaView: the module keeps its OWN scan of the executable; its result must equal the resolved seam on the known build and on the forced-unknown
    // one (CharaTable.cpp cross-checks it with ActiveBuild().charaView the same way)
    {
        constexpr std::uintptr_t kBase = 0x140000000ull;
        auto reader = [&](std::uintptr_t address, void* out, std::size_t bytes)
        { return address >= kBase && address - kBase <= 0xffffffffull && image->Read(static_cast<std::uint32_t>(address - kBase), out, bytes); };
        for (const ResolvedBuild* rb : { &known, &forced })
        {
            const auto res = ext::ffxiv::chara::table::ResolveFromImage(reader, kBase, rb->build.charaView);
            const SeamResult* accessor = rb->Find("chara.accessor");
            const SeamResult* manager = rb->Find("chara.managerPointer");
            CHECK(accessor != nullptr && manager != nullptr);
            CHECK(res.state == ext::ffxiv::chara::table::AccessorState::Ready);
            if (accessor != nullptr && manager != nullptr)
                CHECK(res.accessorRva == accessor->rva && res.managerPointerRva == manager->rva);
        }
    }

    // ---- 3. the rollback key: everything the key restores, except the simulation-tick lease (never part of it)
    GateInput legacyIn = in;
    legacyIn.rb = nullptr;
    GateSettings legacy;
    legacy.legacyGameBuild = true;
    CHECK(EnabledSet(Decide(legacyIn, legacy)) == Without(kAll, "latencyTick"));
    CHECK(!Decide(legacyIn, legacy).Enabled(Feature::LatencyTick));
    legacyIn.exactKnownBuild = false;
    CHECK(EnabledSet(Decide(legacyIn, legacy)).empty());

    // ---- 4. Foreign hooks over helper functions, a failing resolver, the whole startup on the real image
    DenseImage& img = *image;
    auto seamRva = [&](const char* id)
    {
        const SeamResult* s = known.Find(id);
        CHECK(s != nullptr && s->hasValue);
        return s != nullptr ? s->rva : 0u;
    };
    const std::uint32_t cameraGetter = seamRva("camera.getter");
    const std::uint32_t cacheCompare = seamRva("quality.cacheCompare");
    CHECK(cameraGetter == 0x25f610 && cacheCompare == 0x374f10);
    const std::vector<std::uint8_t> ff25 = { 0xff, 0x25, 0, 0, 0, 0, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11 }; // jmp [rip+0]: 14 bytes
    const std::vector<std::uint8_t> e9 = { 0xe9, 0x00, 0x10, 0x00, 0x00 };                                              // jmp rel32: 5 bytes
    GateInput knownIn = in;
    knownIn.exeIsFfxiv = true;
    knownIn.exactKnownBuild = true;
    GateSettings forcedOn;
    forcedOn.forceUnknownBuild = true;
    forcedOn.unknownBuildHooks = true;
    auto resolveWith = [&](bool withTable)
    {
        ResolveOptions o;
        o.knownTable = withTable ? &table : nullptr;
        return ResolveImage(*image, registry, o);
    };

    // A 5-byte E9 over quality.cacheCompare: contextCacheOffset becomes a single anchor through a weak dependency
    WithBytes(img, cacheCompare, e9,
              [&]
              {
                  const ResolvedBuild k = resolveWith(true);
                  const SeamResult* s = k.Find("quality.contextCacheOffset");
                  CHECK(s != nullptr && s->status == SeamStatus::FoundSingleAnchor && s->viaWeakDependency && !s->fellBackToTable);
                  knownIn.rb = &k;
                  const GateReport g = Decide(knownIn, GateSettings {});
                  if (EnabledSet(g) != kAll)
                      std::printf("FAIL: a foreign E9 over quality.cacheCompare turned off: %s\n", g.Reason(Feature::QualityHooks).c_str());
                  CHECK(EnabledSet(g) == kAll);
                  CHECK(DifferingRows(k.build, table, false, nullptr) == 0);
                  const ResolvedBuild u = resolveWith(false);
                  GateInput unknownIn = in;
                  unknownIn.rb = &u;
                  unknownIn.exactKnownBuild = true;
                  const GateReport gu = Decide(unknownIn, forcedOn);
                  CHECK(EnabledSet(gu) == Without(kAll, "qualityHooks") && Contains(gu.Reason(Feature::QualityHooks), "contextCacheOffset") &&
                        gu.Enabled(Feature::QualityRead));
              });
    // a hook over camera.getter: the probe's camera seams fall back to the table and the probe stays on; the jitter source (its own getter site is
    // the same function: EntryPatched, never hooked) goes off on the known build, as the old guard block did
    WithBytes(img, cameraGetter, e9,
              [&]
              {
                  const ResolvedBuild k = resolveWith(true);
                  knownIn.rb = &k;
                  const GateReport g = Decide(knownIn, GateSettings {});
                  CHECK(g.Enabled(Feature::JitterProbe) && g.Enabled(Feature::QualityHooks) && g.Enabled(Feature::CharaViewAccessor));
                  if (g.Enabled(Feature::JitterSource))
                      std::printf("note: jitterSource stays on with a hook over camera.getter\n");
                  CHECK(!g.Enabled(Feature::JitterSource) && Contains(g.Reason(Feature::JitterSource), "foreign hook"));
              });
    CHECK(DifferingRows(resolveWith(true).build, table, false, nullptr) == 0); // the patch was restored

    // F3 on the real image: a window of the code section that cannot be read, and an image that cannot be read at all
    {
        const FailingWindowImage windowed(img, 0x400000, 0x500000);
        ResolveOptions o;
        o.knownTable = &table;
        const ResolvedBuild w = ResolveImage(windowed, registry, o);
        CHECK(w.knownBuild && w.scanFailed && !w.scanError.empty() && w.registryErrors.empty());
        knownIn.rb = &w;
        const GateReport g = Decide(knownIn, GateSettings {});
        CHECK(g.legacyFallback && g.known && EnabledSet(g) == kAll);
        // the same on the forced-unknown path, and on a build that is not the exact one: off
        ResolveOptions none;
        const ResolvedBuild wu = ResolveImage(windowed, registry, none);
        GateInput unknownIn = in;
        unknownIn.rb = &wu;
        unknownIn.exactKnownBuild = true;
        CHECK(!Decide(unknownIn, forcedOn).legacyFallback && EnabledSet(Decide(unknownIn, forcedOn)) == "sceneHdr,lighting,uiHudless,geometry,markers,mipBind,quirkIsolatesShutdown,quirkBridgedNativeDx11,catalogHealth");
        unknownIn.exactKnownBuild = false;
        CHECK(!Decide(unknownIn, GateSettings {}).legacyFallback && !Decide(unknownIn, GateSettings {}).Enabled(Feature::QualityHooks));
    }

    // the whole startup around the REAL resolver (a fake environment: the image is the real exe, the log and publish are captured)
    {
        struct Run
        {
            GameBuild published;
            GateReport gate;
            int publishes = 0;
        };
        auto startup = [&](const ImageView& view, bool exactStampSize, const GateSettings& settings)
        {
            Run run;
            ResolvedBuild holder;
            StartupEnv env;
            env.readSettings = [&] { return settings; };
            env.exePath = [] { return std::wstring(L"C:\\Games\\FFXIV\\game\\ffxiv_dx11.exe"); };
            env.stampSizeMatchesKnown = [&] { return exactStampSize; };
            env.registry = [&]() -> const Registry& { return registry; };
            env.resolve = [&](const GameBuild* offered, double* ms) -> const ResolvedBuild*
            {
                ResolveOptions o;
                o.knownTable = offered;
                holder = ResolveImage(view, registry, o);
                *ms = 1.0;
                return &holder;
            };
            env.log = [](const StartupLog&) {};
            env.publish = [&](const GameBuild& b, const GateReport& g)
            {
                run.published = b;
                run.gate = g;
                ++run.publishes;
            };
            RunStartup(env);
            return run;
        };
        // known build, default keys: every feature, the table row by row (the build is the resolved one and it equals the table)
        const Run good = startup(img, true, GateSettings {});
        CHECK(good.publishes == 1 && EnabledSet(good.gate) == kAll && DifferingRows(good.published, table, false, nullptr) == 0 && !good.gate.legacyFallback);
        // the resolver fails on the exact known build: the same features and the same table through the legacy fallback
        const FailingWindowImage windowed(img, 0x400000, 0x500000);
        const Run fell = startup(windowed, true, GateSettings {});
        CHECK(fell.publishes == 1 && fell.gate.legacyFallback && EnabledSet(fell.gate) == kAll && DifferingRows(fell.published, table, false, nullptr) == 0);
        // the same failure on a build that is not the exact one: off, and zeros are published, not the table
        const Run other = startup(windowed, false, GateSettings {});
        CHECK(other.publishes == 1 && !other.gate.legacyFallback && !other.gate.Enabled(Feature::QualityHooks) && other.gate.Enabled(Feature::Lighting));
        CHECK(other.published.jitterSource.generatorRva == 0 && other.published.nativeQuality.querySizeRva == 0);
    }

    return TEST_RESULT("gate_real_exe");
}
