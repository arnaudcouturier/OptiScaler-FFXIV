// The simulation-tick anchors (ext/ffxiv/resolve/AnchorsLatency.h) on the two real executables, resolved and compared value by value:
//
//   * analysis\patch-20261016\ffxiv_dx11.exe          the table's build (2026-10-15) - skipped when it is not there
//   * tools-downloads\ffxiv-benchmark\game\ffxiv_dx11.exe  the 2024-05-30 benchmark build, read only - skipped when it is not there
//
// For each one: the five seams resolve from the bytes alone (no table), the values are the ones the two builds are known to have, both interior
// anchors of the tick agree, the three counter offsets have the layout the tick's code encodes, every anchor passes the offline margin rule, and
// the static vtable of the Framework object still holds the tick in slot 4 (found in read-only data at image base + RVA, the pointer the install
// compares against).
//
// On the table's build the resolver's cross-check must agree, and three in-memory decoys prove what the design relies on:
//   * a byte moved inside the clock block: the teardown anchor alone finds the tick (FoundSingleAnchor, still usable) and the two QueryPerformance
//     counter offsets become unusable - the feature stays ON, because the counters are telemetry,
//   * a foreign inline hook over the tick entry (a 5-byte E9, what Dalamud writes): the interior anchors still find the tick, the seam is Found and
//     NOT EntryPatched, and the frame-counter offset (the one anchor that starts at the entry) is the only thing lost,
//   * both interior blocks broken: NotFound, and the gate turns the feature off.
#include "resolve_test_util.h"

#include "Margin.h"
#include "ext/ffxiv/resolve/Gate.h"
#include "ext/ffxiv/resolve/Registry.h"
#include "ext/ffxiv/resolve/Resolver.h"
#include "ext/latency/TickLeaseCore.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace rt;
using namespace ext::ffxiv;
using namespace ext::ffxiv::resolve;

namespace
{
constexpr std::uint64_t kPreferredBase = 0x140000000ull; // FFXIV's link-time image base, which is what the .rdata table stores

struct Expected
{
    const char* name;
    std::uint32_t tick, instance, frameCounter, perfValue, perfFreq;
};

constexpr Expected kLive { "live 2026-10-15", 0xd1a40, 0x2907928, 0x16d0, 0x16b0, 0x16a8 };
constexpr Expected kBench { "benchmark 2024-05-30", 0x9e140, 0x258d518, 0x16c8, 0x16a8, 0x16a0 };

const SeamResult* Get(const ResolvedBuild& rb, const char* id)
{
    const SeamResult* s = rb.Find(id);
    if (s == nullptr)
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
        std::printf("FAIL: %s is %s at 0x%x, expected Found at 0x%x (%s)\n", id, StatusName(s->status), s->rva, want, s->reason.c_str());
    CHECK(s->status == SeamStatus::Found);
    CHECK(s->rva == want);
}

// The Framework table: exactly one 8-byte pointer to the tick in read-only, non-writable data, and it is slot 4 (the four slots before it are other
// functions of the image).
void CheckVtable(const DenseImage& img, const Expected& want)
{
    const std::uint64_t target = kPreferredBase + want.tick;
    std::uint32_t at = 0;
    int hits = 0;
    for (const ImageSection& s : img.Sections())
    {
        if (s.Executable() || s.Writable() || !s.Readable())
            continue;
        for (std::uint32_t off = 0; off + 8 <= s.virtualSize; off += 8)
        {
            std::uint64_t v = 0;
            if (!img.Read(s.rva + off, &v, sizeof(v)))
                continue;
            if (v == target)
            {
                if (hits++ == 0)
                    at = s.rva + off;
            }
        }
    }
    if (hits != 1)
        std::printf("FAIL: %d read-only slots hold the tick (0x%llx), expected exactly 1\n", hits, static_cast<unsigned long long>(target));
    CHECK(hits == 1);
    if (hits != 1)
        return;
    CHECK(at % 8 == 0 && at >= 4 * 8);
    for (int k = 1; k <= 4; ++k)
    {
        std::uint64_t v = 0;
        CHECK(img.Read(at - 8 * k, &v, sizeof(v)));
        const std::uint64_t rva = v - kPreferredBase;
        CHECK(v != target); // the other slots are other functions
        CHECK(img.Info().sizeOfImage > rva && InRegion(img.Info(), RegionKind::Code, static_cast<std::uint32_t>(rva)));
    }
    std::printf("  %s: vtable slot 4 at 0x%x holds the tick\n", want.name, at);
}

void CheckBuild(const DenseImage& img, const Expected& want)
{
    std::printf("== %s (stamp 0x%x, size 0x%x)\n", want.name, img.TimeDateStamp(), img.SizeOfImage());
    const Registry registry = BuildRegistry();
    CHECK(ValidateRegistry(registry).empty());

    // 1. the five seams, from the bytes alone
    const ResolvedBuild pure = ResolveImage(img, registry);
    CHECK(!pure.knownBuild && pure.registryErrors.empty() && !pure.scanFailed);
    CheckValue(pure, "latency.tick", want.tick);
    CheckValue(pure, "latency.frameworkInstance", want.instance);
    CheckValue(pure, "latency.frameCounterOffset", want.frameCounter);
    CheckValue(pure, "latency.perfCounterValueOffset", want.perfValue);
    CheckValue(pure, "latency.perfCounterFreqOffset", want.perfFreq);

    // both interior anchors of the tick found the same function (plain Found, not FoundSingleAnchor)
    const SeamResult* tick = Get(pure, "latency.tick");
    REQUIRE(tick != nullptr);
    REQUIRE(tick->anchors.size() == 2);
    CHECK(tick->anchors[0].outcome == AnchorOutcome::Unique && tick->anchors[1].outcome == AnchorOutcome::Unique);
    CHECK(tick->anchors[0].value == want.tick && tick->anchors[1].value == want.tick);
    CHECK(pure.Find("latency.tick")->status == SeamStatus::Found && !pure.Find("latency.tick")->viaWeakDependency);

    // the layout the install's third check compares
    const LatencyBuild& b = pure.build.latency;
    CHECK(ext::latency::LayoutConsistent(b.frameCounterOffset, b.perfCounterValueOffset, b.perfCounterFreqOffset));
    std::printf("  offsets: frameCounter 0x%x, perf value 0x%x, perf freq 0x%x\n", b.frameCounterOffset, b.perfCounterValueOffset,
                b.perfCounterFreqOffset);

    // 2. the offline margin rule of every anchor of the group, on the real bytes
    for (const SeamSpec& seam : registry.seams)
    {
        if (seam.id.rfind("latency.", 0) != 0)
            continue;
        const SeamResult* s = pure.Find(seam.id);
        REQUIRE(s != nullptr);
        for (std::size_t i = 0; i < seam.anchors.size() && i < s->anchors.size(); ++i)
        {
            // found == false is the good case here: no other site lies within the cap, so the distance is cap + 1 and every required margin holds.
            const auto m = offline::AnchorMargin(img, seam.anchors[i], s->anchors[i]);
            std::printf("  %s anchor '%s': nearest other site %s distance=%u sites=%u required=%u\n", seam.id.c_str(), seam.anchors[i].name.c_str(), m.found ? "yes" : "none within cap", m.distance, m.sitesAtDistance, seam.anchors[i].minMargin);
            if (!offline::MarginOk(m, seam.anchors[i].minMargin))
                std::printf("FAIL: %s anchor '%s': nearest other site 0x%x is %u fixed bytes away (%u at that distance)\n", seam.id.c_str(),
                            seam.anchors[i].name.c_str(), m.rva, m.distance, m.sitesAtDistance);
            CHECK(offline::MarginOk(m, seam.anchors[i].minMargin));
        }
    }

    // 3. the static identity the install checks live: slot 4 of the Framework vtable
    CheckVtable(img, want);

    // 4. the gate: a build the table does not know runs the lease only with the unknown-build key, and the bench is such a build
    GateInput in;
    in.exeIsFfxiv = true;
    in.exactKnownBuild = img.TimeDateStamp() == kCurrentBuild.timeDateStamp && img.SizeOfImage() == kCurrentBuild.sizeOfImage;
    in.rb = &pure;
    in.registry = &registry;
    GateSettings off;
    off.forceUnknownBuild = true;
    GateSettings on = off;
    on.unknownBuildHooks = true;
    CHECK(!Decide(in, off).Enabled(Feature::LatencyTick));
    if (!in.exactKnownBuild)
    {
        const GateReport unknown = Decide(in, on);
        CHECK(unknown.Enabled(Feature::LatencyTick));
        // every telemetry offset resolved on this build too, so the lease may read all three
        CHECK(unknown.latencyOffsetsResolved == kLatencyAllOffsetsResolved);
        std::printf("  unknown build, key on: latencyTick ON (%s), offsets 0x%x\n", unknown.Reason(Feature::LatencyTick).c_str(),
                    unknown.latencyOffsetsResolved);
    }

    // 5. the table's own build: the cross-check agrees and the resolved build equals the table
    if (in.exactKnownBuild)
    {
        ResolveOptions options;
        options.knownTable = &kCurrentBuild;
        LogCapture log;
        options.sink = log.Sink();
        const ResolvedBuild known = ResolveImage(img, registry, options);
        CHECK(known.knownBuild);
        in.rb = &known;
        for (const SeamResult& s : known.seams)
        {
            if (s.id.rfind("latency.", 0) != 0)
                continue;
            CHECK(s.status == SeamStatus::Found && s.agreement == Agreement::Agree && !s.fellBackToTable);
        }
        CHECK(known.build.latency.tickRva == kCurrentBuild.latency.tickRva);
        CHECK(known.build.latency.frameworkInstanceRva == kCurrentBuild.latency.frameworkInstanceRva);
        CHECK(known.build.latency.frameCounterOffset == kCurrentBuild.latency.frameCounterOffset);
        CHECK(known.build.latency.perfCounterValueOffset == kCurrentBuild.latency.perfCounterValueOffset);
        CHECK(known.build.latency.perfCounterFreqOffset == kCurrentBuild.latency.perfCounterFreqOffset);
        const GateReport gate = Decide(in, GateSettings {});
        CHECK(gate.Enabled(Feature::LatencyTick));
        CHECK(gate.Reason(Feature::LatencyTick).find("2 required checks") != std::string::npos);
        // the per-offset resolved mask the install uses: all three on a clean resolve, never a blind read of an offset that did not resolve
        CHECK(gate.latencyOffsetsResolved == kLatencyAllOffsetsResolved);
    }
}

// In-memory patches of the real image, restored afterwards.
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

// The three decoys, on the table's own build only (the values are its values).
void CheckDecoys(DenseImage& img, const Expected& want)
{
    const Registry registry = BuildRegistry();
    ResolveOptions options;
    options.knownTable = &kCurrentBuild;
    // +0x1a from the entry: the first fixed byte of the clock block (`48 8B 44 24 30`, the `mov rax,[rsp+0x30]`)
    const std::uint32_t clock = want.tick + 0x1a;
    const std::uint32_t teardown = want.tick + 0x385; // inside the tail block (`80 7B 08 00`)

    // (a) one byte moved inside the clock block: the teardown anchor alone finds the tick, the two counter offsets are lost, the feature stays on
    WithBytes(img, clock + 1, { 0x00 },
              [&]
              {
                  const ResolvedBuild rb = ResolveImage(img, registry, options);
                  const SeamResult* tick = Get(rb, "latency.tick");
                  REQUIRE(tick != nullptr);
                  CHECK(tick->status == SeamStatus::FoundSingleAnchor && tick->rva == want.tick);
                  CHECK(tick->anchors[0].outcome == AnchorOutcome::Zero && tick->anchors[1].outcome == AnchorOutcome::Unique);
                  // the two QueryPerformanceCounter offsets are decoded from that block: unusable, and filled from the table on this build
                  for (const char* id : { "latency.perfCounterValueOffset", "latency.perfCounterFreqOffset" })
                  {
                      const SeamResult* s = Get(rb, id);
                      REQUIRE(s != nullptr);
                      CHECK(s->status == SeamStatus::NotFound && s->fellBackToTable);
                      CHECK(rb.UsableOrTable(id) && !rb.Usable(id));
                  }
                  // the frame-counter offset has its own anchor and still resolves
                  CHECK(rb.Usable("latency.frameCounterOffset"));
                  GateInput in;
                  in.exeIsFfxiv = true;
                  in.exactKnownBuild = true;
                  in.rb = &rb;
                  in.registry = &registry;
                  const GateReport g = Decide(in, GateSettings {});
                  CHECK(g.Enabled(Feature::LatencyTick)); // the counters are telemetry: an unusable one must not turn the boundary off
                  // on the exact known build the two lost offsets are filled from the table (UsableOrTable), which is verified against this
                  // executable: they count as resolved, and the gate can read them
                  CHECK(g.latencyOffsetsResolved == kLatencyAllOffsetsResolved);
              });

    // (b) a foreign inline hook over the entry (5-byte E9): the interior anchors still find the function, the seam is NOT EntryPatched, and only the
    // frame-counter offset (whose anchor starts at the entry) is lost
    const std::vector<std::uint8_t> e9 = { 0xe9, 0x00, 0x10, 0x00, 0x00 };
    WithBytes(img, want.tick, e9,
              [&]
              {
                  const ResolvedBuild rb = ResolveImage(img, registry, options);
                  const SeamResult* tick = Get(rb, "latency.tick");
                  REQUIRE(tick != nullptr);
                  CHECK(tick->status == SeamStatus::Found && tick->rva == want.tick);
                  CHECK(tick->status != SeamStatus::EntryPatched);
                  const SeamResult* counter = Get(rb, "latency.frameCounterOffset");
                  REQUIRE(counter != nullptr);
                  CHECK(counter->status == SeamStatus::NotFound && counter->fellBackToTable);
                  GateInput in;
                  in.exeIsFfxiv = true;
                  in.exactKnownBuild = true;
                  in.rb = &rb;
                  in.registry = &registry;
                  const GateReport g = Decide(in, GateSettings {});
                  CHECK(g.Enabled(Feature::LatencyTick));
                  // on the exact known build the lost frame counter is filled from the table, so its bit stays set here
                  CHECK(g.latencyOffsetsResolved == kLatencyAllOffsetsResolved);
                  // on a build the table does not know, the same loss clears the bit: the install then skips the offset instead of reading it
                  const ResolvedBuild unknownRb = ResolveImage(img, registry);
                  CHECK(!unknownRb.knownBuild);
                  GateInput uin;
                  uin.exeIsFfxiv = true;
                  uin.exactKnownBuild = false;
                  uin.rb = &unknownRb;
                  uin.registry = &registry;
                  GateSettings unknownKey;
                  unknownKey.unknownBuildHooks = true;
                  const GateReport ug = Decide(uin, unknownKey);
                  CHECK(ug.Enabled(Feature::LatencyTick));
                  CHECK(ug.latencyOffsetsResolved == (kLatencyAllOffsetsResolved & ~kLatencyFrameCounterResolved));
              });

    // (c) both interior blocks broken: the seam is NotFound and the gate turns the feature off
    WithBytes(img, clock + 1, { 0x00 },
              [&]
              {
                  WithBytes(img, teardown + 1, { 0x00 },
                            [&]
                            {
                                const ResolvedBuild rb = ResolveImage(img, registry, options);
                                const SeamResult* tick = Get(rb, "latency.tick");
                                REQUIRE(tick != nullptr);
                                CHECK(tick->status == SeamStatus::NotFound);
                                GateInput in;
                                in.exeIsFfxiv = true;
                                in.exactKnownBuild = true;
                                in.rb = &rb;
                                in.registry = &registry;
                                const GateReport g = Decide(in, GateSettings {});
                                CHECK(!g.Enabled(Feature::LatencyTick));
                                if (g.Enabled(Feature::LatencyTick))
                                    std::printf("FAIL: the lease stayed on with both tick anchors gone\n");
                            });
              });
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::printf("FAIL: usage: resolve_latency_anchors <live exe> <benchmark exe>\n");
        return 1;
    }
    int checked = 0;
    for (int i = 1; i <= 2 && i < argc; ++i)
    {
        const std::string path = argv[i];
        const Expected& want = i == 1 ? kLive : kBench;
        std::ifstream probe(path, std::ios::binary);
        if (!probe)
        {
            std::printf("SKIP: %s is not there (%s)\n", want.name, path.c_str());
            continue;
        }
        std::string error;
        const auto image = DenseImage::FromFile(path, &error);
        if (!image)
        {
            std::printf("FAIL: cannot read %s: %s\n", path.c_str(), error.c_str());
            return 1;
        }
        CheckBuild(*image, want);
        if (i == 1 && image->TimeDateStamp() == kCurrentBuild.timeDateStamp && image->SizeOfImage() == kCurrentBuild.sizeOfImage)
            CheckDecoys(*image, want);
        else if (i == 1)
            std::printf("SKIP: the decoys need the table's build, %s is not it\n", path.c_str());
        ++checked;
    }
    std::printf("checked %d executable(s)\n", checked);
    return TEST_RESULT("resolve_latency_anchors");
}
