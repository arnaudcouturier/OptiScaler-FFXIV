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
    int calls = 0; // every host query / action

    bool FrameGenActive() override { ++calls; return fgActive; }
    Provider FrameGenProvider() override { ++calls; return provider; }
    bool HdrOutput() override { ++calls; return hdr; }
    bool DebugCompareOn() override { ++calls; return compare; }
    float CoverageLimit() override { ++calls; return limit; }
    IHudlessSource* Source() override { ++calls; return sourcePtr; }
    Reason CheckCapture(const FrameImage&) override { ++calls; return check; }
    Reason StageCapture(const FrameImage&) override { ++calls; return stage; }
    bool PasteReady() override { ++calls; return pasteReady; }
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
    std::vector<Reason> reasons;
    for (uint64_t i = 1; i <= 140; ++i)
    {
        paste.produced = i;
        paste.coverageSequence = i; // one new readback per present
        paste.coverage = 0.9f;
        reasons.push_back(Present(core, Mode::UiFree, paste).reason);
        if (trippedAt == 0 && reasons.back() == Reason::MaskCoverage)
            trippedAt = i;
    }
    // present 1 takes the baseline, then 5 fresh samples over the limit: blocking from present 6 for 120 presents
    // (6..125), the retry is present 126, the next streak of 5 samples trips again at present 131
    assert(trippedAt == 6);
    for (uint64_t i = 6; i < 126; ++i)
        assert(reasons[i - 1] == Reason::MaskCoverage);
    for (uint64_t i = 1; i <= 5; ++i)
        assert(reasons[i - 1] == Reason::None);
    for (uint64_t i = 126; i <= 130; ++i)
        assert(reasons[i - 1] == Reason::None); // present 126 is the retry
    for (uint64_t i = 131; i <= 140; ++i)
        assert(reasons[i - 1] == Reason::MaskCoverage);
    const Status s = core.Snapshot();
    assert(s.coverageTripped);
    assert(s.reasons[static_cast<size_t>(Reason::MaskCoverage)] == 120 + 10);

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

// Characterization of the behaviour the guard had before the silent probe: a constant coverage over the limit makes the
// engine trip, block for exactly kRetryFrames presents, retry with a burst of presents that are swapped for a few
// frames and trip again. Every burst is two changes of the frame generation base. This is the cause of the flash at
// fades and the dialogue flicker in cutscenes; later changes replace this test.
void TestReplayFlappingToday()
{
    Replay replay;
    replay.coverage = 0.95f;
    replay.Steps(1000);
    const auto& runs = replay.runs;

    // Presents 1 and 2 have no completed image (paste not ready), 3..6 are swapped (5 fresh samples over the limit
    // arrive at presents 3..7), present 7 is the first blocked one.
    assert(runs[0].reason == Reason::PasteNotReady && runs[1].reason == Reason::PasteNotReady);
    for (size_t i = 2; i < 6; ++i)
        assert(runs[i].swapped && runs[i].reason == Reason::None);
    assert(runs[6].reason == Reason::MaskCoverage && !runs[6].staged);

    // Blocked runs: exactly 120 presents each (the last one is cut off by the end of the replay)
    std::vector<std::pair<size_t, size_t>> blocked; // first index, length
    for (size_t i = 0; i < runs.size(); ++i)
    {
        if (runs[i].reason != Reason::MaskCoverage)
            continue;
        if (!blocked.empty() && blocked.back().first + blocked.back().second == i)
            ++blocked.back().second;
        else
            blocked.push_back({ i, 1 });
    }
    assert(blocked.size() == 8);
    for (size_t k = 0; k + 1 < blocked.size(); ++k)
        assert(blocked[k].second == static_cast<size_t>(CoverageGuard::kRetryFrames));
    assert(blocked.back().second < static_cast<size_t>(CoverageGuard::kRetryFrames));

    // Between two blocked runs: one retry burst. The source was idle during the block, so the first present of the burst
    // has no boundary; the next two have no completed image (every image was withdrawn by the block); then four
    // swapped presents until the fifth fresh sample over the limit trips the guard again.
    for (size_t k = 0; k + 1 < blocked.size(); ++k)
    {
        const size_t first = blocked[k].first + blocked[k].second;
        assert(blocked[k + 1].first - first == 7);
        assert(runs[first].reason == Reason::NoBoundaryThisFrame && !runs[first].staged);
        assert(runs[first + 1].reason == Reason::PasteNotReady && runs[first + 2].reason == Reason::PasteNotReady);
        for (size_t j = 3; j < 7; ++j)
            assert(runs[first + j].swapped && runs[first + j].reason == Reason::None);
    }

    // The pattern repeats every kRetryFrames + 7 presents, each cycle is two changes of the base
    assert(blocked[1].first - blocked[0].first == static_cast<size_t>(CoverageGuard::kRetryFrames) + 7);
    assert(replay.flips >= 2 * (1000 / 127));
    assert(replay.flips == 2 + 2 * (blocked.size() - 1));
    const Status s = replay.core.Snapshot();
    assert(s.reasons[static_cast<size_t>(Reason::NoBoundaryThisFrame)] == blocked.size() - 1);
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
    TestReplayFlappingToday();
    std::puts("PASS: engine core: every fallback reason, sequencing, hysteresis, coverage guard, scene HDR priority, log lines");
    return 0;
}
