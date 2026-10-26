// S10 package B: the per-present decision sequence (ext/hudless/HudlessCore.h) driven by a fake IHudlessSource and a fake
// provider / GPU host: every fallback reason of docs/HUDLESS.md section 5, the sequencing of the three seams, first failure
// wins, hysteresis of the displayed state, coverage guard (trip, retry timing, "unknown"), scene HDR priority, and the
// log lines (start, mode change, state change, periodic summary). With the mode off nothing is touched.
#include "ext/hudless/HudlessCore.h"

#include <cassert>
#include <cstdio>
#include <deque>
#include <memory>
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
    bool anchorLater = false; // the boundary's same-frame fact: the first backbuffer UI draw came after off-screen UI draws
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
        out.anchoredAfterOffscreenUi = anchorLater;
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
    bool pausedByPreset = false; // the DLSS-G render preset takes the UI recomposition away
    GuardPolicy policy = GuardPolicy::Legacy; // [HudLess] GuardMode
    int pausedQueries = 0;
    int stages = 0;
    std::vector<std::string> log;
    int calls = 0;        // every host query / action
    int pasteQueries = 0; // PasteReady() calls

    bool FrameGenActive() override { ++calls; return fgActive; }
    Provider FrameGenProvider() override { ++calls; return provider; }
    bool HdrOutput() override { ++calls; return hdr; }
    bool DebugCompareOn() override { ++calls; return compare; }
    float CoverageLimit() override { ++calls; return limit; }
    GuardPolicy CoverageGuardPolicy() override { return policy; }
    IHudlessSource* Source() override { ++calls; return sourcePtr; }
    Reason CheckCapture(const FrameImage&) override { ++calls; return check; }
    Reason StageCapture(const FrameImage&) override { ++calls; ++stages; return stage; }
    bool PausedByPreset() override { ++calls; ++pausedQueries; return pausedByPreset; }
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

// One full present: Pre, UiFreeFrame, Post (bookkeeping only). `tags` is the tag set the host sent (-1 = what the engine sends over SDR:
// the hud-less tag exactly when the core wants tags).
Run Present(Core& core, Mode mode, const PasteCounters& paste = {}, int tags = -1)
{
    Run r {};
    r.staged = core.BeginPresent(mode, paste);
    r.swapped = core.DecideBase();
    r.reason = core.PresentReason();
    core.NoteTagSet(tags >= 0 ? static_cast<uint8_t>(tags) : (core.WantsTags() ? 1 : 0));
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

// The invariant of the traps list: exactly one of Produce / Invalidate / Hold per ui_free present, Produce on every swapped
// base, on probing frames and on PasteNotReady frames (bootstrap), Invalidate on every other fallback (Hold instead for
// the first presents after a swapped one, see TestPasteHold), none outside ui_free.
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
        assert(core.PasteDecision() != PasteAction::Hold); // nothing was swapped before: nothing to hold
        assert(core.PasteVisible() == r.swapped);
    }
    // the same right after a swapped present: a present without a capture holds instead of invalidating, a present with a
    // capture never holds, the paste stays visible unless the present invalidates
    for (unsigned bits = 0; bits < 64; ++bits)
    {
        FakeHost host;
        Core core(host);
        assert(Present(core, Mode::UiFree).swapped);
        host.fgActive = !(bits & 1);
        host.compare = bits & 2;
        host.pasteReady = !(bits & 4);
        host.sceneOwns = bits & 8;
        host.stage = (bits & 16) ? Reason::SlotBusy : Reason::None;
        host.source.result = (bits & 32) ? TakeResult::NotThisFrame : TakeResult::Ok;
        const Run r = Present(core, Mode::UiFree);
        const PasteAction action = core.PasteDecision();
        assert(action != PasteAction::None);
        assert(r.staged ? action != PasteAction::Hold : action == PasteAction::Hold);
        assert(!(r.swapped && action != PasteAction::Produce));
        assert(core.PasteVisible() == (action != PasteAction::Invalidate));
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
    static constexpr uint64_t kLatency = 2;          // presents between Produce and its completed image / readback
    static constexpr uint64_t kOutputsPerPresent = 2; // 2x frame generation: a generated frame and the real one

    FakeHost host;
    Core core { host };
    float coverage = 0.f;     // mask coverage of the images produced from now on
    bool noBoundary = false;  // the game shows no UI boundary in this present (fade to a loading screen, menu, ...)
    bool anchorFlag = false;  // the boundary says this present's frame was anchored after an off-screen UI draw
    std::vector<float> imageCoverage; // the mask coverage the present's own paste image will measure

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
    std::vector<bool> visible; // the paste is drawn over the outputs of the present (Core::PasteVisible)
    std::vector<bool> tags;    // the hud-less tag is sent for the present (Core::WantsTags)
    uint64_t flips = 0;        // changes of the swapped state
    bool lastSwapped = false;

    Run Step()
    {
        ++present;
        while (!inFlight.empty() && inFlight.front().dueAt <= present)
        {
            const Image image = inFlight.front();
            inFlight.pop_front();
            counters.coverage = image.coverage;
            counters.coverageSequence = image.ordinal;
            if (image.valid)
                completed = true;
        }
        counters.produced = produced; // builds are counted when they are submitted, as UiPaste::Snapshot() does

        host.source.result = (noBoundary || !tookLast) ? TakeResult::NotThisFrame : TakeResult::Ok;
        host.source.anchorLater = anchorFlag;
        host.pasteReady = completed;
        const int takesBefore = host.source.takes;
        const Run r = Present(core, Mode::UiFree, counters, host.hdr ? 0 : -1);
        imageCoverage.push_back(coverage);
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
        // PasteAction::Hold leaves the ring alone

        flips += r.swapped != lastSwapped;
        lastSwapped = r.swapped;
        runs.push_back(r);
        actions.push_back(action);
        visible.push_back(core.PasteVisible());
        tags.push_back(core.WantsTags());
        if (visible.back())
            counters.pasted += kOutputsPerPresent; // no GPU here: the gate stands in for "the outputs were pasted"
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
        // tripped at present 6, then probing long enough for the hold after the last swapped present to run out
        for (uint64_t i = 1; i <= 6 + 2 * Core::kLeaveHold; ++i)
        {
            p.produced = i;
            p.coverageSequence = i;
            p.coverage = 0.9f;
            Present(core, Mode::UiFree, p);
        }
        assert(core.Snapshot().coverageTripped);
        const int queriesBefore = host.pasteQueries;
        setup(host);
        p.produced = 7 + 2 * Core::kLeaveHold;
        p.coverageSequence = p.produced;
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

// The paste hold: the outputs of the first kLeaveHold presents after a swapped one still carry the last UI image, so the
// generated frame between the hud-less base and the normal base is not an unpasted blend of the two. After that the
// ring is invalidated as before.
void TestPasteHold()
{
    static_assert(Core::kLeaveHold == 2);

    struct Step
    {
        PasteAction action;
        bool visible;
        bool swapped;
        Run run;
    };
    // Mirrors the engine: the decision is read in the third seam, before EndPresent, and must not change afterwards
    auto step = [](FakeHost& host, Core& core, TakeResult result, Mode mode = Mode::UiFree)
    {
        host.source.result = result;
        Step s {};
        s.run.staged = core.BeginPresent(mode, {});
        s.run.swapped = core.DecideBase();
        s.run.reason = core.PresentReason();
        s.action = core.PasteDecision();
        s.visible = core.PasteVisible();
        s.swapped = core.Swapped();
        core.EndPresent();
        assert(core.PasteDecision() == s.action && core.PasteVisible() == s.visible);
        return s;
    };

    // The boundary is lost after a run of swapped presents
    {
        FakeHost host;
        Core core(host);
        for (int i = 0; i < 10; ++i)
        {
            const Step s = step(host, core, TakeResult::Ok);
            assert(s.swapped && s.action == PasteAction::Produce && s.visible);
        }
        for (int i = 0; i < Core::kLeaveHold; ++i)
        {
            const Step s = step(host, core, TakeResult::NotThisFrame);
            assert(!s.run.staged && !s.swapped && s.run.reason == Reason::NoBoundaryThisFrame);
            assert(s.action == PasteAction::Hold && s.visible);
        }
        for (int i = 0; i < 5; ++i)
        {
            const Step s = step(host, core, TakeResult::NotThisFrame);
            assert(s.action == PasteAction::Invalidate && !s.visible);
        }
        // the paste is ready in this fake, so the base is swapped again at once
        const Step back = step(host, core, TakeResult::Ok);
        assert(back.swapped && back.action == PasteAction::Produce && back.visible);

        // a loss shorter than the hold, then back: the hold starts over after the next swapped present
        assert(step(host, core, TakeResult::NotThisFrame).action == PasteAction::Hold);
        assert(step(host, core, TakeResult::Ok).swapped);
        assert(step(host, core, TakeResult::NotThisFrame).action == PasteAction::Hold);
        assert(step(host, core, TakeResult::NotThisFrame).action == PasteAction::Hold);
        assert(step(host, core, TakeResult::NotThisFrame).action == PasteAction::Invalidate);
    }

    // The same for every present that takes no capture
    const auto losses = { +[](FakeHost& h) { h.source.result = TakeResult::Rejected; },
                          +[](FakeHost& h) { h.check = Reason::SizeMismatch; },
                          +[](FakeHost& h) { h.stage = Reason::SlotBusy; },
                          +[](FakeHost& h) { h.compare = true; },
                          +[](FakeHost& h) { h.fgActive = false; } };
    for (auto loss : losses)
    {
        FakeHost host;
        Core core(host);
        for (int i = 0; i < 5; ++i)
            assert(step(host, core, TakeResult::Ok).swapped);
        loss(host);
        const TakeResult result = host.source.result;
        for (int i = 0; i < Core::kLeaveHold; ++i)
        {
            const Step s = step(host, core, result);
            assert(!s.run.staged && s.action == PasteAction::Hold && s.visible);
        }
        const Step s = step(host, core, result);
        assert(!s.run.staged && s.action == PasteAction::Invalidate && !s.visible);
    }

    // Scene HDR taking over a frame withdraws the paste at once, hold or not
    {
        FakeHost host;
        Core core(host);
        for (int i = 0; i < 5; ++i)
            assert(step(host, core, TakeResult::Ok).swapped);
        host.sceneOwns = true;
        const Step s = step(host, core, TakeResult::Ok);
        assert(s.run.staged && !s.swapped && s.run.reason == Reason::SceneHdrOwnsFrame);
        assert(s.action == PasteAction::Invalidate && !s.visible);
    }

    // A session that never swapped never holds: nothing was removed from the base, there is nothing to cover
    {
        FakeHost host;
        Core core(host);
        for (int i = 0; i < 20; ++i)
        {
            const Step s = step(host, core, TakeResult::NotThisFrame);
            assert(s.action == PasteAction::Invalidate && !s.visible);
        }
        host.pasteReady = false;
        for (int i = 0; i < 20; ++i)
        {
            const Step s = step(host, core, TakeResult::Ok);
            assert(s.run.reason == Reason::PasteNotReady && s.action == PasteAction::Produce && !s.visible);
        }
        for (int i = 0; i < 20; ++i)
            assert(step(host, core, TakeResult::NotThisFrame).action == PasteAction::Invalidate);
    }

    // A swap the host refused (Reject) leaves nothing to hold
    {
        FakeHost host;
        Core core(host);
        core.BeginPresent(Mode::UiFree, {});
        assert(core.DecideBase());
        core.Reject(Reason::SizeMismatch);
        assert(!core.Swapped() && core.PasteDecision() == PasteAction::Produce && !core.PasteVisible());
        core.EndPresent();
        host.source.result = TakeResult::NotThisFrame;
        core.BeginPresent(Mode::UiFree, {});
        assert(core.PasteDecision() == PasteAction::Invalidate && !core.PasteVisible());
        core.EndPresent();
    }

    // A mode change or mode off forgets the hold
    for (const Mode other : { Mode::Off, Mode::Recomposition })
    {
        FakeHost host;
        Core core(host);
        for (int i = 0; i < 5; ++i)
            assert(step(host, core, TakeResult::Ok).swapped);
        step(host, core, TakeResult::Ok, other);
        const Step s = step(host, core, TakeResult::NotThisFrame);
        assert(s.action == PasteAction::Invalidate && !s.visible);
    }

    // Recomposition has no producer decisions at all, hold included
    {
        FakeHost host;
        Core core(host);
        for (int i = 0; i < 5; ++i)
        {
            const Step s = step(host, core, TakeResult::Ok, Mode::Recomposition);
            assert(s.action == PasteAction::None && !s.visible);
        }
        const Step s = step(host, core, TakeResult::NotThisFrame, Mode::Recomposition);
        assert(s.action == PasteAction::None && !s.visible);
    }
}

// The hold covers the first kLeaveHold presents after the last swapped one on a probing present as well: the trip is a
// change of the base like a lost boundary. Probing presents beyond the hold are not pasted.
void TestPasteHoldAtTrip()
{
    Replay replay;
    replay.coverage = 0.95f;
    replay.Steps(60);
    assert(replay.runs[5].swapped && replay.runs[6].reason == Reason::MaskCoverage);
    // presents 1..2 bootstrap (nothing to paste), 3..6 swapped, then the hold, then nothing
    for (size_t i = 0; i < 2; ++i)
        assert(!replay.visible[i]);
    for (size_t i = 2; i < 6; ++i)
        assert(replay.visible[i]);
    for (size_t i = 6; i < 6 + Core::kLeaveHold; ++i)
        assert(replay.visible[i] && replay.actions[i] == PasteAction::Produce);
    for (size_t i = 6 + Core::kLeaveHold; i < replay.visible.size(); ++i)
        assert(!replay.visible[i] && replay.actions[i] == PasteAction::Produce); // still produced: the guard needs it
}

// A short loss of the boundary no longer costs paste-not-ready presents: the ring survives the hold, so the base is swapped
// again at once. A longer loss invalidates the ring and the way back goes through the paste-not-ready presents as before.
void TestHoldAvoidsPasteNotReady()
{
    for (int lost = 1; lost <= 6; ++lost)
    {
        Replay replay;
        replay.coverage = 0.10f;
        replay.Steps(20);
        replay.noBoundary = true;
        replay.Steps(lost);
        replay.noBoundary = false;
        replay.Steps(6);
        const size_t back = 20 + static_cast<size_t>(lost); // index of the first present after the loss
        const bool kept = lost <= Core::kLeaveHold;
        for (size_t i = 0; i < static_cast<size_t>(lost); ++i)
            assert(replay.actions[20 + i] == (static_cast<int>(i) < Core::kLeaveHold ? PasteAction::Hold : PasteAction::Invalidate));
        if (kept)
        {
            for (size_t i = back; i < back + 6; ++i)
                assert(replay.runs[i].swapped && replay.runs[i].reason == Reason::None);
        }
        else
        {
            assert(replay.runs[back].reason == Reason::PasteNotReady && replay.runs[back + 1].reason == Reason::PasteNotReady);
            for (size_t i = back + 2; i < back + 6; ++i)
                assert(replay.runs[i].swapped && replay.runs[i].reason == Reason::None);
        }
    }
}

// A cutscene: the mask covers about half of the screen (under the limit), and the game shows no UI boundary for 30 presents
// every 100 (fades, scene changes). Each run is exactly two changes of the base: out when the boundary is lost, back
// once the paste has a completed image again. The guard never trips.
void TestCutsceneReplay()
{
    Replay replay;
    replay.coverage = 0.50f;
    constexpr int kRuns = 10;
    for (int n = 1; n <= 1050; ++n)
    {
        replay.noBoundary = (n % 100) >= 70;
        replay.Step();
    }
    const Status s = replay.core.Snapshot();
    assert(s.reasons[static_cast<size_t>(Reason::MaskCoverage)] == 0);
    assert(s.reasons[static_cast<size_t>(Reason::NoBoundaryThisFrame)] == 30 * kRuns);
    assert(replay.flips == 1 + 2 * kRuns); // the bootstrap swap, then two per run

    // two per run, in every window around a run (runs are presents 70..99, 170..199, ...)
    for (int k = 0; k < kRuns; ++k)
    {
        const size_t from = 100 * static_cast<size_t>(k) + 60;
        const size_t to = std::min(from + 100, replay.runs.size()); // presents from+1 .. to
        int flips = 0;
        for (size_t i = from; i < to; ++i)
            flips += replay.runs[i].swapped != replay.runs[i - 1].swapped;
        assert(flips == 2);
    }

    // the first kLeaveHold presents of each run hold, the rest invalidate; the way back has the usual two presents
    // without a completed image
    size_t holds = 0, invalidates = 0;
    for (size_t i = 0; i < replay.actions.size(); ++i)
    {
        holds += replay.actions[i] == PasteAction::Hold;
        invalidates += replay.actions[i] == PasteAction::Invalidate;
    }
    assert(holds == static_cast<size_t>(Core::kLeaveHold) * kRuns);
    assert(invalidates == static_cast<size_t>(30 - Core::kLeaveHold) * kRuns);
    assert(s.reasons[static_cast<size_t>(Reason::PasteNotReady)] == 2 + 2 * kRuns);
}

// Which presents send the hud-less tag to the frame generator. In ui_free only the presents whose base was really swapped
// to the hud-less image: a present that fell back (paste not ready, probing, a refused swap, no capture) gets the same
// inputs as with the mode off. Recomposition needs the tags on every present with a capture, they ARE the feature.
void TestTagGating()
{
    auto tagsOf = [](auto&& setup, Mode mode = Mode::UiFree)
    {
        FakeHost host;
        setup(host);
        Core core(host);
        const Run r = Present(core, mode);
        return std::pair { core.WantsTags(), r };
    };
    auto noop = [](FakeHost&) {};

    {
        const auto [tags, r] = tagsOf(noop);
        assert(r.swapped && tags);
    }
    {
        const auto [tags, r] = tagsOf([](FakeHost& h) { h.pasteReady = false; });
        assert(r.staged && !r.swapped && r.reason == Reason::PasteNotReady && !tags);
    }
    {
        const auto [tags, r] = tagsOf([](FakeHost& h) { h.sceneOwns = true; });
        assert(r.staged && !r.swapped && r.reason == Reason::SceneHdrOwnsFrame && !tags);
    }
    for (auto setup : { +[](FakeHost& h) { h.fgActive = false; }, +[](FakeHost& h) { h.source.result = TakeResult::NotThisFrame; },
                        +[](FakeHost& h) { h.source.result = TakeResult::Rejected; },
                        +[](FakeHost& h) { h.check = Reason::SizeMismatch; }, +[](FakeHost& h) { h.stage = Reason::SlotBusy; } })
    {
        const auto [tags, r] = tagsOf(setup);
        assert(!r.staged && !tags);
    }

    // a swap the host undid is not a swapped present
    {
        FakeHost host;
        Core core(host);
        core.BeginPresent(Mode::UiFree, {});
        assert(core.DecideBase() && core.WantsTags());
        core.Reject(Reason::FormatUnsupported);
        assert(!core.Swapped() && !core.WantsTags());
        core.EndPresent();
    }

    // recomposition: every present with a capture, nothing without one; mode off never
    {
        const auto [tags, r] = tagsOf(noop, Mode::Recomposition);
        assert(r.staged && !r.swapped && tags);
    }
    {
        const auto [tags, r] = tagsOf([](FakeHost& h) { h.pasteReady = false; h.sceneOwns = true; }, Mode::Recomposition);
        assert(r.staged && tags);
    }
    {
        const auto [tags, r] = tagsOf([](FakeHost& h) { h.source.result = TakeResult::NotThisFrame; }, Mode::Recomposition);
        assert(!r.staged && !tags);
    }
    assert(!tagsOf(noop, Mode::Off).first);

    // over a whole trip, a cutscene and the way back: the tag follows the swapped state exactly
    Replay replay;
    for (int n = 1; n <= 3000; ++n)
    {
        replay.coverage = (n < 200) ? 0.10f : (n < 600) ? 0.95f : (n < 1400) ? 0.50f : 0.10f; // trip at 200..., release after 1400
        replay.noBoundary = (n % 250) >= 220;
        const Run r = replay.Step();
        assert(replay.core.WantsTags() == r.swapped);
    }
    assert(replay.flips > 10 && replay.core.Snapshot().captured > 1000);
}

// Log lines of the replays: the periodic summary (every 600 presents) and the state-change lines
std::vector<std::string> SummaryLines(const Replay& replay)
{
    std::vector<std::string> out;
    for (const auto& line : replay.host.log)
        if (line.find("600 presents:") != std::string::npos)
            out.push_back(line);
    return out;
}

// Normal gameplay must not change: the mask covers about 12 % of the screen, the base is swapped on every present once the
// first image completed, the paste stays visible on every present, nothing holds, nothing invalidates, every present is
// tagged. The replay has no GPU: `pasted` is modelled as two outputs per present with the paste visible (2x frame
// generation), so "pasted 1200" checks the gate present for present.
void TestSteadyGameplayReplay()
{
    Replay replay;
    replay.coverage = 0.12f;
    replay.Steps(1300);

    for (size_t i = 2; i < replay.runs.size(); ++i)
    {
        assert(replay.runs[i].staged && replay.runs[i].swapped && replay.runs[i].reason == Reason::None);
        assert(replay.actions[i] == PasteAction::Produce && replay.visible[i] && replay.tags[i]);
    }
    assert(replay.flips == 1); // the bootstrap swap, nothing after it
    assert(!replay.core.Snapshot().coverageTripped);

    // the second window is the steady state: 600 of 600 captured, one paste image per present, two outputs pasted each
    const auto lines = SummaryLines(replay);
    assert(lines.size() == 2);
    assert(lines[1] ==
           "HudLess: mode ui_free, state Active; 600 presents: 600 captured, 0 fallbacks; mask coverage avg 12.0% max 12.0%; "
           "paste built 600 pasted 1200 skipped 0; slot waits 0; boundary: ok; guard trips 0 releases 0; swapped bad frames 0; "
           "anchor-later 0 of 600 frames (0.0%), longest run 0; tag changes 0");
}

// A black screen inside a cutscene (the boundary still fires, the mask covers nearly everything): after the trip the
// window reads Fallback(MaskCoverage) with nothing captured, one paste image per present and nothing pasted.
void TestBlackScreenSummary()
{
    Replay replay;
    replay.coverage = 0.95f;
    replay.Steps(1300);
    const auto lines = SummaryLines(replay);
    assert(lines.size() == 2);
    assert(lines[1] ==
           "HudLess: mode ui_free, state Fallback(MaskCoverage); 600 presents: 0 captured, 600 fallbacks (MaskCoverage 600); "
           "mask coverage avg 95.0% max 95.0%; paste built 600 pasted 0 skipped 0; slot waits 0; boundary: ok; guard trips 0 "
           "releases 0; swapped bad frames 0; anchor-later 0 of 600 frames (0.0%), longest run 0; tag changes 0");
}

// One fade: the displayed state changes twice (into Fallback(MaskCoverage) at the trip, back to Active after the release),
// not once per swap of the base.
void TestStateLinesPerFade()
{
    Replay replay;
    for (int n = 1; n <= 1000; ++n)
    {
        replay.coverage = (n > 200 && n <= 400) ? 0.95f : 0.12f; // a fade of 200 presents
        replay.Step();
    }
    // The first Active is reached after the two bootstrap presents (the tracker passes through Fallback(PasteNotReady) on
    // the way, because 8 of the first 10 presents are not yet 90 %): count the lines after it.
    std::vector<std::string> states;
    bool active = false;
    for (const auto& line : replay.host.log)
    {
        if (line.rfind("HudLess: state ", 0) != 0)
            continue;
        if (active)
            states.push_back(line);
        else if (line.ends_with("-> Active"))
            active = true;
    }
    assert(active);
    assert(states.size() == 2);
    assert(states[0] == "HudLess: state Active -> Fallback(MaskCoverage)");
    assert(states[1] == "HudLess: state Fallback(MaskCoverage) -> Active");
    assert(replay.flips == 3); // bootstrap swap, trip, release
    assert(replay.core.Snapshot().reasons[static_cast<size_t>(Reason::PasteNotReady)] == 2);
}
} // namespace

// A render preset that takes NVIDIA's UI recomposition away (A, C) makes the recomposition tags and captures useless: the engine asks for
// nothing (the UI-boundary source is armed only by Take(), so its capture goes idle), stages and tags nothing, and says why.
void TestPausedByPreset()
{
    // recomposition + a dropping preset: no Take, no staging, no tags, the reason is counted and becomes the displayed fallback
    {
        FakeHost host;
        host.pausedByPreset = true;
        Core core(host);
        for (int i = 0; i < 5; ++i)
        {
            const Run r = Present(core, Mode::Recomposition);
            assert(!r.staged && !r.swapped && r.reason == Reason::PausedByPreset);
            assert(!core.WantsTags() && !core.Staged());
        }
        assert(core.Snapshot().phase == Phase::Searching); // not yet a verdict
        for (int i = 0; i < 200; ++i)
        {
            const Run r = Present(core, Mode::Recomposition);
            assert(!r.staged && r.reason == Reason::PausedByPreset);
        }
        assert(host.source.takes == 0 && host.source.activeCalls == 0 && host.stages == 0);
        const Status s = core.Snapshot();
        assert(s.phase == Phase::Fallback && s.reason == Reason::PausedByPreset && s.captured == 0);
        assert(s.reasons[static_cast<size_t>(Reason::PausedByPreset)] == 205);
        assert(StateText(s.phase, s.reason) == "Fallback(PausedByPreset)");
        assert(std::string(ReasonText(Reason::PausedByPreset)).find("preset") != std::string::npos);
    }
    // the preset goes back to Auto / B (a live switch): the capture resumes at once and the state recovers
    {
        FakeHost host;
        host.pausedByPreset = true;
        Core core(host);
        for (int i = 0; i < 100; ++i)
            Present(core, Mode::Recomposition);
        assert(core.Snapshot().phase == Phase::Fallback);
        host.pausedByPreset = false;
        const Run first = Present(core, Mode::Recomposition);
        assert(first.staged && first.reason == Reason::None && core.WantsTags());
        for (int i = 0; i < 80; ++i)
            Present(core, Mode::Recomposition);
        assert(core.Snapshot().phase == Phase::Active && host.source.takes == 81);
    }
    // only the recomposition is affected: ui_free (the recommended mode with C) is never asked, so it is never paused by the preset
    {
        FakeHost host;
        host.pausedByPreset = true;
        Core core(host);
        const Run r = Present(core, Mode::UiFree);
        assert(r.staged && r.swapped && r.reason == Reason::None && host.pausedQueries == 0);
    }
    // frame generation off or an unsupported provider is reported as that, not as the preset
    {
        FakeHost host;
        host.pausedByPreset = true;
        host.fgActive = false;
        Core core(host);
        assert(Present(core, Mode::Recomposition).reason == Reason::NoFrameGeneration);
    }
    {
        FakeHost host;
        host.pausedByPreset = true;
        host.provider = Provider::None;
        Core core(host);
        assert(Present(core, Mode::Recomposition).reason == Reason::ProviderUnsupported);
    }
    // mode off asks nothing at all
    {
        FakeHost host;
        host.pausedByPreset = true;
        Core core(host);
        Present(core, Mode::Off);
        assert(host.calls == 0 && host.pausedQueries == 0);
    }
}

// ---- the decisions of the engine core, recorded -----------------------------------------------------------------------------------
// A deterministic mix of coverage, lost boundaries and fades is replayed and every present's decision (staged, swapped, reason, paste
// action, paste visible, tags wanted) is folded into one hash. The literals were recorded before the log counters existed: the counters
// only watch, so the same streams must keep producing them (and the boundary's same-frame facts, which the counters read, must not
// move a decision in the legacy guard).
struct Lcg
{
    uint32_t s = 12345;
    uint32_t Next()
    {
        s = s * 1664525u + 1013904223u;
        return s >> 8;
    }
};

uint64_t DecisionDigest(const Replay& r)
{
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v)
    {
        h ^= v;
        h *= 1099511628211ull;
    };
    for (size_t i = 0; i < r.runs.size(); ++i)
    {
        mix(r.runs[i].staged);
        mix(r.runs[i].swapped);
        mix(static_cast<uint64_t>(r.runs[i].reason));
        mix(static_cast<uint64_t>(r.actions[i]));
        mix(r.visible[i]);
        mix(r.tags[i]);
    }
    mix(r.flips);
    return h;
}

// Scenario k: the stream the digest is taken over. `flagged` (the boundary's anchor-later fact) is set by `flag(present)` when given.
template <typename FlagFn> uint64_t ScenarioDigest(int k, FlagFn&& flag)
{
    Replay replay;
    Lcg rng;
    int burstLeft = 0;
    for (int n = 1; n <= 4000; ++n)
    {
        switch (k)
        {
        case 0: // steady HUD
            replay.coverage = 0.12f;
            break;
        case 1: // random mix: calm, half-screen windows, fades, lost boundaries
        {
            const uint32_t v = rng.Next() % 100;
            replay.coverage = v < 70 ? 0.12f : (v < 85 ? 0.50f : 0.95f);
            replay.noBoundary = (rng.Next() % 100) < 4;
            break;
        }
        case 2: // every other frame bad
            replay.coverage = (n % 2) ? 0.95f : 0.10f;
            break;
        case 3: // every fourth frame bad
            replay.coverage = (n % 4 == 0) ? 0.95f : 0.10f;
            break;
        default: // bursts of 5-60 bad frames
            if (burstLeft == 0 && (rng.Next() % 150) == 0)
                burstLeft = 5 + static_cast<int>(rng.Next() % 56);
            replay.coverage = burstLeft > 0 ? 0.95f : 0.10f;
            if (burstLeft > 0)
                --burstLeft;
            break;
        }
        flag(replay, n);
        replay.Step();
    }
    return DecisionDigest(replay);
}

void TestDecisionsRecorded()
{
    // Streams 0, 2 and 3 share a value: with the legacy guard an alternating or every-fourth bad frame never trips it, so every
    // present is swapped, exactly as in steady play.
    static constexpr uint64_t kRecorded[5] = { 11994251758413254906ull, 15914297416367574552ull, 11994251758413254906ull,
                                               11994251758413254906ull, 1196904380689745642ull };
    uint64_t got[5];
    for (int k = 0; k < 5; ++k)
    {
        got[k] = ScenarioDigest(k, [](Replay&, int) {});
        std::printf("scenario %d digest %llu\n", k, static_cast<unsigned long long>(got[k]));
        std::fflush(stdout);
    }
    for (int k = 0; k < 5; ++k)
        assert(got[k] == kRecorded[k]);
    // The boundary's anchor-later fact is only counted: any pattern of it leaves every decision where it was
    for (int k = 0; k < 5; ++k)
        assert(ScenarioDigest(k, [](Replay& r, int n) { r.anchorFlag = (n % 11) < 6 || (n % 97) < 40; }) == kRecorded[k]);
}

// ---- the counters of the 600-present line ------------------------------------------------------------------------------------------
uint64_t NumberAfter(const std::string& line, const std::string& key)
{
    const size_t at = line.find(key);
    assert(at != std::string::npos);
    return std::stoull(line.substr(at + key.size()));
}

// What follows "boundary: ok; ": the counters, exactly as written
std::string CounterTail(const Replay& replay)
{
    const auto lines = SummaryLines(replay);
    assert(!lines.empty());
    const size_t at = lines[0].find("; guard trips ");
    assert(at != std::string::npos);
    return lines[0].substr(at + 2);
}

void TestLogCounters()
{
    // Three bad frames, never the five in a row that trip the legacy guard: each swapped base whose own image measured over the limit
    // is counted when that readback arrives. Two runs of anchor-later frames (5 and 3), 600 acting frames. The base is swapped from
    // present 3 on: one change of the tag set.
    {
        Replay r;
        for (int n = 1; n <= 600; ++n)
        {
            r.coverage = (n >= 100 && n <= 102) ? 0.95f : 0.10f;
            r.anchorFlag = (n >= 300 && n <= 304) || (n >= 400 && n <= 402);
            r.Step();
        }
        assert(CounterTail(r) == "guard trips 0 releases 0; swapped bad frames 3; anchor-later 8 of 600 frames (1.3%), longest run 5; "
                                 "tag changes 1");
    }
    // A burst of 20 bad frames: the legacy guard needs five readbacks, so the bad frames 300..305 were swapped (six); the probing
    // presents after the trip are not. One trip, one release; the base goes out at the trip and back at the release (tag set 0 -> 1
    // -> 0 -> 1 = three changes).
    {
        Replay r;
        for (int n = 1; n <= 600; ++n)
        {
            r.coverage = (n >= 300 && n <= 319) ? 0.95f : 0.10f;
            r.Step();
        }
        assert(r.flips == 3);
        assert(CounterTail(r) == "guard trips 1 releases 1; swapped bad frames 6; anchor-later 0 of 600 frames (0.0%), longest run 0; "
                                 "tag changes 3");
    }
    // Over HDR output the engine sends no tags at all: the base still changes three times, the tag set never
    {
        Replay r;
        r.host.hdr = true;
        for (int n = 1; n <= 600; ++n)
        {
            r.coverage = (n >= 300 && n <= 319) ? 0.95f : 0.10f;
            r.Step();
        }
        assert(r.flips == 3);
        assert(CounterTail(r).ends_with("tag changes 0"));
    }
    // A present that sent nothing (the host noted no tag set, as when it returns before tagging) is a present without tags
    {
        FakeHost host;
        Core core(host);
        core.BeginPresent(Mode::UiFree, {});
        core.DecideBase();
        core.NoteTagSet(1);
        core.EndPresent();
        core.BeginPresent(Mode::UiFree, {});
        core.DecideBase();
        core.EndPresent();
        for (int n = 3; n <= 600; ++n)
            Present(core, Mode::UiFree);
        std::string line;
        for (const auto& l : host.log)
            if (l.find("600 presents") != std::string::npos)
                line = l;
        assert(!line.empty() && line.ends_with("tag changes 3")); // 0 -> 1, 1 -> 0, 0 -> 1
    }
    // The tag set is a set: hud-less alone and hud-less with the UI image are two different sets
    {
        FakeHost host;
        Core core(host);
        for (int n = 1; n <= 600; ++n)
            Present(core, Mode::Recomposition, {}, n < 100 ? 3 : (n < 200 ? 1 : 3));
        std::string line;
        for (const auto& l : host.log)
            if (l.find("600 presents") != std::string::npos)
                line = l;
        assert(line.ends_with("tag changes 3")); // 0 -> 3, 3 -> 1, 1 -> 3
    }
    // A random stream against a reference computed from the replay's own records (the swapped state and the measured coverage of every
    // present); anchor-later frames in runs of 7
    {
        Replay r;
        Lcg rng;
        for (int n = 1; n <= 600; ++n)
        {
            const uint32_t v = rng.Next() % 100;
            r.coverage = v < 70 ? 0.12f : (v < 85 ? 0.50f : 0.95f);
            r.noBoundary = (rng.Next() % 100) < 4;
            r.anchorFlag = (n % 40) < 7;
            r.Step();
        }
        uint64_t swappedBad = 0, flagged = 0, longest = 0, run = 0;
        for (size_t i = 0; i < 600; ++i)
        {
            if (r.actions[i] == PasteAction::Produce && r.runs[i].swapped && r.imageCoverage[i] > 0.60f &&
                i + 1 + Replay::kLatency <= 600)
                ++swappedBad; // its readback arrives kLatency presents later, inside this window
            const bool flag = (static_cast<int>(i + 1) % 40) < 7;
            flagged += flag;
            run = flag ? run + 1 : 0;
            longest = std::max(longest, run);
        }
        const auto lines = SummaryLines(r);
        assert(!lines.empty() && swappedBad > 0 && flagged > 0);
        assert(NumberAfter(lines[0], "swapped bad frames ") == swappedBad);
        assert(NumberAfter(lines[0], "anchor-later ") == flagged);
        assert(NumberAfter(lines[0], "longest run ") == longest);
    }
    // A guard that is off (CoverageGuard = 0) has no limit to be over: nothing is a bad frame
    {
        Replay r;
        r.host.limit = 0.f;
        for (int n = 1; n <= 600; ++n)
        {
            r.coverage = 0.95f;
            r.Step();
        }
        assert(CounterTail(r).find("swapped bad frames 0;") != std::string::npos);
    }
}

// ---- [HudLess] GuardMode = density, end to end ---------------------------------------------------------------------------------------
// Streams where a frame is bad exactly when the boundary says it was anchored after off-screen UI draws (the cause class the policy is
// for); a bad frame's image measures 0.95, a clean one 0.10. Flips are changes of the swapped base; 3600 presents are one minute at 60 fps.
struct StreamResult
{
    uint64_t flips = 0;
    uint64_t swappedBad = 0;           // swapped presents whose own image was bad
    uint64_t swappedBadAfterTrip = 0;  // ... after the first present that fell back to MaskCoverage
    bool tripped = false;
    size_t releasedAt = 0;             // index of the first swapped present after the first trip (0 = none)
};

template <typename BadFn> StreamResult RunStream(GuardPolicy policy, int presents, BadFn&& badAt, Replay* keep = nullptr)
{
    Replay local;
    Replay& r = keep ? *keep : local;
    r.host.policy = policy;
    for (int n = 1; n <= presents; ++n)
    {
        const bool bad = badAt(n);
        r.coverage = bad ? 0.95f : 0.10f;
        r.anchorFlag = bad;
        r.Step();
    }
    StreamResult out;
    out.flips = r.flips;
    for (size_t i = 0; i < r.runs.size(); ++i)
    {
        if (!out.tripped && r.runs[i].reason == Reason::MaskCoverage)
            out.tripped = true;
        else if (out.tripped && out.releasedAt == 0 && r.runs[i].swapped)
            out.releasedAt = i;
        if (r.runs[i].swapped && r.imageCoverage[i] > 0.60f)
        {
            ++out.swappedBad;
            out.swappedBadAfterTrip += out.tripped;
        }
    }
    return out;
}

void TestDensityReplay()
{
    constexpr int kMinute = 3600;
    const StreamResult legacy2 = RunStream(GuardPolicy::Legacy, kMinute, [](int n) { return n % 2 == 0; });
    const StreamResult dense2 = RunStream(GuardPolicy::Density, kMinute, [](int n) { return n % 2 == 0; });
    // every other frame bad: the legacy guard never sees five in a row, so it pastes about half of a minute's frames bad; the density
    // guard trips within the first dozen presents, and from its first trip on no bad frame is ever swapped
    assert(legacy2.swappedBad > 1500 && !legacy2.tripped && legacy2.flips == 1);
    assert(dense2.tripped && dense2.swappedBad <= 8 && dense2.swappedBadAfterTrip == 0 && dense2.releasedAt == 0 && dense2.flips == 2);

    const StreamResult legacy4 = RunStream(GuardPolicy::Legacy, kMinute, [](int n) { return n % 4 == 0; });
    const StreamResult dense4 = RunStream(GuardPolicy::Density, kMinute, [](int n) { return n % 4 == 0; });
    assert(legacy4.swappedBad > 700 && !legacy4.tripped);
    assert(dense4.tripped && dense4.swappedBad <= 8 && dense4.swappedBadAfterTrip == 0 && dense4.releasedAt == 0);

    // bursts of 5 to 60 bad frames with 35 clean presents between them: legacy releases after 30 clean samples and trips again at the
    // next burst, density needs 60 and stays in the fallback (time in fallback looks like mode off)
    for (const int burst : { 5, 10, 20, 40, 60 })
    {
        const int period = burst + 35;
        auto bursts = [=](int n) { return ((n - 1) % period) < burst; };
        const StreamResult l = RunStream(GuardPolicy::Legacy, kMinute, bursts);
        const StreamResult d = RunStream(GuardPolicy::Density, kMinute, bursts);
        assert(l.flips >= 2 * static_cast<uint64_t>(kMinute / period - 2)); // out and back in, every burst
        assert(d.flips == 2);                                                 // the bootstrap swap and the one trip
        assert(d.flips * 10 < l.flips);
        assert(d.swappedBadAfterTrip == 0 && d.swappedBad < l.swappedBad);
    }

    // far apart bursts (the guard releases between them under both policies): never more flips than legacy, and a burst costs at most
    // the three bad frames that trip it plus the two presents the readback takes
    for (const int burst : { 5, 20, 60 })
    {
        const int period = burst + 200;
        auto bursts = [=](int n) { return ((n - 1) % period) < burst; };
        const StreamResult l = RunStream(GuardPolicy::Legacy, 5 * period, bursts);
        const StreamResult d = RunStream(GuardPolicy::Density, 5 * period, bursts);
        assert(d.flips <= l.flips);
        assert(d.swappedBad <= 5u * 5u);
        assert(d.swappedBad <= l.swappedBad);
    }
}

// The boundary's same-frame fact holds a tripped guard at the very present that would have released it, instead of waiting for the
// readback of the bad frame (two presents later): that frame is not swapped.
void TestDensityHoldsRelease()
{
    auto stream = [](bool flagTheBadFrame)
    {
        auto r = std::make_unique<Replay>();
        r->host.policy = GuardPolicy::Density;
        for (int n = 1; n <= 150; ++n)
        {
            const bool bad = (n >= 10 && n <= 12) || n == 74;
            r->coverage = bad ? 0.95f : 0.10f;
            r->anchorFlag = bad && (flagTheBadFrame || n != 74);
            r->Step();
        }
        return r;
    };

    // images 10..12 are the three bad samples (arrive at presents 12..14): tripped at present 14; images 13.. are clean, the 60th clean
    // sample arrives at present 74, the present whose own frame is bad
    const auto plain = stream(false); // the frame is bad but the boundary did not say so: released, the bad frame is swapped
    assert(plain->runs[13].reason == Reason::MaskCoverage && !plain->runs[72].swapped);
    assert(plain->runs[73].swapped && plain->imageCoverage[73] > 0.60f);

    const auto held = stream(true);
    for (size_t i = 13; i <= 134; ++i)
        assert(!held->runs[i].swapped && held->runs[i].reason == Reason::MaskCoverage);
    assert(held->imageCoverage[73] > 0.60f);
    // the bad frame's own readback (present 76) restarts the count; 60 clean samples later (present 136) the guard lets go
    assert(held->runs[135].swapped && held->runs[135].reason == Reason::None);

    // the paste hold is the old one: the first kLeaveHold presents after the last swapped one still show the paste, then it is gone
    for (size_t i = 13; i < 13 + Core::kLeaveHold; ++i)
        assert(held->visible[i] && held->actions[i] == PasteAction::Produce);
    for (size_t i = 13 + Core::kLeaveHold; i <= 134; ++i)
        assert(!held->visible[i] && held->actions[i] == PasteAction::Produce);
}

// A readback that stops arriving: the starvation release is the same 120 probed presents, with the density policy too
void TestDensityStarvation()
{
    FakeHost host;
    host.policy = GuardPolicy::Density;
    Core core(host);
    PasteCounters frozen;
    std::vector<Run> runs;
    for (uint64_t i = 1; i <= 4; ++i)
    {
        frozen.produced = i;
        frozen.coverageSequence = i;
        frozen.coverage = 0.9f;
        runs.push_back(Present(core, Mode::UiFree, frozen));
    }
    // baseline at present 1, three bad samples at 2..4: tripped at present 4 (the legacy guard needs five)
    assert(runs[2].reason == Reason::None && runs[3].reason == Reason::MaskCoverage);
    for (uint64_t i = 5; i <= 4 + CoverageGuard::kStarveFrames; ++i)
        runs.push_back(Present(core, Mode::UiFree, frozen)); // the same readback again and again
    for (uint64_t i = 4; i < 4 + CoverageGuard::kStarveFrames; ++i)
        assert(runs[i - 1].reason == Reason::MaskCoverage); // presents 4..123
    assert(runs[4 + CoverageGuard::kStarveFrames - 1].reason == Reason::None && runs[4 + CoverageGuard::kStarveFrames - 1].swapped);
    assert(!core.Snapshot().coverageTripped);
}

// A tripped guard whose due release is not decided by a present that reached the boundary (no frame generation, say) still lets go
void TestDensityReleaseWithoutBoundary()
{
    FakeHost host;
    host.policy = GuardPolicy::Density;
    Core core(host);
    PasteCounters p;
    auto step = [&](uint64_t i, float coverage)
    {
        p.produced = i;
        p.coverageSequence = i;
        p.coverage = coverage;
        return Present(core, Mode::UiFree, p);
    };
    for (uint64_t i = 1; i <= 4; ++i)
        step(i, 0.9f); // tripped at present 4
    assert(core.Snapshot().coverageTripped);
    for (uint64_t i = 5; i < 5 + CoverageGuard::kDensityReleaseSamples - 1; ++i)
        step(i, 0.1f);
    assert(core.Snapshot().coverageTripped);
    host.fgActive = false; // the present that brings the 60th clean sample never gets as far as the boundary
    step(5 + CoverageGuard::kDensityReleaseSamples - 1, 0.1f);
    assert(!core.Snapshot().coverageTripped);
    host.fgActive = true;
    assert(step(5 + CoverageGuard::kDensityReleaseSamples, 0.1f).swapped);
}

// The policy is announced once, only when it is not the default; legacy writes no new line
void TestDensityLogLine()
{
    auto lines = [](GuardPolicy policy)
    {
        Replay r;
        r.host.policy = policy;
        r.Steps(50);
        std::vector<std::string> out;
        for (const auto& l : r.host.log)
            if (l.rfind("HudLess: coverage guard", 0) == 0)
                out.push_back(l);
        return out;
    };
    assert(lines(GuardPolicy::Legacy).empty());
    const auto density = lines(GuardPolicy::Density);
    assert(density.size() == 1 && density[0].find("density") != std::string::npos);
    // 3 of the last 12 already cover 3 in a row: the text names no separate "in a row" trip
    assert(density[0].find("or 5 in a row") == std::string::npos);

    // It is written whenever density becomes active: again after a switch away and back
    Replay r;
    r.host.policy = GuardPolicy::Density;
    r.Steps(20);
    r.host.policy = GuardPolicy::Legacy;
    r.Steps(20);
    r.host.policy = GuardPolicy::Density;
    r.Steps(20);
    size_t count = 0;
    for (const auto& l : r.host.log)
        count += l.rfind("HudLess: coverage guard", 0) == 0;
    assert(count == 2);
}

// ---- edges of the log counters that the first round left open (found by the W2 mutation checks) --------------------------------------
std::string SummaryOf(const FakeHost& host, const char* presents = "600 presents")
{
    std::string line;
    for (const auto& l : host.log)
        if (l.find(presents) != std::string::npos)
            line = l;
    return line;
}

void TestCounterEdges()
{
    // A swapped base whose image measures exactly at the limit is not a bad frame (bad = over the limit)
    {
        Replay r;
        for (int n = 1; n <= 600; ++n)
        {
            r.coverage = 0.60f;
            r.Step();
        }
        assert(CounterTail(r).find("swapped bad frames 0;") != std::string::npos);
    }
    // A readback whose ring entry belongs to another image (the ring wrapped) is attributed to nothing: 132 shares the entry of 100
    {
        FakeHost host;
        Core core(host);
        PasteCounters p;
        for (uint64_t n = 1; n <= 600; ++n)
        {
            p.produced = n; // one image per present, the base swapped from the first present on
            p.coverageSequence = n == 100 ? 132 : n;
            p.coverage = n == 100 ? 0.95f : 0.10f;
            Present(core, Mode::UiFree, p);
        }
        assert(SummaryOf(host).find("swapped bad frames 0;") != std::string::npos);
    }
    // The longest run is a figure of its own window: a run of ten anchor-later frames across the window edge is 5 + 5
    {
        Replay r;
        for (int n = 1; n <= 1200; ++n)
        {
            r.anchorFlag = n >= 596 && n <= 605;
            r.Step();
        }
        const auto lines = SummaryLines(r);
        assert(lines.size() == 2);
        assert(NumberAfter(lines[0], "longest run ") == 5 && NumberAfter(lines[1], "longest run ") == 5);
    }
    // The counters start again with a mode change: the run of anchor-later frames and the tag set of the old mode are not carried over
    {
        FakeHost host;
        Core core(host);
        for (int n = 1; n <= 100; ++n)
        {
            host.source.anchorLater = n >= 96;
            Present(core, Mode::UiFree, {}, 1);
        }
        for (int n = 101; n <= 700; ++n)
        {
            host.source.anchorLater = n <= 103;
            Present(core, Mode::Recomposition, {}, 1);
        }
        const std::string line = SummaryOf(host);
        assert(!line.empty() && line.find("mode recomposition") != std::string::npos);
        assert(NumberAfter(line, "longest run ") == 3);
        assert(line.ends_with("tag changes 1")); // 0 -> 1 at the first present of the new mode
    }
    // ... and a readback of an image the old mode built is not attributed to the new one
    {
        FakeHost host;
        Core core(host);
        PasteCounters p;
        for (uint64_t n = 1; n <= 50; ++n)
        {
            p.produced = n;
            p.coverageSequence = n;
            p.coverage = 0.10f;
            Present(core, Mode::UiFree, p);
        }
        p.produced = 50;
        Present(core, Mode::Off, p);
        for (uint64_t n = 51; n <= 700; ++n)
        {
            p.produced = n;
            p.coverageSequence = n == 52 ? 48 : n; // the late readback of image 48 (built over a swapped base), over the limit
            p.coverage = n == 52 ? 0.95f : 0.10f;
            Present(core, Mode::UiFree, p);
        }
        assert(SummaryOf(host).find("swapped bad frames 0;") != std::string::npos);
    }
    // The fact is kept per present whatever the result of the Take: a due release meets an anchor-later present whose Take found no
    // boundary, and the guard still holds (the release comes at the 60th clean sample, present 74, as in TestDensityHoldsRelease)
    {
        auto stream = [](bool fact)
        {
            auto r = std::make_unique<Replay>();
            r->host.policy = GuardPolicy::Density;
            for (int n = 1; n <= 80; ++n)
            {
                r->coverage = (n >= 10 && n <= 12) ? 0.95f : 0.10f;
                r->noBoundary = n == 74;
                r->anchorFlag = fact && n == 74;
                r->Step();
            }
            return r;
        };
        const auto freed = stream(false);
        const auto held = stream(true);
        assert(freed->runs[74].reason != Reason::MaskCoverage); // without the fact the guard let go at present 74
        assert(held->runs[74].reason == Reason::MaskCoverage);  // with it, present 75 is still a probing one
    }
    // A release decided in the last present of a window is counted in that window: by the boundary's answer, or at the end of a
    // present that never got as far as the boundary. Three bad images (536..538) trip the guard at present 540, the 60th clean one
    // arrives at present 600.
    for (const bool boundary : { true, false })
    {
        Replay r;
        r.host.policy = GuardPolicy::Density;
        for (int n = 1; n <= 600; ++n)
        {
            r.coverage = (n >= 536 && n <= 538) ? 0.95f : 0.10f;
            r.host.fgActive = boundary || n != 600;
            r.Step();
        }
        const auto lines = SummaryLines(r);
        assert(lines.size() == 1);
        assert(NumberAfter(lines[0], "guard trips ") == 1 && NumberAfter(lines[0], "releases ") == 1);
    }
    // The announcement names the numbers the guard really uses
    {
        Replay r;
        r.host.policy = GuardPolicy::Density;
        r.Steps(5);
        std::string line;
        for (const auto& l : r.host.log)
            if (l.rfind("HudLess: coverage guard", 0) == 0)
                line = l;
        assert(line.find("trips on " + std::to_string(CoverageGuard::kDensityTripBad) + " bad of the last " +
                         std::to_string(CoverageGuard::kDensityWindow) + " samples") != std::string::npos);
        assert(line.find("releases after " + std::to_string(CoverageGuard::kDensityReleaseSamples) + " clean ones in a row") !=
               std::string::npos);
    }
    // A host that does not answer the policy runs today's guard
    {
        struct PlainHost final : IEngineHost
        {
            bool FrameGenActive() override { return true; }
            Provider FrameGenProvider() override { return Provider::DlssG; }
            bool HdrOutput() override { return false; }
            bool DebugCompareOn() override { return false; }
            float CoverageLimit() override { return 0.6f; }
            IHudlessSource* Source() override { return nullptr; }
            Reason CheckCapture(const FrameImage&) override { return Reason::None; }
            Reason StageCapture(const FrameImage&) override { return Reason::None; }
            bool PausedByPreset() override { return false; }
            bool PasteReady() override { return true; }
            bool SceneHdrOwnsFrame() override { return false; }
            void Log(const std::string&) override {}
        } host;
        assert(host.CoverageGuardPolicy() == GuardPolicy::Legacy);
    }
}

int main()
{
    TestOff();
    TestDecisionsRecorded();
    TestLogCounters();
    TestDensityReplay();
    TestDensityHoldsRelease();
    TestDensityStarvation();
    TestDensityReleaseWithoutBoundary();
    TestDensityLogLine();
    TestCounterEdges();
    TestPausedByPreset();
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
    TestPasteHold();
    TestPasteHoldAtTrip();
    TestHoldAvoidsPasteNotReady();
    TestCutsceneReplay();
    TestTagGating();
    TestSteadyGameplayReplay();
    TestBlackScreenSummary();
    TestStateLinesPerFade();
    std::puts("PASS: engine core: every fallback reason, sequencing, hysteresis, coverage guard, scene HDR priority, log lines");
    return 0;
}
