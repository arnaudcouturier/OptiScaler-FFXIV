// S10 package B: the per-present decision sequence (ext/hudless/HudlessCore.h) driven by a fake IHudlessSource and a fake
// provider / GPU host: every fallback reason of docs/HUDLESS.md section 5, the sequencing of the three seams, first failure
// wins, hysteresis of the displayed state, coverage guard (trip, retry timing, "unknown"), scene HDR priority, and the
// log lines (start, mode change, state change, periodic summary). With the mode off nothing is touched.
#include "ext/hudless/HudlessCore.h"

#include <cassert>
#include <cstdio>
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
            ++paste.produced;
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
        paste.coverage = 0.9f;
        reasons.push_back(Present(core, Mode::UiFree, paste).reason);
        if (trippedAt == 0 && reasons.back() == Reason::MaskCoverage)
            trippedAt = i;
    }
    // 3 stale frames, then 5 samples over the limit: blocking from the 8th present, for 120 presents, then a retry
    assert(trippedAt == 8);
    for (uint64_t i = 8; i < 128; ++i)
        assert(reasons[i - 1] == Reason::MaskCoverage);
    assert(reasons[127] == Reason::None); // present 128: the retry
    assert(reasons[7 - 1] == Reason::None && reasons[0] == Reason::None);
    const Status s = core.Snapshot();
    assert(s.coverageTripped || s.reasons[static_cast<size_t>(Reason::MaskCoverage)] >= 120);
    assert(s.reasons[static_cast<size_t>(Reason::MaskCoverage)] >= 120);

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

    // Guard off (CoverageGuard = 0): any coverage is fine
    FakeHost host3;
    host3.limit = 0.f;
    Core core3(host3);
    for (uint64_t i = 1; i <= 60; ++i)
    {
        PasteCounters p;
        p.produced = i;
        p.coverage = 1.0f;
        assert(Present(core3, Mode::UiFree, p).reason == Reason::None);
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
    std::puts("PASS: engine core: every fallback reason, sequencing, hysteresis, coverage guard, scene HDR priority, log lines");
    return 0;
}
