// S10 package B: the per-present decision sequence (ext/hudless/HudlessCore.h) driven by a fake IHudlessSource and a fake
// provider / GPU host: every fallback reason of docs/HUDLESS.md section 5, the sequencing of the three seams, first failure
// wins, hysteresis of the displayed state, coverage guard (trip, retry timing, "unknown"), scene HDR priority, and the
// log lines (start, mode change, state change, periodic summary). With the mode off nothing is touched.
#include "ext/hudless/HudlessCore.h"

#include <cassert>
#include <cstdio>
#include <deque>
#include <string>
#include <utility>
#include <vector>

using namespace ext::hudless;

namespace
{
struct FakeSource final : IHudlessSource
{
    bool armed = true;
    TakeResult result = TakeResult::Ok;
    const char* reason = "";
    int takes = 0;
    int activeCalls = 0;
    bool Active() override
    {
        ++activeCalls;
        return armed;
    }
    TakeResult Take(FrameImage& out) override
    {
        ++takes;
        out = {};
        out.width = 3840;
        out.height = 2160;
        out.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        return result;
    }
    const char* LastReason() override { return reason; }
    void EndFrame() override {}
};

struct FakeHost final : IEngineHost
{
    FakeSource source;
    IHudlessSource* sourcePtr = &source;
    bool fgActive = true;
    Provider provider = Provider::DlssG;
    bool hdr = false;
    bool compare = false;
    float limit = 0.60f;
    Reason check = Reason::None;
    Reason stage = Reason::None;
    bool pasteReady = true;
    bool sceneOwns = false;
    std::vector<std::string> log;
    int calls = 0;        // every host query / action
    int pasteQueries = 0; // PasteReady() calls

    bool FrameGenActive() override { ++calls; return fgActive; }
    Provider FrameGenProvider() override { ++calls; return provider; }
    bool HdrOutput() override { ++calls; return hdr; }
    bool DebugCompareOn() override { ++calls; return compare; }
    float CoverageLimit() override { ++calls; return limit; }
    IHudlessSource* Source() override { ++calls; return sourcePtr; }
    Reason CheckCapture(const FrameImage&) override { ++calls; return check; }
    Reason StageCapture(const FrameImage&) override { ++calls; return stage; }
    bool PasteReady() override { ++calls; ++pasteQueries; return pasteReady; }
    bool SceneHdrOwnsFrame() override { ++calls; return sceneOwns; }
    void Log(const std::string& line) override { log.push_back(line); }
};

struct Run
{
    bool staged;
    bool swapped;
    Reason reason;
};

// One full present: Pre, UiFreeFrame, Post (bookkeeping only)
Run Present(Core& core, Mode mode, const PasteCounters& paste = {})
{
    Run r {};
    r.staged = core.BeginPresent(mode, paste);
    r.swapped = core.DecideBase();
    r.reason = core.PresentReason();
    core.EndPresent();
    return r;
}

void TestOff()
{
    FakeHost host;
    Core core(host);
    for (int i = 0; i < 1000; ++i)
    {
        const Run r = Present(core, Mode::Off);
        assert(!r.staged && !r.swapped && r.reason == Reason::ModeOff);
    }
    // Byte for byte today's: not one query, one start line, nothing counted
    assert(host.calls == 0 && host.source.takes == 0 && host.source.activeCalls == 0);
    assert(host.log.size() == 1 && host.log[0] == "HudLess: mode off");
    const Status s = core.Snapshot();
    assert(s.presents == 0 && s.captured == 0 && s.phase == Phase::Off && s.mode == Mode::Off);
}

void TestReasons()
{
    auto reasonOf = [](auto&& setup, Mode mode = Mode::UiFree)
    {
        FakeHost host;
        setup(host);
        Core core(host);
        return Present(core, mode);
    };

    // the happy paths
    {
        const Run r = reasonOf([](FakeHost&) {});
        assert(r.staged && r.swapped && r.reason == Reason::None);
    }
    {
        const Run r = reasonOf([](FakeHost&) {}, Mode::Recomposition);
        assert(r.staged && !r.swapped && r.reason == Reason::None); // recomposition never swaps the base
    }

    assert(reasonOf([](FakeHost& h) { h.fgActive = false; }).reason == Reason::NoFrameGeneration);
    assert(reasonOf([](FakeHost& h) { h.provider = Provider::FsrFg; }).reason == Reason::ProviderUnsupported);
    assert(reasonOf([](FakeHost& h) { h.provider = Provider::XeFg; }).reason == Reason::ProviderUnsupported);
    assert(reasonOf([](FakeHost& h) { h.provider = Provider::None; }).reason == Reason::ProviderUnsupported);
    // recomposition: the tags exist for FSR-FG and XeFG too
    assert(reasonOf([](FakeHost& h) { h.provider = Provider::FsrFg; }, Mode::Recomposition).reason == Reason::None);
    assert(reasonOf([](FakeHost& h) { h.provider = Provider::XeFg; }, Mode::Recomposition).reason == Reason::None);
    assert(reasonOf([](FakeHost& h) { h.hdr = true; }, Mode::Recomposition).reason == Reason::FormatUnsupported);
    assert(reasonOf([](FakeHost& h) { h.hdr = true; }).reason == Reason::None); // ui_free is carried over HDR10 output
    assert(reasonOf([](FakeHost& h) { h.sourcePtr = nullptr; }).reason == Reason::NoSource);
    assert(reasonOf([](FakeHost& h) { h.source.armed = false; }).reason == Reason::NoSource);
    assert(reasonOf([](FakeHost& h) { h.source.result = TakeResult::NoSource; }).reason == Reason::NoSource);
    assert(reasonOf([](FakeHost& h) { h.source.result = TakeResult::NotThisFrame; }).reason ==
           Reason::NoBoundaryThisFrame);
    assert(reasonOf([](FakeHost& h) { h.source.result = TakeResult::Rejected; }).reason == Reason::CaptureRejected);
    assert(reasonOf([](FakeHost& h) { h.check = Reason::SizeMismatch; }).reason == Reason::SizeMismatch);
    assert(reasonOf([](FakeHost& h) { h.check = Reason::FormatUnsupported; }).reason == Reason::FormatUnsupported);
    assert(reasonOf([](FakeHost& h) { h.check = Reason::MsaaOrArray; }).reason == Reason::MsaaOrArray);
    assert(reasonOf([](FakeHost& h) { h.stage = Reason::SlotBusy; }).reason == Reason::SlotBusy);
    assert(reasonOf([](FakeHost& h) { h.stage = Reason::CaptureRejected; }).reason == Reason::CaptureRejected);
    assert(reasonOf([](FakeHost& h) { h.compare = true; }).reason == Reason::DebugTintOn);
    assert(reasonOf([](FakeHost& h) { h.compare = true; }, Mode::Recomposition).reason == Reason::None);

    // after the staging: paste not ready (the capture is still staged: the paste producer still runs), scene HDR owns
    {
        const Run r = reasonOf([](FakeHost& h) { h.pasteReady = false; });
        assert(r.staged && !r.swapped && r.reason == Reason::PasteNotReady);
    }
    {
        const Run r = reasonOf([](FakeHost& h) { h.sceneOwns = true; });
        assert(r.staged && !r.swapped && r.reason == Reason::SceneHdrOwnsFrame);
    }
    // the scene HDR probe has side effects: it is only asked after the cheaper paste check passed
    {
        FakeHost host;
        host.pasteReady = false;
        host.sceneOwns = true;
        Core core(host);
        assert(Present(core, Mode::UiFree).reason == Reason::PasteNotReady);
    }
    // recomposition never asks for the paste or the scene
    {
        FakeHost host;
        host.pasteReady = false;
        host.sceneOwns = true;
        Core core(host);
        assert(Present(core, Mode::Recomposition).reason == Reason::None);
    }

    // the first failure of a present wins, a late failure is recorded when nothing failed before
    {
        FakeHost host;
        host.source.result = TakeResult::NotThisFrame;
        Core core(host);
        core.BeginPresent(Mode::UiFree, {});
        core.Fail(Reason::SlotBusy);
        assert(core.PresentReason() == Reason::NoBoundaryThisFrame);
        core.EndPresent();
    }
    {
        FakeHost host;
        Core core(host);
        assert(core.BeginPresent(Mode::Recomposition, {}) && core.PresentReason() == Reason::None);
        core.Fail(Reason::CaptureRejected); // the provider refused the tag
        assert(core.PresentReason() == Reason::CaptureRejected);
        core.EndPresent();
        const Status s = core.Snapshot();
        assert(s.captured == 0 && s.presents == 1 && s.reasons[static_cast<size_t>(Reason::CaptureRejected)] == 1);
    }
    // MaskCoverage is exercised in TestCoverage
}

// The invariant of the traps list: exactly one of Produce / Invalidate per ui_free present, Produce on every swapped base
// and on PasteNotReady frames (bootstrap), Invalidate on every other fallback, none outside ui_free.
void TestPasteDecision()
{
    auto decide = [](auto&& setup, Mode mode = Mode::UiFree)
    {
        FakeHost host;
        setup(host);
        Core core(host);
        const Run r = Present(core, mode);
        return std::pair { core.PasteDecision(), r };
    };
    auto noop = [](FakeHost&) {};

    {
        const auto [action, r] = decide(noop);
        assert(r.swapped && action == PasteAction::Produce); // a swapped base always gets its paste
    }
    {
        const auto [action, r] = decide([](FakeHost& h) { h.pasteReady = false; });
        assert(!r.swapped && r.reason == Reason::PasteNotReady && action == PasteAction::Produce); // bootstrap
    }
    {
        const auto [action, r] = decide([](FakeHost& h) { h.sceneOwns = true; });
        assert(!r.swapped && action == PasteAction::Invalidate);
    }
    for (auto setup : { +[](FakeHost& h) { h.fgActive = false; }, +[](FakeHost& h) { h.provider = Provider::FsrFg; },
                        +[](FakeHost& h) { h.sourcePtr = nullptr; }, +[](FakeHost& h) { h.compare = true; },
                        +[](FakeHost& h) { h.source.result = TakeResult::NotThisFrame; },
                        +[](FakeHost& h) { h.source.result = TakeResult::Rejected; },
                        +[](FakeHost& h) { h.check = Reason::SizeMismatch; },
                        +[](FakeHost& h) { h.stage = Reason::SlotBusy; } })
    {
        const auto [action, r] = decide(setup);
        assert(!r.swapped && !r.staged && action == PasteAction::Invalidate);
    }
    // never swapped and invalidated in the same present, over any combination of the toggles
    for (unsigned bits = 0; bits < 64; ++bits)
    {
        FakeHost host;
        host.fgActive = !(bits & 1);
        host.compare = bits & 2;
        host.pasteReady = !(bits & 4);
        host.sceneOwns = bits & 8;
        host.stage = (bits & 16) ? Reason::SlotBusy : Reason::None;
        host.source.result = (bits & 32) ? TakeResult::NotThisFrame : TakeResult::Ok;
        Core core(host);
        const Run r = Present(core, Mode::UiFree);
        assert(!(r.swapped && core.PasteDecision() == PasteAction::Invalidate));
        assert(core.PasteDecision() != PasteAction::None);
    }
    // outside ui_free the producer is never involved
    assert(decide(noop, Mode::Recomposition).first == PasteAction::None);
    assert(decide(noop, Mode::Off).first == PasteAction::None);
}

void TestBoundaryText()
{
    FakeHost host;
    Core core(host);
    host.source.reason = "marker shader created before the hook";
    host.source.result = TakeResult::Rejected;
    Present(core, Mode::UiFree);
    assert(core.Snapshot().boundary == "rejected: marker shader created before the hook");
    host.source.result = TakeResult::NotThisFrame;
    Present(core, Mode::UiFree);
    assert(core.Snapshot().boundary == "not detected");
    host.source.result = TakeResult::Ok;
    Present(core, Mode::UiFree);
    assert(core.Snapshot().boundary == "ok");
    host.sourcePtr = nullptr;
    Present(core, Mode::UiFree);
    assert(core.Snapshot().boundary == "no source");
}

void TestStateMachineAndLogs()
{
    FakeHost host;
    Core core(host);

    // Searching -> Active (one line each), start line first
    for (int i = 0; i < 9; ++i)
        Present(core, Mode::UiFree);
    assert(core.Snapshot().phase == Phase::Searching);
    Present(core, Mode::UiFree);
    assert(core.Snapshot().phase == Phase::Active);
    assert(host.log.size() == 3);
    assert(host.log[0] == "HudLess: mode ui_free");
    assert(host.log[1] == "HudLess: state Off -> Searching");
    assert(host.log[2] == "HudLess: state Searching -> Active");

    // 6 failures of 60 stay Active (hysteresis), no line
    for (int i = 0; i < 50; ++i)
        Present(core, Mode::UiFree);
    host.stage = Reason::SlotBusy;
    for (int i = 0; i < 6; ++i)
        Present(core, Mode::UiFree);
    host.stage = Reason::None;
    assert(core.Snapshot().phase == Phase::Active && host.log.size() == 3);

    // Sustained failures: Fallback(<reason>) exactly once, then back to Active
    host.stage = Reason::SlotBusy;
    for (int i = 0; i < 40; ++i)
        Present(core, Mode::UiFree);
    assert(core.Snapshot().phase == Phase::Fallback && core.Snapshot().reason == Reason::SlotBusy);
    assert(host.log.size() == 4 && host.log[3] == "HudLess: state Active -> Fallback(SlotBusy)");
    host.stage = Reason::None;
    for (int i = 0; i < 70; ++i)
        Present(core, Mode::UiFree);
    assert(core.Snapshot().phase == Phase::Active);
    assert(host.log.size() == 5 && host.log[4] == "HudLess: state Fallback(SlotBusy) -> Active");

    // A mode change is logged and restarts the state machine
    const size_t before = host.log.size();
    Present(core, Mode::Recomposition);
    assert(host.log[before] == "HudLess: mode ui_free -> recomposition");
    assert(host.log[before + 1] == "HudLess: state Off -> Searching");
    Present(core, Mode::Off);
    assert(host.log.back() == "HudLess: mode recomposition -> off");
    assert(core.Snapshot().phase == Phase::Off);
}

void TestNoFrameGenerationGoesOff()
{
    FakeHost host;
    Core core(host);
    for (int i = 0; i < 20; ++i)
        Present(core, Mode::UiFree);
    assert(core.Snapshot().phase == Phase::Active);
    host.fgActive = false;
    for (int i = 0; i < 9; ++i)
        Present(core, Mode::UiFree);
    assert(core.Snapshot().phase == Phase::Active); // a short pause does not flap the display
    Present(core, Mode::UiFree);
    assert(core.Snapshot().phase == Phase::Off);
    assert(host.log.back() == "HudLess: state Active -> Off");
}

void TestSummary()
{
    FakeHost host;
    Core core(host);
    PasteCounters paste;
    for (int i = 1; i <= 600; ++i)
    {
        host.stage = (i == 100) ? Reason::SlotBusy : Reason::None;
        host.source.result = (i == 300) ? TakeResult::NotThisFrame : TakeResult::Ok;
        if (i != 100 && i != 300)
        {
            ++paste.produced;
            paste.coverageSequence = paste.produced; // a readback of the newest image completed
        }
        paste.pasted += 2;
        paste.coverage = 0.04f;
        if (i == 450)
            paste.coverage = 0.097f;
        Present(core, Mode::UiFree, paste);
    }
    paste.skipped = 6;
    // the 600th present already wrote the line; the paste counters it saw are the ones up to that present
    std::string summary;
    for (const auto& line : host.log)
        if (line.find("600 presents") != std::string::npos)
            summary = line;
    assert(!summary.empty());
    assert(summary.rfind("HudLess: mode ui_free, state Active; 600 presents: 598 captured, 2 fallbacks (SlotBusy 1, "
                         "NoBoundaryThisFrame 1); mask coverage avg ",
                         0) == 0);
    assert(summary.find("max 9.7%; paste built 598 pasted 1200 skipped 0; slot waits 0; boundary: ok") !=
           std::string::npos);

    // next window starts from zero: the second line counts only its own presents
    host.log.clear();
    for (int i = 1; i <= 600; ++i)
        Present(core, Mode::UiFree, paste);
    assert(host.log.size() == 1 && host.log[0].find("; 600 presents: 600 captured, 0 fallbacks;") != std::string::npos);
}

void TestCoverage()
{
    FakeHost host;
    Core core(host);
    PasteCounters paste;
    uint64_t trippedAt = 0;
    std::vector<Run> runs;
    for (uint64_t i = 1; i <= 140; ++i)
    {
        paste.produced = i;
        paste.coverageSequence = i; // one new readback per present
        paste.coverage = 0.9f;
        runs.push_back(Present(core, Mode::UiFree, paste));
        if (trippedAt == 0 && runs.back().reason == Reason::MaskCoverage)
            trippedAt = i;
    }
    // present 1 takes the baseline, then 5 fresh samples over the limit: tripped from present 6. The coverage stays over
    // the release level, so the guard stays tripped. A tripped present keeps asking the source and taking the capture
    // (it probes), it only does not swap the base.
    assert(trippedAt == 6);
    for (uint64_t i = 1; i <= 5; ++i)
        assert(runs[i - 1].reason == Reason::None && runs[i - 1].swapped);
    for (uint64_t i = 6; i <= 140; ++i)
        assert(runs[i - 1].reason == Reason::MaskCoverage && runs[i - 1].staged && !runs[i - 1].swapped);
    const Status s = core.Snapshot();
    assert(s.coverageTripped);
    assert(s.reasons[static_cast<size_t>(Reason::MaskCoverage)] == 135);
    assert(host.source.takes == 140);

    // Tripped and the readbacks stop (the sequence is frozen): after kStarveFrames probed presents the guard lets go, the
    // present after that is swapped again
    FakeHost hostStarve;
    Core coreStarve(hostStarve);
    PasteCounters frozen;
    std::vector<Run> starveRuns;
    for (uint64_t i = 1; i <= 6; ++i)
    {
        frozen.produced = i;
        frozen.coverageSequence = i;
        frozen.coverage = 0.9f;
        starveRuns.push_back(Present(coreStarve, Mode::UiFree, frozen));
    }
    assert(starveRuns.back().reason == Reason::MaskCoverage); // tripped at present 6, probing
    for (uint64_t i = 7; i <= 6 + CoverageGuard::kStarveFrames; ++i)
        starveRuns.push_back(Present(coreStarve, Mode::UiFree, frozen)); // the same readback again and again
    for (uint64_t i = 6; i < 6 + CoverageGuard::kStarveFrames; ++i)
        assert(starveRuns[i - 1].reason == Reason::MaskCoverage); // presents 6..125
    assert(starveRuns[6 + CoverageGuard::kStarveFrames - 1].reason == Reason::None); // present 126 is swapped
    assert(starveRuns[6 + CoverageGuard::kStarveFrames - 1].swapped);
    assert(!coreStarve.Snapshot().coverageTripped);

    // A tripped guard does not count presents without a capture towards starvation
    FakeHost hostGap;
    Core coreGap(hostGap);
    for (uint64_t i = 1; i <= 6; ++i)
    {
        frozen.produced = i;
        frozen.coverageSequence = i;
        Present(coreGap, Mode::UiFree, frozen);
    }
    assert(coreGap.Snapshot().coverageTripped);
    hostGap.source.result = TakeResult::NotThisFrame;
    for (int i = 0; i < 3 * CoverageGuard::kStarveFrames; ++i)
        assert(Present(coreGap, Mode::UiFree, frozen).reason == Reason::NoBoundaryThisFrame);
    assert(coreGap.Snapshot().coverageTripped);
    // The only probed present so far is present 6, so one starvation count is in. The boundary comes back: the first
    // present after the gap probes (the previous one took no capture, it adds nothing), 118 more probing presents bring the
    // count to 119, the next present reaches kStarveFrames and the guard lets go.
    hostGap.source.result = TakeResult::Ok;
    for (int i = 1; i < CoverageGuard::kStarveFrames; ++i)
        assert(Present(coreGap, Mode::UiFree, frozen).reason == Reason::MaskCoverage);
    assert(Present(coreGap, Mode::UiFree, frozen).reason == Reason::None);

    // The same stale readback (coverage high, sequence frozen) never blocks, however many presents read it
    FakeHost hostStale;
    Core coreStale(hostStale);
    PasteCounters stale;
    stale.produced = 50;
    stale.coverageSequence = 50;
    stale.coverage = 0.99f;
    for (int i = 1; i <= 60; ++i)
    {
        const Run r = Present(coreStale, Mode::UiFree, stale);
        assert(r.staged && r.swapped && r.reason == Reason::None);
    }
    assert(coreStale.Snapshot().coverageUnknown);

    // Coverage "unknown": no readback ever completes. Never blocks, reads as unknown after 8 presents
    FakeHost host2;
    Core core2(host2);
    PasteCounters none; // coverage -1
    for (int i = 1; i <= 30; ++i)
    {
        const Run r = Present(core2, Mode::UiFree, none);
        assert(r.staged && r.swapped && r.reason == Reason::None);
        assert(core2.Snapshot().coverageUnknown == (i >= CoverageGuard::kUnknownFrames));
    }

    // A fresh readback ends "unknown"
    PasteCounters one;
    one.produced = 3;
    one.coverageSequence = 3;
    one.coverage = 0.1f;
    Present(core2, Mode::UiFree, one);
    assert(!core2.Snapshot().coverageUnknown);

    // Guard off (CoverageGuard = 0): any coverage is fine
    FakeHost host3;
    host3.limit = 0.f;
    Core core3(host3);
    for (uint64_t i = 1; i <= 60; ++i)
    {
        PasteCounters p;
        p.produced = i;
        p.coverageSequence = i;
        p.coverage = 1.0f;
        assert(Present(core3, Mode::UiFree, p).reason == Reason::None);
    }
}

// Closes the loop the real engine has and the tests above feed by hand: the paste producer runs only on presents whose
// PasteDecision() is Produce, its readback (coverage + sequence) reaches the next presents with a latency, a completed
// image makes the paste ready, an Invalidate withdraws every image, and the live source goes idle for the next present
// when this one did not ask it (LiveSource::EndFrame arms the next frame only if Take() ran).
struct Replay
{
    static constexpr uint64_t kLatency = 2; // presents between Produce and its completed image / readback

    FakeHost host;
    Core core { host };
    float coverage = 0.f;     // mask coverage of the images produced from now on
    bool noBoundary = false;  // the game shows no UI boundary in this present (fade to a loading screen, menu, ...)

    struct Image
    {
        uint64_t ordinal;
        float coverage;
        uint64_t dueAt;
        bool valid; // not withdrawn by an Invalidate before it completed
    };
    std::deque<Image> inFlight;
    PasteCounters counters;
    uint64_t present = 0;
    uint64_t produced = 0;
    bool completed = false; // a completed image exists and nothing invalidated the ring since
    bool tookLast = true;   // the source was asked in the previous present

    std::vector<Run> runs;
    std::vector<PasteAction> actions;
    uint64_t flips = 0; // changes of the swapped state
    bool lastSwapped = false;

    Run Step()
    {
        ++present;
        while (!inFlight.empty() && inFlight.front().dueAt <= present)
        {
            const Image image = inFlight.front();
            inFlight.pop_front();
            counters.produced = image.ordinal;
            counters.coverage = image.coverage;
            counters.coverageSequence = image.ordinal;
            if (image.valid)
                completed = true;
        }

        host.source.result = (noBoundary || !tookLast) ? TakeResult::NotThisFrame : TakeResult::Ok;
        host.pasteReady = completed;
        const int takesBefore = host.source.takes;
        const Run r = Present(core, Mode::UiFree, counters);
        tookLast = host.source.takes > takesBefore;

        const PasteAction action = core.PasteDecision();
        if (action == PasteAction::Produce)
        {
            inFlight.push_back({ ++produced, coverage, present + kLatency, true });
        }
        else if (action == PasteAction::Invalidate)
        {
            completed = false;
            for (auto& image : inFlight)
                image.valid = false;
        }

        flips += r.swapped != lastSwapped;
        lastSwapped = r.swapped;
        runs.push_back(r);
        actions.push_back(action);
        return r;
    }

    void Steps(int n)
    {
        for (int i = 0; i < n; ++i)
            Step();
    }
};

// A tripped guard keeps probing: the source is still asked, a capture is still taken, the paste image is still built (the
// guard needs its readbacks), and the base is not swapped and no tags are wanted.
void TestProbeKeepsStaging()
{
    Replay replay;
    replay.coverage = 0.95f;
    replay.Steps(6);
    assert(!replay.core.Probing() && replay.core.WantsTags()); // presents 1..6: bootstrap, then swapped
    for (int i = 0; i < 60; ++i)
    {
        const int takes = replay.host.source.takes;
        const Run r = replay.Step();
        assert(replay.core.Probing());
        assert(r.staged && !r.swapped && r.reason == Reason::MaskCoverage);
        assert(replay.core.Staged() && !replay.core.Swapped());
        assert(replay.host.source.takes == takes + 1);
        assert(replay.core.PasteDecision() == PasteAction::Produce);
        assert(!replay.core.WantsTags());
        assert(replay.core.Snapshot().coverageTripped);
    }
    assert(replay.produced == 66);         // a paste image on every present, probing ones included
    assert(replay.host.pasteQueries == 6); // a probing present never asks whether the paste is ready

    // The probe is a ui_free matter: the other modes never probe
    FakeHost host;
    Core core(host);
    PasteCounters p;
    for (uint64_t i = 1; i <= 20; ++i)
    {
        p.produced = i;
        p.coverageSequence = i;
        p.coverage = 0.99f;
        Present(core, Mode::Recomposition, p);
        assert(!core.Probing() && core.PresentReason() == Reason::None && core.WantsTags());
    }
}

// What wins on a probing present. Capture failures come before the probe, so the first failure of the present is the
// one that counts; everything after the capture (paste ready, scene HDR) is not asked while probing.
void TestProbePrecedence()
{
    struct Outcome
    {
        Run run;
        bool probing;
        PasteAction action;
        bool tags;
        int pasteQueries;
    };
    auto trippedPresent = [](auto&& setup)
    {
        FakeHost host;
        Core core(host);
        PasteCounters p;
        for (uint64_t i = 1; i <= 6; ++i)
        {
            p.produced = i;
            p.coverageSequence = i;
            p.coverage = 0.9f;
            Present(core, Mode::UiFree, p);
        }
        assert(core.Snapshot().coverageTripped);
        const int queriesBefore = host.pasteQueries;
        setup(host);
        p.produced = 7;
        p.coverageSequence = 7;
        const Run r = Present(core, Mode::UiFree, p);
        return Outcome { r, core.Probing(), core.PasteDecision(), core.WantsTags(), host.pasteQueries - queriesBefore };
    };

    {
        const Outcome o = trippedPresent([](FakeHost&) {});
        assert(o.run.staged && !o.run.swapped && o.run.reason == Reason::MaskCoverage && o.probing &&
               o.action == PasteAction::Produce && !o.tags && o.pasteQueries == 0);
    }
    {
        // a paste that is not ready, or a scene HDR owner, would change the reason or the base if they were asked
        const Outcome o = trippedPresent(
            [](FakeHost& h)
            {
                h.pasteReady = false;
                h.sceneOwns = true;
            });
        assert(o.run.reason == Reason::MaskCoverage && o.probing && o.action == PasteAction::Produce);
    }

    // No boundary, rejected, refused, slot busy: nothing was captured, so it is not a probe and that failure is the
    // reason of the present
    const auto noCapture = [](const Outcome& o, Reason reason)
    {
        assert(!o.run.staged && !o.run.swapped && !o.probing && o.run.reason == reason);
        assert(o.action == PasteAction::Invalidate && !o.tags);
    };
    noCapture(trippedPresent([](FakeHost& h) { h.source.result = TakeResult::NotThisFrame; }), Reason::NoBoundaryThisFrame);
    noCapture(trippedPresent([](FakeHost& h) { h.source.result = TakeResult::Rejected; }), Reason::CaptureRejected);
    noCapture(trippedPresent([](FakeHost& h) { h.check = Reason::SizeMismatch; }), Reason::SizeMismatch);
    noCapture(trippedPresent([](FakeHost& h) { h.stage = Reason::SlotBusy; }), Reason::SlotBusy);
    noCapture(trippedPresent([](FakeHost& h) { h.compare = true; }), Reason::DebugTintOn);
    noCapture(trippedPresent([](FakeHost& h) { h.fgActive = false; }), Reason::NoFrameGeneration);
    noCapture(trippedPresent([](FakeHost& h) { h.provider = Provider::FsrFg; }), Reason::ProviderUnsupported);
}

// Constant coverage over the limit. Before the silent probe this tripped, blocked for exactly 120 presents, retried with
// a burst that swapped the base for a few frames, and tripped again: two changes of the base per ~127 presents (the
// history of this file has the characterization test of that behaviour). Now the guard trips once, nothing is swapped
// afterwards and the source is never left idle.
void TestReplayNoBurst()
{
    Replay replay;
    replay.coverage = 0.95f;
    replay.Steps(1000);
    const auto& runs = replay.runs;

    // presents 1 and 2: no completed image yet, 3..6 swapped, five fresh samples over the limit arrive at 3..7
    assert(runs[0].reason == Reason::PasteNotReady && runs[1].reason == Reason::PasteNotReady);
    for (size_t i = 2; i < 6; ++i)
        assert(runs[i].swapped && runs[i].reason == Reason::None);
    for (size_t i = 6; i < runs.size(); ++i)
        assert(runs[i].staged && !runs[i].swapped && runs[i].reason == Reason::MaskCoverage);

    assert(replay.flips == 2); // the bootstrap swap and the trip: nothing else
    const Status s = replay.core.Snapshot();
    assert(s.captured == 4);
    assert(s.reasons[static_cast<size_t>(Reason::NoBoundaryThisFrame)] == 0);
    assert(s.reasons[static_cast<size_t>(Reason::PasteNotReady)] == 2);
    assert(s.reasons[static_cast<size_t>(Reason::MaskCoverage)] == 994);
    assert(replay.host.source.takes == 1000); // the source never goes idle
    assert(replay.produced == 1000);
}

// The release is seamless: the paste images were built all along, so the first swapped present after the 30th clean
// sample has no reason, and it stays swapped.
void TestReleaseIsSeamless()
{
    Replay replay;
    replay.coverage = 0.95f;
    replay.Steps(60); // tripped at present 7
    replay.coverage = 0.10f;
    const size_t first = replay.runs.size(); // the first present whose image has the low coverage
    replay.Steps(100);
    const auto& runs = replay.runs;

    // that image's readback arrives kLatency presents later; the 30th clean sample releases the guard
    const size_t release = first + Replay::kLatency + CoverageGuard::kReleaseSamples - 1;
    for (size_t i = 6; i < release; ++i)
        assert(runs[i].reason == Reason::MaskCoverage && !runs[i].swapped);
    for (size_t i = release; i < runs.size(); ++i)
        assert(runs[i].staged && runs[i].swapped && runs[i].reason == Reason::None);

    assert(replay.flips == 3); // bootstrap swap, trip, release
    const Status s = replay.core.Snapshot();
    assert(!s.coverageTripped);
    assert(s.reasons[static_cast<size_t>(Reason::PasteNotReady)] == 2); // only the bootstrap at the very start
    assert(s.reasons[static_cast<size_t>(Reason::NoBoundaryThisFrame)] == 0);
}

// Hysteresis: the trip needs five fresh samples in a row over the limit, the release thirty in a row under 0.75 x the
// limit. A cutscene that sits just under the limit never trips; after a trip it does not flip back and forth either.
void TestHysteresisBand()
{
    // below the limit, with single peaks above it (never five in a row): no trip, no change of the base
    {
        const float pattern[] = { 0.50f, 0.55f, 0.66f };
        Replay replay;
        for (int i = 0; i < 1000; ++i)
        {
            replay.coverage = pattern[i % 3];
            replay.Step();
        }
        assert(replay.flips == 1); // the bootstrap swap
        assert(replay.core.Snapshot().reasons[static_cast<size_t>(Reason::MaskCoverage)] == 0);
    }
    // four in a row over the limit, then one under it, forever: still no trip
    {
        Replay replay;
        for (int i = 0; i < 1000; ++i)
        {
            replay.coverage = (i % 5 == 4) ? 0.50f : 0.66f;
            replay.Step();
        }
        assert(replay.flips == 1);
        assert(replay.core.Snapshot().reasons[static_cast<size_t>(Reason::MaskCoverage)] == 0);
    }
    // just under the limit for a long time: no trip
    {
        Replay replay;
        replay.coverage = 0.59f;
        replay.Steps(1000);
        assert(replay.flips == 1);
    }

    // after a trip, coverage that is under the limit but over the release level keeps the guard tripped
    for (const float held : { 0.50f, 0.55f, 0.46f, 0.59f })
    {
        Replay replay;
        replay.coverage = 0.95f;
        replay.Steps(40);
        replay.coverage = held;
        replay.Steps(1000);
        for (size_t i = 6; i < replay.runs.size(); ++i)
            assert(replay.runs[i].reason == Reason::MaskCoverage && !replay.runs[i].swapped);
        assert(replay.flips == 2);
    }
    // 29 clean samples and one in the band, over and over: never released
    {
        Replay replay;
        replay.coverage = 0.95f;
        replay.Steps(40);
        for (int cycle = 0; cycle < 30; ++cycle)
            for (int i = 0; i < 30; ++i)
            {
                replay.coverage = (i == 29) ? 0.50f : 0.10f;
                replay.Step();
            }
        for (size_t i = 6; i < replay.runs.size(); ++i)
            assert(!replay.runs[i].swapped);
        assert(replay.flips == 2);
    }
}
} // namespace

int main()
{
    TestOff();
    TestReasons();
    TestPasteDecision();
    TestBoundaryText();
    TestStateMachineAndLogs();
    TestNoFrameGenerationGoesOff();
    TestSummary();
    TestCoverage();
    TestProbeKeepsStaging();
    TestProbePrecedence();
    TestReplayNoBurst();
    TestReleaseIsSeamless();
    TestHysteresisBand();
    std::puts("PASS: engine core: every fallback reason, sequencing, hysteresis, coverage guard, scene HDR priority, log lines");
    return 0;
}
