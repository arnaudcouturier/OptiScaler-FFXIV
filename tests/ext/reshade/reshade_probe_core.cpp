// S10 D0: probe core logic without ReShade: runtime report text, the F3 frame ledger (counts, probe/end-of-frame split,
// detail frames then summaries), run folder naming and pruning, the file report, and the session fed by a fake backend.
#include "ext/reshade/ReShadeProbe.h"
#include "ext/reshade/ReShadeProbeCore.h"
#include "ext/reshade/ReShadeReport.h"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace ext::reshade::probe;

namespace
{
struct CollectReport final : IReport
{
    std::vector<std::string> lines;
    void Line(const std::string& l) override { lines.push_back(l); }
    bool Has(const std::string& needle) const
    {
        for (const auto& l : lines)
            if (l.find(needle) != std::string::npos)
                return true;
        return false;
    }
    size_t Count(const std::string& needle) const
    {
        size_t n = 0;
        for (const auto& l : lines)
            n += l.find(needle) != std::string::npos ? 1 : 0;
        return n;
    }
};

struct FakeBackend final : IBackend
{
    int64_t now = 0;
    bool describeOk = true;
    RuntimeInfo info;
    int describeCalls = 0;
    int techniqueCalls = 0;
    int64_t Qpc() override { return now; }
    int64_t QpcFrequency() override { return 1'000'000; } // 1 tick = 1 us
    bool Describe(uint64_t id, bool withTechniques, RuntimeInfo& out) override
    {
        describeCalls++;
        techniqueCalls += withTechniques ? 1 : 0;
        if (!describeOk)
            return false;
        out = info;
        out.id = id;
        out.techniquesKnown = withTechniques;
        if (!withTechniques)
            out.techniques.clear();
        return true;
    }
    // invasive steps: unused by these tests
    uint64_t PickRuntime(void*, const std::vector<uint64_t>&, std::string&) override { return 0; }
    BoundaryInfo Inspect(void*, uint64_t) override { return {}; }
    DepthReport QueryDepth(uint64_t, const BoundaryInfo&) override { return {}; }
    void SnapshotBefore(void*, const BoundaryInfo&) override {}
    PixelDiff SnapshotAfterAndDiff(void*, const BoundaryInfo&) override { return {}; }
    CallOutcome RenderEffects(void*, uint64_t, const BoundaryInfo&) override { return {}; }
    bool GetEffectsState(uint64_t, bool&) override { return false; }
    bool SetEffectsState(uint64_t, bool) override { return false; }
};

RuntimeInfo SampleRuntime()
{
    RuntimeInfo r;
    r.api = Api::D3D11;
    r.native = 0xAAA0;
    r.device = 0xBBB0;
    r.hwnd = 0xCCC0;
    r.backBufferCount = 2;
    r.backBufferWidth = 3840;
    r.backBufferHeight = 2160;
    r.backBufferFormat = 28;
    r.effectsEnabled = true;
    r.presetPath = ".\\reshade-presets\\a.ini";
    r.techniques = { { "Tonemap", "Tonemap.fx", true }, { "Bloom", "Bloom.fx", false }, { "Sharpen", "Sharp.fx", true } };
    return r;
}

EffectsEvent Pass(uint64_t rt, bool begin, int64_t qpc, bool fromProbe = false)
{
    EffectsEvent e;
    e.begin = begin;
    e.runtime = rt;
    e.cmdList = 0x10;
    e.rtv = 0x20;
    e.rtvSrgb = 0x30;
    e.width = 3840;
    e.height = 2160;
    e.format = 28;
    e.qpc = qpc;
    e.fromProbe = fromProbe;
    return e;
}

void TestFormatRuntime()
{
    RuntimeInfo r = SampleRuntime();
    r.id = 0x1562B089470;
    r.techniquesKnown = true;
    const auto lines = FormatRuntime(r, "reshade_reloaded_effects");
    std::string all;
    for (const auto& l : lines)
        all += l + "\n";
    assert(all.find("runtime 0x1562b089470") != std::string::npos);
    assert(all.find("api=D3D11") != std::string::npos);
    assert(all.find("3840x2160") != std::string::npos);
    assert(all.find("effects enabled: true") != std::string::npos);
    assert(all.find("preset: \".\\reshade-presets\\a.ini\"") != std::string::npos);
    assert(all.find("3 total, 2 enabled") != std::string::npos);
    assert(all.find("enabled: Tonemap (Tonemap.fx)") != std::string::npos);
    assert(all.find("enabled: Sharpen (Sharp.fx)") != std::string::npos);
    assert(all.find("disabled (1): Bloom") != std::string::npos);

    r.techniquesKnown = false;
    const auto early = FormatRuntime(r, "init_effect_runtime #1");
    assert(early.back().find("not known yet") != std::string::npos);
}

void TestLedger()
{
    FrameLedger ledger(1'000'000); // 1 tick = 1 us
    const uint64_t rt = 0x77;

    // frame 1: one end-of-frame pass of 250 us, none from the probe
    ledger.OnEffects(Pass(rt, true, 1000));
    assert(ledger.Open(rt).begin == 1 && ledger.Open(rt).finish == 0);
    ledger.OnEffects(Pass(rt, false, 1250));
    auto lines = ledger.OnFrameEnd(rt);
    assert(lines.size() == 1);
    assert(lines[0].find("frame 1") != std::string::npos && lines[0].find("begin=1 finish=1") != std::string::npos);
    assert(lines[0].find("span=250us") != std::string::npos);
    assert(lines[0].find("in-probe begin=0") != std::string::npos);
    FrameRecord last = ledger.LastClosed(rt);
    assert(last.frame == 1 && last.begin == 1 && last.finish == 1 && last.spanUs == 250.0);
    assert(last.width == 3840 && last.rtv == 0x20);
    assert(ledger.Open(rt).begin == 0);

    // frame 2: a probe pass (400 us) and NO end-of-frame pass (the skip worked)
    ledger.OnEffects(Pass(rt, true, 5000, true));
    ledger.OnEffects(Pass(rt, false, 5400, true));
    lines = ledger.OnFrameEnd(rt);
    last = ledger.LastClosed(rt);
    assert(last.begin == 0 && last.finish == 0 && last.probeBegin == 1 && last.probeFinish == 1);
    assert(last.probeSpanUs == 400.0);
    assert(lines[0].find("begin=0 finish=0") != std::string::npos &&
           lines[0].find("in-probe begin=1 finish=1") != std::string::npos);

    // frame 3: probe pass AND an end-of-frame pass (the skip did not work)
    ledger.OnEffects(Pass(rt, true, 6000, true));
    ledger.OnEffects(Pass(rt, false, 6100, true));
    ledger.OnEffects(Pass(rt, true, 7000));
    ledger.OnEffects(Pass(rt, false, 7300));
    ledger.OnFrameEnd(rt);
    last = ledger.LastClosed(rt);
    assert(last.begin == 1 && last.probeBegin == 1);

    // a second runtime does not disturb the first
    ledger.OnEffects(Pass(0x88, true, 10));
    assert(ledger.Open(rt).begin == 0 && ledger.Open(0x88).begin == 1);
    ledger.Forget(0x88);
    assert(ledger.Open(0x88).begin == 0 && ledger.Frames(0x88) == 0);
    assert(ledger.Frames(rt) == 3);

    // detail frames up to 120, then silence until the 600-frame summary
    FrameLedger l2(1'000'000);
    size_t detail = 0, summary = 0;
    for (uint64_t f = 1; f <= 1300; f++)
    {
        l2.OnEffects(Pass(rt, true, int64_t(f) * 1000));
        l2.OnEffects(Pass(rt, false, int64_t(f) * 1000 + 100));
        if (f == 700) // one frame with two passes
        {
            l2.OnEffects(Pass(rt, true, 1));
            l2.OnEffects(Pass(rt, false, 2));
        }
        for (const auto& l : l2.OnFrameEnd(rt))
        {
            if (l.find("summary") != std::string::npos)
            {
                summary++;
                if (summary == 1)
                    assert(l.find("last 600 frames") != std::string::npos);
                if (summary == 2)
                    assert(l.find("2+ passes=1") != std::string::npos);
            }
            else
                detail++;
        }
    }
    assert(detail == FrameLedger::kDetailFrames);
    assert(summary == 2); // frames 600 and 1200
}

void TestFolders()
{
    assert(RunFolderName(2026, 10, 16, 18, 5, 9) == "reshade-20261016-180509");
    assert(IsRunFolderName("reshade-20261016-180509"));
    assert(!IsRunFolderName("20261016-180509")); // the other diagnostics' folders: never ours
    assert(!IsRunFolderName("reshade-20261016-18050"));
    assert(!IsRunFolderName("reshade-20261016_180509"));
    assert(!IsRunFolderName("reshade-2026101a-180509"));
    assert(!IsRunFolderName("reshade-20261016-180509-extra"));

    std::vector<std::string> names { "reshade-20261016-120000", "20261016-130000",      "reshade-20261015-120000",
                                     "reshade-20261017-120000", "notes",                "reshade-20261014-120000",
                                     "reshade-20261013-120000", "reshade-20261012-120000" };
    auto prune = RunsToPrune(names, 4);
    assert(prune.size() == 2);
    assert(prune[0] == "reshade-20261012-120000" && prune[1] == "reshade-20261013-120000");
    assert(RunsToPrune(names, 10).empty());
}

void TestFileReport()
{
    namespace fs = std::filesystem;
    const fs::path base = fs::temp_directory_path() / "optiscaler-reshade-report-test";
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base);

    // Pre-existing folders: 6 old valid runs (must be pruned to 4), one foreign folder and one file (untouched).
    for (int i = 1; i <= 6; i++)
        fs::create_directories(base / RunFolderName(2025, 1, i, 0, 0, 0));
    fs::create_directories(base / "20250101-000000");
    {
        std::ofstream(base / "reshade-20250101-000000.txt") << "x";
    }

    FileReport report(base);
    assert(report.Folder().empty()); // nothing created before the first line
    report.Line("hello");
    report.Line("[F1] world");
    assert(!report.Failed());
    const fs::path folder = report.Folder();
    assert(folder.parent_path() == base);
    assert(IsRunFolderName(folder.filename().string()));

    std::ifstream in(folder / "reshade-probe.txt");
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    assert(text.find("hello") != std::string::npos && text.find("[F1] world") != std::string::npos);
    assert(text.find("[+") != std::string::npos);

    size_t runs = 0;
    for (const auto& e : fs::directory_iterator(base))
        runs += IsRunFolderName(e.path().filename().string()) ? 1 : 0;
    assert(runs == FileReport::kKeepRuns); // 4 old + the new one
    assert(fs::exists(base / "20250101-000000"));
    assert(fs::exists(base / "reshade-20250101-000000.txt"));
    assert(!fs::exists(base / RunFolderName(2025, 1, 1, 0, 0, 0)));
    assert(fs::exists(base / RunFolderName(2025, 1, 6, 0, 0, 0)));

    // A base path that cannot be a directory (it lives below a file) fails quietly and never throws.
    const fs::path blocker = base / "blocker.txt";
    {
        std::ofstream(blocker) << "x";
    }
    FileReport bad(blocker / "sub");
    bad.Line("lost");
    assert(bad.Failed());
    bad.Line("still lost");

    fs::remove_all(base, ec);
}

void TestSession()
{
    FakeBackend be;
    be.info = SampleRuntime();
    CollectReport rep;
    Session s(be, rep);
    s.Begin("registration: OK");
    assert(rep.Has("registration: OK"));

    const uint64_t rt = 0x1562B089470;
    s.OnInitRuntime(rt);
    assert(s.AliveRuntimes() == 1);
    assert(rep.Has("init_effect_runtime #1") && rep.Has("not known yet"));

    // ReShade recreates the runtime with the same pointer after ResizeBuffers
    s.OnDestroyRuntime(rt);
    assert(s.AliveRuntimes() == 0 && rep.Has("destroy_effect_runtime"));
    s.OnInitRuntime(rt);
    assert(s.AliveRuntimes() == 1 && rep.Has("init_effect_runtime #2"));

    s.OnReloadedEffects(rt);
    assert(be.techniqueCalls == 1 && rep.Has("3 total, 2 enabled"));
    RuntimeInfo got;
    assert(s.Runtime(rt, got) && got.techniques.size() == 3 && got.techniquesKnown);

    s.OnPresetPath(rt, "new.ini");
    assert(rep.Has("preset \"new.ini\""));
    assert(s.Runtime(rt, got) && got.presetPath == "new.ini");

    // a normal frame
    be.now = 100;
    s.OnEffects(Pass(rt, true, 100));
    s.OnEffects(Pass(rt, false, 160));
    s.OnQueuePresent();
    s.OnReshadePresent(rt);
    assert(rep.Has("frame 1: end-of-frame passes begin=1 finish=1"));

    const EventCounts c = s.Counts();
    assert(c.n[size_t(EventKind::InitRuntime)] == 2);
    assert(c.n[size_t(EventKind::DestroyRuntime)] == 1);
    assert(c.n[size_t(EventKind::ReloadedEffects)] == 1);
    assert(c.n[size_t(EventKind::PresetPath)] == 1);
    assert(c.n[size_t(EventKind::BeginEffects)] == 1 && c.n[size_t(EventKind::FinishEffects)] == 1);
    assert(c.n[size_t(EventKind::ReshadePresent)] == 1 && c.n[size_t(EventKind::QueuePresent)] == 1);
    assert(c.Total() == 9);
    assert(FormatEventCounts(c).find("reshade_present=1") != std::string::npos);

    const SessionStatus st = s.Status("C:\\x", false);
    assert(st.framesSeen == 1 && st.aliveRuntimes == 1 && st.reportFolder == "C:\\x");

    // failures never throw and are visible in the report
    FakeBackend bad;
    bad.describeOk = false;
    CollectReport rep2;
    Session s2(bad, rep2);
    s2.OnInitRuntime(0x99);
    assert(rep2.Has("description failed"));
    s2.OnReloadedEffects(0x99);
    assert(rep2.Count("description failed") == 2);

    // the probe-call flag is per thread and scoped
    assert(!InProbeCall());
    {
        ProbeCallScope scope;
        assert(InProbeCall());
    }
    assert(!InProbeCall());
}
} // namespace

int main()
{
    TestFormatRuntime();
    TestLedger();
    TestFolders();
    TestFileReport();
    TestSession();
    std::puts("PASS: runtime report text, frame ledger, run folders and pruning, file report, session event flow");
    return 0;
}
