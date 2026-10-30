// The native nameplate engine protocol (ext/hudless/UiPlates.cpp) on WARP: the production module compiled with shadowed
// includes, a checked leaf mutex that fails the test on a re-lock or a call made under it, a real D3D11 game device and a
// real D3D12 bridge device. Every scenario plays the game side (reserve, draw into the slot, frame finish) and the
// presentation side (outputs) one step at a time, and checks which frame's plate image each output received.
//
// The ledger rules checked after every scenario: no output receives a plate image of a frame the game drew natively; at
// most one image per output; a hidden frame's image reaches every output presented in its own window; a declined or
// native frame is followed by no stale hidden image; no pin survives an output; no leaf lock is held across a call into
// the game side, the log or the pass's stand-ins.
#include "pch.h"

#include "plate_harness.h"

#include "ext/fg/dlssg/PresetStatus.h"
#include "ext/hudless/HudlessConfig.h"
#include "ext/hudless/HudlessEngine.h"
#include "ext/hudless/PlateSync.h"

#include "plate_test_clock.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <string>
#include <thread>
#include <vector>

using namespace plate_test;
using ext::hudless::PlateInterp;
using ext::hudless::plates::Counters;
using ext::hudless::plates::FrameMode;

// The HUD-less engine's own bodies are not linked here (they own a second GPU engine and its paste): the seam wrappers
// under test are counted through them, so a scenario says how often the wiring reached the HUD-less engine at all.
int g_detailCalls = 0;

namespace ext::hudless::detail
{
void PreInteropCopy(Dx11wDx12SC&) { ++g_detailCalls; }
void UiFreeFrame(Dx11wDx12SC&, unsigned int, ID3D12Resource*&, D3D12_RESOURCE_STATES&) { ++g_detailCalls; }
void PostInteropCopy(Dx11wDx12SC&) { ++g_detailCalls; }
void OnReleaseInterop(Dx11wDx12SC&) { ++g_detailCalls; }
void BeforeOverlay(IDXGISwapChain*, ID3D12CommandQueue*) { ++g_detailCalls; }
void PlateProbePost(Dx11wDx12SC&) { ++g_detailCalls; }
void PlateProbeExpected(uint32_t) { ++g_detailCalls; }
void PlateProbeOutput(IDXGISwapChain*, ID3D12CommandQueue*) { ++g_detailCalls; }
} // namespace ext::hudless::detail

namespace
{
// The scenarios that run a side on another thread add to it from there.
std::atomic<int> g_failures { 0 };
#undef EXPECT

#define EXPECT(cond, ...)                                                                                              \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            ++g_failures;                                                                                              \
            std::printf("    FAIL line %d: %s  ", __LINE__, #cond);                                                    \
            std::printf(__VA_ARGS__);                                                                                  \
            std::printf("\n");                                                                                         \
        }                                                                                                              \
    } while (0)

const char* ModeName(FrameMode mode)
{
    switch (mode)
    {
    case FrameMode::Hidden:
        return "hidden";
    case FrameMode::Shadow:
        return "shadow";
    default:
        return "declined";
    }
}

// The product's bit sink: the engine clears the feature bit when nothing is drawable any more and sets it again when the
// key came back on. A scenario that routes its presents through the seams needs the seams to stop with the bit.
void BitSinkWithFeature(bool on)
{
    RecordBit(on);
    ext::SetActive(ext::F::Plate, on);
}

// A clean engine for the next scenario: released, key off, a bit sink the scenario reads, DLSS-G, SDR, no preset pause.
struct Scenario
{
    Driver driver;
    Counters base;

    explicit Scenario(unsigned multiplier = 3, UINT width = 64, UINT height = 32) : driver(width, height, multiplier)
    {
        ext::hudless::plates::OnRelease();
        ext::hudless::PublishPlateSettings({ PlateInterp::Off, 1, 24 });
        ext::hudless::plates::SetPlateFeatureBitSink(&RecordBit);
        plate_test_clock::Reset();
        BitCalls().clear();
        BitHook = nullptr;
        State::Instance().isShuttingDown = false;
        State::Instance().activeFgOutput = FGOutput::DLSSG;
        ext::hudless::TestMode() = ext::hudless::Mode::Off;
        ext::hudless::TestRecomposition().enabled = false;
        ext::fg::preset::TestDecision().droppedByPreset = false;
        driver.Multiplier(multiplier);
        base = driver.Counters();
    }

    ~Scenario()
    {
            BitHook = nullptr;
        ext::hudless::plates::SetPlateFeatureBitSink(nullptr);
        ext::hudless::plates::OnRelease();
        ext::hudless::PublishPlateSettings({ PlateInterp::Off, 1, 24 });
        ext::hudless::TestMode() = ext::hudless::Mode::Off;
        ext::fg::preset::TestDecision().droppedByPreset = false;
        ext::hudless::plates::sync::ClearViolations();
        }

    Counters Now() const { return driver.Counters(); }

    // The counters this scenario changed (the engine is process-wide, so every check is a delta).
    Counters Delta() const
    {
        const Counters now = Now();
        Counters delta;
        delta.promoted = now.promoted - base.promoted;
        delta.hidden = now.hidden - base.hidden;
        delta.shadow = now.shadow - base.shadow;
        delta.native = now.native - base.native;
        delta.declined = now.declined - base.declined;
        delta.slotsCreated = now.slotsCreated - base.slotsCreated;
        delta.slotsRetired = now.slotsRetired - base.slotsRetired;
        delta.empty = now.empty - base.empty;
        delta.retiredStale = now.retiredStale - base.retiredStale;
        delta.reclaimed = now.reclaimed - base.reclaimed;
        delta.plated = now.plated - base.plated;
        delta.skipped = now.skipped - base.skipped;
        delta.hiddenSkipped = now.hiddenSkipped - base.hiddenSkipped;
        delta.promoteFailedHidden = now.promoteFailedHidden - base.promoteFailedHidden;
        delta.lateOutputs = now.lateOutputs - base.lateOutputs;
        delta.earlyOutputs = now.earlyOutputs - base.earlyOutputs;
        delta.transitions = now.transitions - base.transitions;
        delta.releasedHidden = now.releasedHidden - base.releasedHidden;
        delta.affinityBroken = now.affinityBroken - base.affinityBroken;
        delta.pinned = now.pinned - base.pinned;
        delta.pinsOutstanding = now.pinsOutstanding;
        return delta;
    }
};

// One lockstep frame: the plate draw, the present, then the batch's outputs.
void Frame(Scenario& s, unsigned& n, unsigned outputs = 3, bool withPlate = true)
{
    s.driver.Frame(n, withPlate);
    s.driver.Post();
    for (unsigned k = 0; k < outputs; ++k)
        s.driver.Output(n);
    ++n;
}

// Four warm-up frames: no slot yet, then three shadow frames (the pass proves itself with zero instances).
void WarmUp(Scenario& s, unsigned& n, unsigned outputs = 3)
{
    s.driver.Interp(PlateInterp::Endpoint);
    Frame(s, n, outputs); // declined: no slot yet
    Frame(s, n, outputs); // shadow 1
    Frame(s, n, outputs); // shadow 2
    Frame(s, n, outputs); // shadow 3: armed at the end of it
}

// Frames until one hides (at most `limit`). Returns the number of frames run.
int UntilHidden(Scenario& s, unsigned& n, unsigned outputs = 3, int limit = 10)
{
    for (int i = 0; i < limit; ++i)
    {
        Frame(s, n, outputs);
        if (s.driver.Mode(n - 1) == FrameMode::Hidden)
            return i + 1;
    }
    return limit;
}

void ExpectDrawn(const std::vector<OutputEntry>& outputs, size_t from, size_t to, unsigned n)
{
    for (size_t i = from; i < to; ++i)
        EXPECT(outputs[i].drawn == n, "output %zu drew frame %u, expected %u", i, outputs[i].drawn, n);
}

void ExpectNothing(const std::vector<OutputEntry>& outputs, size_t from, size_t to)
{
    for (size_t i = from; i < to; ++i)
        EXPECT(outputs[i].drawn == 0, "output %zu drew frame %u, expected nothing", i, outputs[i].drawn);
}

// The checks every scenario ends with.
void Common(const Scenario& s, const char* what)
{
    EXPECT(s.Now().pinsOutstanding == 0, "%s: %llu pins still held", what,
           static_cast<unsigned long long>(s.Now().pinsOutstanding));
    EXPECT(ext::hudless::plates::sync::Violations() == 0, "%s: lock violation: %s", what,
           ext::hudless::plates::sync::LastViolation().c_str());
    // L1: no output ever received the image of a frame the game drew natively.
    for (const auto& output : s.driver.Outputs())
    {
        if (output.drawn == 0)
            continue;
        bool native = true;
        for (const auto& frame : s.driver.Frames())
            if (frame.n == output.drawn)
                native = frame.mode != FrameMode::Hidden;
        EXPECT(!native, "%s: output %u drew frame %u although the game drew that frame natively", what, output.batch,
               output.drawn);
    }
}

// S1: steady state after the warm-up.
void SteadyState()
{
    Scenario s(3);
    s.driver.Interp(PlateInterp::Endpoint);
    unsigned n = 1;
    WarmUp(s, n);
    EXPECT(s.driver.Mode(1) == FrameMode::Declined, "frame 1 declined (no slot yet): %s", ModeName(s.driver.Mode(1)));
    for (unsigned f = 2; f <= 4; ++f)
    {
        EXPECT(s.driver.Mode(f) == FrameMode::Shadow, "frame %u shadow: %s", f, ModeName(s.driver.Mode(f)));
        EXPECT(s.driver.NativeDraws(f) == 1, "frame %u drew its plates natively", f);
    }
    ExpectNothing(s.driver.Outputs(), 0, 12);
    const size_t warmOutputs = s.driver.Outputs().size();
    const unsigned firstHidden = n;
    for (unsigned i = 0; i < 16; ++i)
        Frame(s, n);
    for (unsigned f = firstHidden; f < n; ++f)
    {
        EXPECT(s.driver.Mode(f) == FrameMode::Hidden, "frame %u hidden: %s", f, ModeName(s.driver.Mode(f)));
        EXPECT(s.driver.NativeDraws(f) == 0, "frame %u drew no plate natively", f);
    }
    // Every output of a hidden frame received that frame's image once.
    for (size_t i = warmOutputs; i + 3 <= s.driver.Outputs().size(); i += 3)
        ExpectDrawn(s.driver.Outputs(), i, i + 3, s.driver.Outputs()[i].batch);
    const Counters delta = s.Delta();
    EXPECT(delta.slotsCreated == 3, "three slots created: %llu", static_cast<unsigned long long>(delta.slotsCreated));
    EXPECT(delta.promoted == n - 1, "one record per present: %llu for %u frames",
           static_cast<unsigned long long>(delta.promoted), n - 1);
    EXPECT(delta.hidden == n - firstHidden, "hidden records: %llu", static_cast<unsigned long long>(delta.hidden));
    EXPECT(delta.shadow == 3, "shadow records: %llu", static_cast<unsigned long long>(delta.shadow));
    EXPECT(delta.native == 1, "native records: %llu", static_cast<unsigned long long>(delta.native));
    EXPECT(delta.hiddenSkipped == 0, "no hidden output was skipped: %llu",
           static_cast<unsigned long long>(delta.hiddenSkipped));
    EXPECT(delta.plated == (n - firstHidden) * 3, "every hidden output was plated: %llu",
           static_cast<unsigned long long>(delta.plated));
    Common(s, "S1");
    std::puts("    S1 steady state: declined/shadow/shadow/shadow, then every frame hidden, its image on every output");
}

// S2: the first armed frame, in detail.
void FirstArmedFrame()
{
    Scenario s(3);
    s.driver.Interp(PlateInterp::Endpoint);
    unsigned n = 1;
    s.driver.Frame(n);
    s.driver.Post();
    s.driver.Output(n);
    EXPECT(s.driver.Mode(1) == FrameMode::Declined, "the first frame has no slot yet");
    ++n;
    for (unsigned f = 2; f <= 4; ++f)
    {
        const size_t before = s.driver.Outputs().size();
        s.driver.Frame(n);
        s.driver.Post();
        s.driver.Output(n);
        EXPECT(s.driver.Mode(f) == FrameMode::Shadow, "frame %u shadow: %s", f, ModeName(s.driver.Mode(f)));
        EXPECT(s.driver.NativeDraws(f) == 1, "a shadow frame draws its plates natively");
        ExpectNothing(s.driver.Outputs(), before, s.driver.Outputs().size());
        ++n;
    }
    const Counters before = s.Now();
    s.driver.Frame(n);
    s.driver.Post();
    const size_t outputs = s.driver.Outputs().size();
    s.driver.Output(n);
    EXPECT(s.driver.Mode(n) == FrameMode::Hidden, "the frame after three shadow promotes is hidden: %s",
           ModeName(s.driver.Mode(n)));
    EXPECT(s.driver.NativeDraws(n) == 0, "a hidden frame draws no plate natively");
    ExpectDrawn(s.driver.Outputs(), outputs, s.driver.Outputs().size(), n);
    const Counters after = s.Now();
    EXPECT(after.hidden - before.hidden == 1, "one hidden record promoted");
    EXPECT(after.transitions - before.transitions == 1, "the shadow to hidden transition is counted");
    ++n;
    Common(s, "S2");
    std::puts("    S2 first armed frame: three shadow frames prove the pass, the fourth frame hides and its image is drawn");
}

// S3: one output skipped (the backbuffer cannot be fetched).
void PassSkip()
{
    Scenario s(3);
    unsigned n = 1;
    WarmUp(s, n);
    Frame(s, n); // hidden
    EXPECT(s.driver.Mode(n - 1) == FrameMode::Hidden, "the frame before the skip is hidden");
    Frame(s, n, 2); // hidden frame: its third output will fail
    const unsigned skippedBatch = n - 1;
    const Counters beforeSkip = s.Now();
    s.driver.FailNextOutput();
    const auto skipped = s.driver.Output(skippedBatch);
    EXPECT(skipped.drawn == 0, "the skipped output drew nothing");
    EXPECT(s.Now().hiddenSkipped - beforeSkip.hiddenSkipped == 1, "the skip of a hidden record is counted");
    EXPECT(s.Now().pinsOutstanding == 0, "the skipped output dropped its pin");
    // The pass proved nothing on that output, so the next frame is not hidden: it is a shadow frame again.
    const size_t beforeNext = s.driver.Outputs().size();
    s.driver.Frame(n);
    s.driver.Post();
    s.driver.Output(n);
    EXPECT(s.driver.Mode(n) == FrameMode::Shadow, "the frame after a skip is shadow: %s", ModeName(s.driver.Mode(n)));
    EXPECT(s.driver.NativeDraws(n) == 1, "and the game drew its plates natively");
    ExpectNothing(s.driver.Outputs(), beforeNext, s.driver.Outputs().size());
    ++n;
    // One shadow frame is enough to prove the path again.
    Frame(s, n);
    EXPECT(s.driver.Mode(n - 1) == FrameMode::Hidden, "the frame after the shadow frame hides again: %s",
           ModeName(s.driver.Mode(n - 1)));
    Frame(s, n);
    Common(s, "S3");
    std::puts("    S3 skip: one output loses the plates, the next frame is shadow again, then it re-arms");
}

// S5: release with a hidden frame open.
void ReleaseWithOpenFrame()
{
    Scenario s(3);
    unsigned n = 1;
    WarmUp(s, n);
    Frame(s, n); // hidden
    EXPECT(s.driver.Mode(n - 1) == FrameMode::Hidden, "hidden before the release");
    s.driver.Frame(n);
    EXPECT(s.driver.Mode(n) == FrameMode::Hidden, "the open frame reserved a hidden slot");
    const Counters before = s.Now();
    s.driver.Release();
    const Counters after = s.Now();
    EXPECT(after.releasedHidden - before.releasedHidden == 1, "the open hidden frame is counted as released");
    EXPECT(after.pinsOutstanding == 0, "no pin survives a release");
    ++n;
    // Nothing is armed after a release: the next frame is declined, then the slots are created again and it re-arms.
    const size_t beforeOutputs = s.driver.Outputs().size();
    s.driver.Frame(n);
    EXPECT(s.driver.Mode(n) == FrameMode::Declined, "the frame after a release is declined: %s",
           ModeName(s.driver.Mode(n)));
    s.driver.Post();
    s.driver.Output(n);
    ExpectNothing(s.driver.Outputs(), beforeOutputs, s.driver.Outputs().size());
    ++n;
    for (int i = 0; i < 8; ++i)
        Frame(s, n);
    EXPECT(s.driver.Mode(n - 1) == FrameMode::Hidden, "the engine re-arms after the release: %s",
           ModeName(s.driver.Mode(n - 1)));
    Common(s, "S5");
    std::puts("    S5 release: the open hidden frame is dropped and counted, the next frame is declined, then it re-arms");
}

// S6: the bridge has no device for one present. A second WARP device cannot be created (the runtime returns the same
// device object), so the device-change rebind itself is what every scenario's first present does: bind a device, retire
// what an older one left, create the produce fence. Here the bridge reports no device at all: the frame that was open is
// dropped with its reservation, so the next frame neither inherits its "hidden" answer nor finds its slot refused.
void DeviceMissing()
{
    Scenario s(3);
    unsigned n = 1;
    WarmUp(s, n);
    Frame(s, n); // hidden
    EXPECT(s.driver.Mode(n - 1) == FrameMode::Hidden, "hidden before the bridge loses its device");
    s.driver.Frame(n); // a hidden slot is reserved
    EXPECT(s.driver.Mode(n) == FrameMode::Hidden, "the open frame reserved a hidden slot");
    const size_t beforeOutputs = s.driver.Outputs().size();
    const Counters beforeDrop = s.Now();
    // The bridge's device is gone for this present: the frame cannot be published and its reservation ends here.
    s.driver.NoDevice();
    s.driver.Post();
    const Counters afterDrop = s.Now();
    EXPECT(afterDrop.releasedHidden - beforeDrop.releasedHidden == 1, "the dropped hidden frame is counted");
    EXPECT(!s.driver.EngineHidden(), "the dropped frame's hidden answer is not current any more");
    // The frame whose plate draws were hidden lost its image; the previous hidden record may keep drawing one frame.
    const auto stale = s.driver.Output(n);
    EXPECT(stale.drawn == n - 1, "the previous hidden record keeps drawing (one frame stale): %u", stale.drawn);
    // The next frame is a shadow frame of its own: before the fix the stale reservation refused it (declined) while the
    // engine still answered "hidden" for it.
    s.driver.WithDevice();
    ++n;
    s.driver.Frame(n);
    EXPECT(s.driver.Mode(n) == FrameMode::Shadow, "the frame after the dropped one reserves its own slot: %s",
           ModeName(s.driver.Mode(n)));
    EXPECT(s.driver.EngineHidden() == (s.driver.Mode(n) == FrameMode::Hidden),
           "the engine's hidden answer is this frame's");
    s.driver.Post();
    ++n;
    const int frames = UntilHidden(s, n);
    EXPECT(s.driver.Mode(n - 1) == FrameMode::Hidden, "the engine re-arms after the device came back (%d frames)",
           frames);
    EXPECT(s.driver.Outputs().size() > beforeOutputs, "outputs continued");
    Common(s, "S6");
    std::puts("    S6 device missing: the open frame is dropped with its slot, the previous hidden record draws one more "
              "frame, and the next frame reserves its own slot again");
}

// S6b: a present that never reaches the frame finish at all (the bridge gave up before the interop copy). The frame's
// reservation must end at that present, not be lent to the next frame.
void DroppedPresent()
{
    Scenario s(3);
    unsigned n = 1;
    WarmUp(s, n);
    Frame(s, n); // hidden
    const unsigned openFrame = n;
    s.driver.Frame(n); // the frame that will be dropped reserves a hidden slot
    EXPECT(s.driver.Mode(openFrame) == FrameMode::Hidden, "the frame reserved a hidden slot: %s",
           ModeName(s.driver.Mode(openFrame)));
    EXPECT(s.driver.EngineHidden(), "and the engine answered hidden for it");
    const Counters before = s.Now();
    s.driver.DropPresent(); // the present started and ended without the frame finish
    EXPECT(!s.driver.EngineHidden(), "the dropped frame's answer is not current after its present");
    ++n;
    s.driver.Frame(n);
    EXPECT(s.driver.Mode(n) != FrameMode::Declined, "the next frame reserves its own slot, not a refused one: %s",
           ModeName(s.driver.Mode(n)));
    EXPECT(s.driver.EngineHidden() == (s.driver.Mode(n) == FrameMode::Hidden),
           "and the engine's answer belongs to this frame");
    const Counters after = s.Now();
    EXPECT(after.reclaimed - before.reclaimed == 1, "the dropped reservation is taken back once: %llu",
           static_cast<unsigned long long>(after.reclaimed - before.reclaimed));
    EXPECT(after.releasedHidden - before.releasedHidden == 1, "and counted as a hidden frame lost");
    s.driver.Post();
    const size_t beforeOutputs = s.driver.Outputs().size();
    s.driver.Output(n);
    for (size_t i = beforeOutputs; i < s.driver.Outputs().size(); ++i)
        EXPECT(s.driver.Outputs()[i].drawn != openFrame, "the dropped frame's image is never drawn");
    ++n;
    // The engine keeps working afterwards: it does not wedge on the dropped frame.
    const int frames = UntilHidden(s, n);
    EXPECT(s.driver.Mode(n - 1) == FrameMode::Hidden, "the engine keeps hiding after a dropped present (%d frames)",
           frames);
    Common(s, "S6b");
    std::puts("    S6b dropped present: the reservation ends with its present, the next frame gets its own slot, and the "
              "dropped image is never drawn");
}

// S7: key off mid-frame, the drain, and the menu write landing between the engine's clear and its re-read.
void KeyToggles()
{
    Scenario s(3);
    unsigned n = 1;
    WarmUp(s, n);
    Frame(s, n); // hidden
    const unsigned openFrame = n;
    s.driver.Frame(n);
    EXPECT(s.driver.Mode(openFrame) == FrameMode::Hidden, "the frame reserved a hidden slot");
    s.driver.Interp(PlateInterp::Off); // the user turns the key off while the frame is being drawn
    s.driver.Post();
    const size_t afterPost = s.driver.Outputs().size();
    s.driver.Output(openFrame);
    ExpectDrawn(s.driver.Outputs(), afterPost, s.driver.Outputs().size(), openFrame); // sticky: the frame is drawn
    EXPECT(BitCalls().empty(), "the last hidden record is still drawable: the bit stays");
    ++n;
    s.driver.Frame(n);
    EXPECT(s.driver.Mode(n) == FrameMode::Declined, "the next frame is declined: %s", ModeName(s.driver.Mode(n)));
    const size_t beforeNative = s.driver.Outputs().size();
    s.driver.Post(); // publishes a native record: the last hidden record is gone
    EXPECT(BitCalls().size() == 1 && BitCalls().back() == false, "the drain clears the feature bit once");
    s.driver.Output(n);
    ExpectNothing(s.driver.Outputs(), beforeNative, s.driver.Outputs().size());
    ++n;

    // The interleaving: the user turns the key back on at the instant the engine clears the bit. The engine re-reads
    // the snapshot after clearing, so the final state is the snapshot's (on) and not a bit left off by the drain.
    BitCalls().clear();
    s.driver.Interp(PlateInterp::Endpoint);
    for (int i = 0; i < 6; ++i)
        Frame(s, n);
    EXPECT(s.driver.Mode(n - 1) == FrameMode::Hidden, "the engine re-arms after the key comes back: %s",
           ModeName(s.driver.Mode(n - 1)));
    BitCalls().clear();
    s.driver.Interp(PlateInterp::Off);
    BitHook = [](bool on)
    {
        if (!on)
            ext::hudless::PublishPlateSettings({ PlateInterp::Endpoint, 1, 24 }); // the menu publishes at once
    };
    s.driver.Frame(n);
    s.driver.Post();
    ++n;
    BitHook = nullptr;
    EXPECT(BitCalls().size() == 2 && BitCalls()[0] == false && BitCalls()[1] == true,
           "the drain cleared the bit and set it again because the key came back (%zu calls)", BitCalls().size());
    EXPECT(ext::hudless::CurrentPlates().interp == PlateInterp::Endpoint, "the snapshot is on");
    // The engine serves frames again on the key that came back.
    for (int i = 0; i < 6; ++i)
        Frame(s, n);
    EXPECT(s.driver.Mode(n - 1) == FrameMode::Hidden, "hidden again after the interleaved key change");
    Common(s, "S7");
    std::puts("    S7 key toggles: the open frame is drawn, the next frame is declined, the drain clears the bit once and "
              "re-reads the snapshot");
}

// S8: multiplier 1 (no capture).
void MultiplierOne()
{
    Scenario s(3);
    unsigned n = 1;
    WarmUp(s, n);
    Frame(s, n); // hidden
    s.driver.Multiplier(1);
    Frame(s, n); // the present after it turns the capture off
    const Counters before = s.Now();
    const size_t beforeOutputs = s.driver.Outputs().size();
    s.driver.Frame(n);
    EXPECT(s.driver.Mode(n) == FrameMode::Declined, "multiplier 1 declines the frame: %s", ModeName(s.driver.Mode(n)));
    EXPECT(s.driver.NativeDraws(n) == 1, "and the game draws its plates natively");
    s.driver.Post();
    s.driver.Output(n);
    const Counters after = s.Now();
    EXPECT(after.declined - before.declined == 1, "the refusal is counted");
    EXPECT(after.slotsCreated == before.slotsCreated, "no slot is created for a declined frame");
    EXPECT(after.plated == before.plated, "the pass draws nothing");
    ExpectNothing(s.driver.Outputs(), beforeOutputs, s.driver.Outputs().size());
    ++n;
    Frame(s, n);
    Common(s, "S8");
    std::puts("    S8 multiplier 1: declined, native plates only, no slot, no pass draw");
}

// S9: multiplier 2 arms and hides, and endpoint never shifts.
void MultiplierTwo()
{
    Scenario s(2);
    unsigned n = 1;
    WarmUp(s, n, 2);
    EXPECT(s.driver.Mode(2) == FrameMode::Shadow, "multiplier 2 shadow frame");
    Frame(s, n, 2);
    EXPECT(s.driver.Mode(n - 1) == FrameMode::Hidden, "multiplier 2 hides after the warm-up: %s",
           ModeName(s.driver.Mode(n - 1)));
    for (unsigned i = 0; i < 4; ++i)
        Frame(s, n, 2);
    EXPECT(s.driver.Mode(n - 1) == FrameMode::Hidden, "and keeps hiding");
    EXPECT(!ext::hudless::plates::ShiftingActive(), "endpoint never shifts");
    Common(s, "S9");
    std::puts("    S9 multiplier 2: arms after three shadow frames, hides, endpoint never shifts");
}

// S11: the preset takes the recomposition away mid-run.
void PresetPause()
{
    Scenario s(3);
    ext::hudless::TestMode() = ext::hudless::Mode::Recomposition;
    unsigned n = 1;
    WarmUp(s, n);
    const unsigned openFrame = n;
    s.driver.Frame(n);
    EXPECT(s.driver.Mode(openFrame) == FrameMode::Hidden, "the frame reserved a hidden slot");
    // The preset drops the recomposition before the present.
    ext::fg::preset::TestDecision().droppedByPreset = true;
    const size_t beforePost = s.driver.Outputs().size();
    s.driver.Post();
    s.driver.Output(openFrame);
    ExpectDrawn(s.driver.Outputs(), beforePost, s.driver.Outputs().size(), openFrame); // drawn unshifted
    EXPECT(!ext::hudless::plates::ShiftingActive(), "the unarmed record is drawn unshifted");
    ++n;
    s.driver.Frame(n);
    EXPECT(s.driver.Mode(n) == FrameMode::Declined, "the gate closes at the next frame: %s", ModeName(s.driver.Mode(n)));
    s.driver.Post();
    ++n;
    Frame(s, n);
    Common(s, "S11");
    std::puts("    S11 pause: the hidden frame is drawn unshifted, then the gate closes and the plates stay native");
}

// S12: the output size changes. One slot is remade per present, so the first frame is declined and the following ones
// use the slot that is already the new size.
void Resize()
{
    Scenario s(3);
    unsigned n = 1;
    WarmUp(s, n);
    Frame(s, n); // hidden at 64x32
    EXPECT(s.driver.Mode(n - 1) == FrameMode::Hidden, "hidden before the resize");
    s.driver.Resize(80, 40);
    const Counters before = s.Now();
    unsigned created = 0;
    unsigned declined = 0;
    for (int i = 0; i < 5; ++i)
    {
        const Counters frameBase = s.Now();
        const size_t beforeOutputs = s.driver.Outputs().size();
        s.driver.Frame(n);
        s.driver.Post();
        s.driver.Output(n);
        const Counters frameNow = s.Now();
        EXPECT(frameNow.slotsCreated - frameBase.slotsCreated <= 1, "at most one slot per present");
        created += static_cast<unsigned>(frameNow.slotsCreated - frameBase.slotsCreated);
        declined += s.driver.Mode(n) == FrameMode::Declined;
        if (i < 3)
            EXPECT(s.driver.Mode(n) != FrameMode::Hidden, "no frame hides before the slots are remade: %s",
                   ModeName(s.driver.Mode(n)));
        if (i == 0)
            ExpectNothing(s.driver.Outputs(), beforeOutputs, s.driver.Outputs().size()); // no stale image
        ++n;
    }
    EXPECT(s.driver.Mode(n - 5) == FrameMode::Declined, "the first frame after the resize is declined");
    EXPECT(created == 3, "the three slots were remade for the new size: %u", created);
    EXPECT(declined >= 1, "at least one frame was declined while the slots were remade");
    EXPECT(s.Now().slotsCreated - before.slotsCreated == 3, "exactly three creations");
    const int frames = UntilHidden(s, n);
    EXPECT(s.driver.Mode(n - 1) == FrameMode::Hidden, "hidden again at the new size (%d frames): %s", frames,
           ModeName(s.driver.Mode(n - 1)));
    EXPECT(s.driver.Outputs().back().drawn == n - 1, "the new size's outputs draw the hidden frame");
    Common(s, "S12");
    std::puts("    S12 resize: frames declined or shadow while one slot per present is remade, no stale image, then it "
              "hides again");
}

// S13: a reservation the engine refuses although the gate is open; the older hidden record must not be drawn.
void RefusedReservation()
{
    Scenario s(3);
    unsigned n = 1;
    WarmUp(s, n);
    const unsigned hiddenFrame = n;
    Frame(s, n); // hidden at 64x32
    EXPECT(s.driver.Mode(hiddenFrame) == FrameMode::Hidden, "hidden before the refusal");
    // The next frame arrives with a size no slot has yet: reserve before hide refuses it although the gate is open.
    s.driver.Resize(80, 40);
    const size_t beforeOutputs = s.driver.Outputs().size();
    s.driver.Frame(n);
    EXPECT(s.driver.Mode(n) == FrameMode::Declined, "the reservation is refused: %s", ModeName(s.driver.Mode(n)));
    EXPECT(s.driver.NativeDraws(n) == 1, "the frame draws its plates natively");
    s.driver.Post();
    // The native record replaces the hidden one: the outputs of this frame must not draw the older hidden image.
    s.driver.Output(n);
    ExpectNothing(s.driver.Outputs(), beforeOutputs, s.driver.Outputs().size());
    ++n;
    Common(s, "S13");
    std::puts("    S13 refused reservation: declined and native, the older hidden record is not drawn over the plates");
}

// S14: the produce signal fails on a hidden frame.
void ProduceFailure()
{
    Scenario s(3);
    unsigned n = 1;
    WarmUp(s, n);
    Frame(s, n);
    const unsigned hiddenFrame = n - 1;
    EXPECT(s.driver.Mode(hiddenFrame) == FrameMode::Hidden, "hidden before the failure");
    const Counters before = s.Now();
    s.driver.Frame(n);
    EXPECT(s.driver.Mode(n) == FrameMode::Hidden, "the frame reserved a hidden slot: %s", ModeName(s.driver.Mode(n)));
    s.driver.QueueMissing();
    const size_t beforePost = s.driver.Outputs().size();
    s.driver.Post();
    const Counters after = s.Now();
    EXPECT(after.promoteFailedHidden - before.promoteFailedHidden == 1, "the failed promote is counted");
    EXPECT(after.pinsOutstanding == 0, "no pin leaks");
    // The drawable record is unchanged: this frame's outputs still draw the previous hidden frame (one frame stale).
    s.driver.Output(hiddenFrame);
    ExpectDrawn(s.driver.Outputs(), beforePost, s.driver.Outputs().size(), hiddenFrame);
    ++n;
    // The gate is closed and the produce path unproven: the next frame is a shadow frame, native.
    s.driver.Frame(n);
    s.driver.Post();
    EXPECT(s.driver.Mode(n) == FrameMode::Shadow, "the frame after the failure is shadow: %s", ModeName(s.driver.Mode(n)));
    EXPECT(s.driver.NativeDraws(n) == 1, "and native");
    ++n;
    const int frames = UntilHidden(s, n);
    EXPECT(s.driver.Mode(n - 1) == FrameMode::Hidden, "the engine re-arms after the failure (%d frames): %s", frames,
           ModeName(s.driver.Mode(n - 1)));
    // The failed promote restarted the stable counter: three shadow frames again, not one.
    EXPECT(frames == 3, "the stable counter restarted after the failure (%d frames, expected 3)", frames);
    Common(s, "S14");
    std::puts("    S14 produce failure: slot freed, the previous hidden record draws one more frame, the gate closes, "
              "then it re-arms");
}

// S15: the pass pins what it reads, and only a free slot is written.
void PassPin()
{
    Scenario s(3);
    unsigned n = 1;
    WarmUp(s, n);
    const unsigned frame = n;
    s.driver.Frame(n);
    s.driver.Post();
    const Counters before = s.Now();
    const auto output = s.driver.Output(frame);
    const Counters after = s.Now();
    EXPECT(output.drawn == frame, "the output drew the frame's own image");
    EXPECT(after.pinned - before.pinned == 1, "one pin taken for the read");
    EXPECT(after.plated - before.plated == 1, "one plated output");
    EXPECT(after.pinsOutstanding == 0, "and the pin is dropped when the output returned");
    ++n;
    // The slot that output read is not handed out again while its record is drawable: the next frame takes another.
    const int previousSlot = s.driver.Frames().back().slot;
    s.driver.Frame(n);
    EXPECT(s.driver.Mode(n) == FrameMode::Hidden, "the next frame hides");
    EXPECT(s.driver.Frames().back().slot != previousSlot, "a different slot: the drawn one is not reserved");
    s.driver.Post();
    s.driver.Output(n);
    ++n;
    EXPECT(s.Now().pinsOutstanding == 0, "no pin survives the scenario");
    Common(s, "S15");
    std::puts("    S15 pins: the pass pins the slot it reads in the copy section, drops it on every exit, and the drawn "
              "slot is not reserved again");
}

// S16: the frame finish runs on another thread than the draws.
void ThreadAffinity()
{
    Scenario s(3);
    unsigned n = 1;
    WarmUp(s, n);
    Frame(s, n); // hidden, on this thread
    s.driver.Frame(n);
    EXPECT(s.driver.Mode(n) == FrameMode::Hidden, "the frame reserved before the thread change");
    const Counters before = s.Now();
    std::thread finish([&]() { s.driver.Post(); });
    finish.join();
    const Counters after = s.Now();
    EXPECT(after.affinityBroken - before.affinityBroken == 1, "the thread break is counted");
    EXPECT(CountLog("Plates: the present ran on another thread") + CountLog("Plates: the frame finish ran on another thread") == 1,
           "and logged once (%d %d)", CountLog("Plates: the present ran on another thread"),
           CountLog("Plates: the frame finish ran on another thread"));
    ++n;
    // Fail open for good: no frame is hidden afterwards.
    for (int i = 0; i < 6; ++i)
    {
        s.driver.Frame(n);
        EXPECT(s.driver.Mode(n) != FrameMode::Hidden, "no frame hides after the thread break: %s",
               ModeName(s.driver.Mode(n)));
        s.driver.Post();
        s.driver.Output(n);
        ++n;
    }
    Common(s, "S16");
    std::puts("    S16 thread affinity: a frame finish on another thread latches the engine off, logged once, no hide "
              "afterwards");
}

// S19: one batch of lag. A hidden frame followed by a declined one: a late output of the hidden batch draws the
// newest record (nothing), so the loss is exactly that output.
void OneBatchLag()
{
    Scenario s(3);
    unsigned n = 1;
    WarmUp(s, n);
    Frame(s, n); // hidden
    const unsigned hiddenFrame = n - 1;
    EXPECT(s.driver.Mode(hiddenFrame) == FrameMode::Hidden, "hidden before the transition");
    // The next frame is declined: the key goes off while the hidden frame's last output is still to come.
    const unsigned declinedFrame = n;
    s.driver.Interp(PlateInterp::Off);
    s.driver.Frame(declinedFrame, true);
    s.driver.Post(); // a second present is not modelled: the frame above already posted
    EXPECT(s.driver.Mode(declinedFrame) == FrameMode::Declined, "the next frame is declined");
    // The hidden batch's missing outputs present after the present of the declined frame (late).
    const size_t beforeLate = s.driver.Outputs().size();
    s.driver.Output(hiddenFrame);
    const auto last = s.driver.Outputs().back();
    EXPECT(last.drawn == 0, "the late output drew the newest record (nothing): %u", last.drawn);
    s.driver.Output(declinedFrame);
    ExpectNothing(s.driver.Outputs(), beforeLate, s.driver.Outputs().size());
    // No stale hidden image after the transition, and the next hidden batch is clean.
    ++n;
    s.driver.Interp(PlateInterp::Endpoint);
    for (int i = 0; i < 8; ++i)
        Frame(s, n);
    EXPECT(s.driver.Mode(n - 1) == FrameMode::Hidden, "the engine re-arms later");
    const size_t lastBatch = s.driver.Outputs().size();
    Frame(s, n);
    ExpectDrawn(s.driver.Outputs(), lastBatch, s.driver.Outputs().size(), n - 1);

    // The mirror case: a native frame then a shadow frame (the pass skips the native record and proves itself again),
    // so a late output of the native batch draws the shadow record: nothing, never a doubled image.
    const unsigned nativeFrame = n;
    s.driver.Frame(nativeFrame, false); // no plate draw at all
    EXPECT(s.driver.Mode(nativeFrame) == FrameMode::Declined, "a frame with no plate draw reserves nothing");
    s.driver.Post();
    const size_t afterNative = s.driver.Outputs().size();
    s.driver.Output(nativeFrame); // the native record's own output: nothing
    ++n;
    s.driver.Frame(n);
    EXPECT(s.driver.Mode(n) == FrameMode::Shadow, "the frame after a native record is a shadow frame: %s",
           ModeName(s.driver.Mode(n)));
    s.driver.Post();
    s.driver.Output(nativeFrame); // a late output of the native batch, after the shadow present
    ExpectNothing(s.driver.Outputs(), afterNative, s.driver.Outputs().size());
    ++n;
    Common(s, "S19");
    std::puts("    S19 one-batch lag: the late output draws the newest record, so the loss is exactly that output and the "
              "next batch is clean; the mirror case cannot double");
}

// S20: early outputs: outputs of the next batch present before its own present.
void EarlyOutputs()
{
    Scenario s(3);
    unsigned n = 1;
    WarmUp(s, n);
    Frame(s, n, 2); // hidden, only two of its three outputs
    const unsigned frame = n - 1;
    EXPECT(s.driver.Mode(frame) == FrameMode::Hidden, "hidden before the early outputs");
    // Two outputs of the *next* batch present before its present: they draw the newest record, this frame's image.
    const Counters before = s.Now();
    const size_t base = s.driver.Outputs().size();
    s.driver.Output(n);               // k = 2 of the open batch: still inside the multiplier
    s.driver.Output(n);               // k = 3: beyond it, and the record did not change
    ExpectDrawn(s.driver.Outputs(), base, s.driver.Outputs().size(), frame);
    const Counters after = s.Now();
    EXPECT(after.earlyOutputs - before.earlyOutputs == 1,
           "the output beyond the multiplier is counted as an early output (%llu)",
           static_cast<unsigned long long>(after.earlyOutputs - before.earlyOutputs));
    EXPECT(after.lateOutputs == before.lateOutputs,
           "a late output that only shifts the batch count is not observable to the phase model");
    // The next batch proceeds normally.
    const size_t next = s.driver.Outputs().size();
    Frame(s, n);
    ExpectDrawn(s.driver.Outputs(), next, s.driver.Outputs().size(), n - 1);
    Common(s, "S20");
    std::puts("    S20 early outputs: outputs of the next batch draw the newest record, and an output beyond the "
              "multiplier is counted");
}

// S21: a hidden frame whose image received no draw. Its slot still holds an older frame's image, so the record must
// publish Native (nothing) and never draw the whole recycled slot.
void EmptyHiddenFrame()
{
    Scenario s(3);
    unsigned n = 1;
    WarmUp(s, n);
    Frame(s, n); // hidden, its image in a slot
    const unsigned imaged = n - 1;
    EXPECT(s.driver.Mode(imaged) == FrameMode::Hidden, "hidden before the empty frame");
    const Counters before = s.Now();
    const size_t beforeOutputs = s.driver.Outputs().size();
    s.driver.FrameNoDraw(n); // reserves a hidden slot, then draws nothing into it
    EXPECT(s.driver.Mode(n) == FrameMode::Hidden, "the frame still reserved a hidden slot: %s",
           ModeName(s.driver.Mode(n)));
    s.driver.Post();
    const Counters after = s.Now();
    EXPECT(after.empty - before.empty == 1, "the hidden frame with no draw is counted: %llu",
           static_cast<unsigned long long>(after.empty - before.empty));
    EXPECT(after.native - before.native == 1, "and it published a native record");
    s.driver.Output(n);
    ExpectNothing(s.driver.Outputs(), beforeOutputs, s.driver.Outputs().size());
    // The restarted gate: a native record proves nothing about the pass, so the next frame is drawn natively once.
    ++n;
    s.driver.Frame(n);
    EXPECT(s.driver.Mode(n) == FrameMode::Shadow, "the frame after the empty image is a shadow frame: %s",
           ModeName(s.driver.Mode(n)));
    EXPECT(s.driver.NativeDraws(n) == 1, "and the game drew its plates");
    s.driver.Post();
    ++n;
    const int frames = UntilHidden(s, n);
    EXPECT(s.driver.Mode(n - 1) == FrameMode::Hidden, "the engine hides again (%d frames)", frames);
    EXPECT(s.driver.Outputs().back().drawn == n - 1, "and its image is drawn");
    Common(s, "S21");
    std::puts("    S21 empty hidden frame: nothing was drawn into the slot, so the record is native and the recycled "
              "slot is never drawn");
}

// S22: the two freshness windows at the values the product uses, on the test's own clock.
void FreshnessWindows()
{
    Scenario s(3);
    unsigned n = 1;
    WarmUp(s, n);
    Frame(s, n); // hidden
    const unsigned hiddenFrame = n - 1;
    EXPECT(s.driver.Mode(hiddenFrame) == FrameMode::Hidden, "hidden before the windows");

    // (a) the alive window (100 ms): the pass proved itself on the frame's last output.
    plate_test_clock::Advance(60);
    EXPECT(ext::hudless::plates::HideAllowed(), "the gate is still open 60 ms after the last output");
    plate_test_clock::Advance(60);
    EXPECT(!ext::hudless::plates::HideAllowed(), "the gate is closed 120 ms after the last output");
    const size_t beforeAlive = s.driver.Outputs().size();
    s.driver.Frame(n);
    EXPECT(s.driver.Mode(n) == FrameMode::Shadow, "a dead pass means a shadow frame: %s", ModeName(s.driver.Mode(n)));
    EXPECT(s.driver.NativeDraws(n) == 1, "and the game draws its plates");
    s.driver.Post();
    s.driver.Output(n);
    ExpectNothing(s.driver.Outputs(), beforeAlive, s.driver.Outputs().size());
    ++n;

    // (b) the record window (250 ms): a hidden record nobody replaced is not drawn any more.
    const int frames = UntilHidden(s, n);
    EXPECT(s.driver.Mode(n - 1) == FrameMode::Hidden, "hidden again (%d frames)", frames);
    EXPECT(ext::hudless::plates::HideAllowed(), "and the gate is open right after a plated output");
    plate_test_clock::Advance(300);
    const Counters beforeStale = s.Now();
    const size_t beforeStaleOutput = s.driver.Outputs().size();
    s.driver.Output(n - 1);
    ExpectNothing(s.driver.Outputs(), beforeStaleOutput, s.driver.Outputs().size());
    EXPECT(s.driver.LastSkip() == "no fresh plate record", "the pass names the stale record: %s",
           s.driver.LastSkip().c_str());
    EXPECT(s.Now().hiddenSkipped - beforeStale.hiddenSkipped == 1, "the stale hidden record is counted as skipped");
    EXPECT(!ext::hudless::plates::HideAllowed(), "and the gate is closed for the next frame");

    // (c) a clock that reads before the record was published (another thread's sample, or a clock that went back) must
    // fail open, not draw the record for the rest of the session.
    s.driver.Frame(n);
    s.driver.Post();
    ++n;
    const int frames2 = UntilHidden(s, n);
    EXPECT(s.driver.Mode(n - 1) == FrameMode::Hidden, "hidden again before the backwards clock (%d frames)", frames2);
    const size_t beforeBack = s.driver.Outputs().size();
    plate_test_clock::Set(plate_test_clock::Now() - 1000);
    s.driver.Output(n - 1);
    ExpectNothing(s.driver.Outputs(), beforeBack, s.driver.Outputs().size());
    EXPECT(!ext::hudless::plates::HideAllowed(), "a record from the future does not open the gate");

    s.driver.Frame(n);
    s.driver.Post();
    ++n;
    Common(s, "S22");
    std::puts("    S22 freshness: 100 ms of no pass closes the hide gate, 250 ms makes the record stale, and a tick "
              "from the future fails open");
}

// S23: the pass holds a pin on the slot it reads while the game side runs, and the presenting queue cannot complete.
// One slot is pinned by the pass, the frames that follow take the other two until none is free: a frame is then declined
// and publishes a native record, and the pinned read still shows its own frame's image.
void PinAcrossReservation()
{
    Scenario s(3);
    unsigned n = 1;
    WarmUp(s, n);
    Frame(s, n); // hidden
    const unsigned pinnedFrame = n - 1;
    EXPECT(s.driver.Mode(pinnedFrame) == FrameMode::Hidden, "hidden before the pin");
    const int pinnedSlot = s.driver.Frames().back().slot;

    PauseGate gate("pass.pinned");
    const size_t deferredIndex = s.driver.Outputs().size();
    std::thread pass(
        [&]()
        {
            // A stand-in that throws would end the process from a worker thread: report it as a failed check instead.
            try
            {
                s.driver.OutputDeferred(pinnedFrame);
            }
            catch (const std::exception& error)
            {
                std::printf("    the presenting thread failed: %s\n", error.what());
                ++g_failures;
            }
        });
    const bool reached = gate.WaitReached();
    EXPECT(reached, "the pass reached its pinned point");
    // The game side keeps running while the pass holds the slot it copied the record for. No output runs here: the pass
    // mutex is held by the paused pass, and a second output would wait for it (a real pacer does serialise them).
    std::vector<int> used;
    bool anyDeclined = false;
    const Counters beforeGame = s.Now();
    for (int i = 0; i < 4; ++i)
    {
        s.driver.Frame(n);
        const auto& entry = s.driver.Frames().back();
        anyDeclined = anyDeclined || entry.mode == FrameMode::Declined;
        used.push_back(entry.slot);
        s.driver.Post();
        ++n;
    }
    EXPECT(std::find(used.begin(), used.end(), pinnedSlot) == used.end(),
           "no frame reserved the slot the pass had pinned (slot %d)", pinnedSlot);
    EXPECT(anyDeclined, "with the pinned slot out of reach a frame was declined rather than reusing it");
    EXPECT(s.Now().native - beforeGame.native >= 1, "the declined frame published a native record");
    gate.Release();
    pass.join();
    EXPECT(gate.Passed(), "the pause was taken at the pin");
    EXPECT(s.Now().pinsOutstanding == 0, "the pin came back");
    s.driver.FinishDeferred();
    EXPECT(s.driver.Outputs()[deferredIndex].drawn == pinnedFrame,
           "the pinned read still shows its own frame's image: %u", s.driver.Outputs()[deferredIndex].drawn);
    // And the engine re-arms afterwards.
    const int frames = UntilHidden(s, n);
    EXPECT(s.driver.Mode(n - 1) == FrameMode::Hidden, "the engine hides again after the stall (%d frames)", frames);
    Common(s, "S23");
    std::puts("    S23 pin across a reservation: the pass keeps its slot, the game side never writes it, the frame the"
              " slot cannot serve is declined, and the pinned read is still its own image");
}

// S24: the pass's GPU work has not completed. The presenting queue is behind a fence the test signals later, so every
// slot a submitted pass read is still busy: frames then find no free slot, decline, and no older hidden image is drawn
// over their native plates.
void GpuLag()
{
    Scenario s(3);
    unsigned n = 1;
    WarmUp(s, n);
    const Counters before = s.Now();
    // Three hidden frames, each with one output whose GPU work cannot complete: one slot each.
    for (int i = 0; i < 3; ++i)
    {
            Frame(s, n, 0); // hidden (the warm-up armed the gate), its output deferred below
            EXPECT(s.driver.Mode(n - 1) == FrameMode::Hidden, "frame %u hidden: %s", n - 1,
               ModeName(s.driver.Mode(n - 1)));
            s.driver.OutputDeferred(n - 1);
            ++n;
    }
    // Every slot now has a reader that has not completed: the next frame cannot reserve one and stays native.
    s.driver.Frame(n);
    EXPECT(s.driver.Mode(n) == FrameMode::Declined, "with every slot still being read the frame is declined: %s",
           ModeName(s.driver.Mode(n)));
    EXPECT(s.driver.NativeDraws(n) == 1, "and the game draws its plates");
    s.driver.Post();
    const size_t declinedOutput = s.driver.Outputs().size();
    s.driver.OutputDeferred(n);
    ++n;
    // The GPU catches up. The three deferred passes each drew their own frame's image; the last output presented the
    // native record, so it shows the background and no plate image.
    s.driver.FinishDeferred();
    EXPECT(s.driver.Outputs()[declinedOutput].drawn == 0, "the declined frame's output drew nothing: %u",
           s.driver.Outputs()[declinedOutput].drawn);
    EXPECT(s.Now().plated - before.plated == 3, "the three deferred passes drew their images: %llu",
           static_cast<unsigned long long>(s.Now().plated - before.plated));
    EXPECT(ext::hudless::plates::Snapshot().pinsOutstanding == 0, "no pin survives the lag");
    EXPECT(ext::hudless::plates::sync::Violations() == 0, "lock violation: %s",
           ext::hudless::plates::sync::LastViolation().c_str());
    const int frames = UntilHidden(s, n);
    EXPECT(s.driver.Mode(n - 1) == FrameMode::Hidden, "the engine hides again once the GPU caught up (%d frames)", frames);
    EXPECT(s.driver.Outputs().back().drawn == n - 1, "and the new frame's image is drawn");
    Common(s, "S24");
    std::puts("    S24 GPU lag: the queue does not complete, every slot stays busy, a frame is declined and stays "
              "native, and the deferred passes drew their own images");
}

// S25: slot creation follows the capture, not the key.
void SlotPolicy()
{
    Scenario s(3);
    unsigned n = 1;
    // An HDR output is never captured: no slot is created and every frame stays native.
    s.driver.Hdr(true);
    s.driver.Interp(PlateInterp::Endpoint);
    for (int i = 0; i < 6; ++i)
    {
        Frame(s, n);
        EXPECT(s.driver.Mode(n - 1) != FrameMode::Hidden, "no frame hides on an HDR output: %s",
               ModeName(s.driver.Mode(n - 1)));
        EXPECT(s.driver.NativeDraws(n - 1) == 1, "and the game draws its plates");
    }
    EXPECT(s.Delta().slotsCreated == 0, "no slot is created for an HDR output: %llu",
           static_cast<unsigned long long>(s.Delta().slotsCreated));
    // SDR: the slots are created and the engine arms.
    s.driver.Hdr(false);
    const int armed = UntilHidden(s, n);
    EXPECT(s.driver.Mode(n - 1) == FrameMode::Hidden, "hidden in SDR (%d frames): %s", armed,
           ModeName(s.driver.Mode(n - 1)));
    EXPECT(s.Delta().slotsCreated == 3, "three slots for the SDR capture: %llu",
           static_cast<unsigned long long>(s.Delta().slotsCreated));
    // The key goes off: the next present has no capture to want, so its native record ends the drain and the slots go.
    const Counters before = s.Now();
    s.driver.Interp(PlateInterp::Off);
    Frame(s, n); // declined: the native record ends the drain
    EXPECT(s.Now().slotsRetired - before.slotsRetired == 3, "the key going off hands the three slots back: %llu",
           static_cast<unsigned long long>(s.Now().slotsRetired - before.slotsRetired));
    // The key coming back on rebuilds them.
    const Counters beforeOn = s.Now();
    s.driver.Interp(PlateInterp::Endpoint);
    const int armed2 = UntilHidden(s, n);
    EXPECT(s.driver.Mode(n - 1) == FrameMode::Hidden, "hidden again after the key came back (%d frames)", armed2);
    EXPECT(s.Now().slotsCreated - beforeOn.slotsCreated == 3, "and three slots were created again: %llu",
           static_cast<unsigned long long>(s.Now().slotsCreated - beforeOn.slotsCreated));
    Common(s, "S25");
    std::puts("    S25 slot policy: no slot for an HDR output, the key going off hands the slots back, and the key "
              "coming back creates them again");
}

// S26: a shadow frame whose produce signal fails. The game drew that frame's plates natively, so an older hidden record
// must not stay drawable over them.
void ShadowSignalFailure()
{
    Scenario s(3);
    unsigned n = 1;
    WarmUp(s, n);
    Frame(s, n);
    const unsigned hiddenFrame = n - 1;
    EXPECT(s.driver.Mode(hiddenFrame) == FrameMode::Hidden, "hidden before the failure");
    // A skipped output closes the gate: the next frame is a shadow frame (the game draws it itself too).
    s.driver.FailNextOutput();
    s.driver.Output(hiddenFrame);
    s.driver.Frame(n);
    EXPECT(s.driver.Mode(n) == FrameMode::Shadow, "the frame under the failure is a shadow frame: %s",
           ModeName(s.driver.Mode(n)));
    EXPECT(s.driver.NativeDraws(n) == 1, "and the game drew its plates");
    const Counters before = s.Now();
    const size_t beforeOutputs = s.driver.Outputs().size();
    s.driver.QueueMissing();
    s.driver.Post();
    EXPECT(s.Now().native - before.native == 1, "the frame publishes a native record");
    s.driver.Output(n);
    ExpectNothing(s.driver.Outputs(), beforeOutputs, s.driver.Outputs().size());
    ++n;
    const int frames = UntilHidden(s, n);
    EXPECT(s.driver.Mode(n - 1) == FrameMode::Hidden, "the engine re-arms afterwards (%d frames)", frames);
    Common(s, "S26");
    std::puts("    S26 shadow frame, signal fails: the native record replaces the older hidden one, so the pass draws "
              "nothing over the plates the game drew");
}

// S27: a native record followed directly by a hidden one (no shadow frame in between). The native record clears the
// alive proof, so the hide gate cannot open on it whatever order the outputs present in.
void NativeThenHidden()
{
    Scenario s(3);
    unsigned n = 1;
    WarmUp(s, n);
    Frame(s, n);
    EXPECT(s.driver.Mode(n - 1) == FrameMode::Hidden, "hidden before the native frame");
    // A frame with no plate draw publishes a native record and clears the alive proof.
    const unsigned nativeFrame = n;
    s.driver.Frame(nativeFrame, false);
    s.driver.Post();
    EXPECT(!ext::hudless::plates::HideAllowed(), "the native record closed the hide gate");
    ++n;
    // The next frame cannot hide whatever order its outputs present in: the loss is bounded by the shadow frame.
    s.driver.Frame(n);
    EXPECT(s.driver.Mode(n) == FrameMode::Shadow, "the frame after a native one is shadow: %s",
           ModeName(s.driver.Mode(n)));
    s.driver.Post();
    // A late output of the native batch draws the newest record, which draws nothing over plates the game drew.
    const size_t before = s.driver.Outputs().size();
    s.driver.Output(nativeFrame);
    ExpectNothing(s.driver.Outputs(), before, s.driver.Outputs().size());
    s.driver.Output(n);
    ExpectNothing(s.driver.Outputs(), before, s.driver.Outputs().size());
    ++n;
    const int frames = UntilHidden(s, n);
    EXPECT(s.driver.Mode(n - 1) == FrameMode::Hidden, "and it hides again afterwards (%d frames)", frames);
    Common(s, "S27");
    std::puts("    S27 native then hidden: the native record clears the alive proof, so no order of outputs draws an "
              "image over a frame the game drew");
}
// S28: the seam routing with both feature bits on. The bridge's present entry, frame finish, output and release each
// reach their own body exactly once per call, and the plate engine publishes exactly one record per present: a Hidden
// record stays the drawable one through its present instead of being replaced by a Native record a second call wrote.
void SeamRouting()
{
    Scenario s(3);
    unsigned n = 1;
    ext::hudless::plates::SetPlateFeatureBitSink(&BitSinkWithFeature);
    ext::SetActive(ext::F::Hudless, true);
    ext::SetActive(ext::F::Plate, true);
    s.driver.ThroughSeams(true);
    s.driver.Interp(PlateInterp::Endpoint);
    // One warm-up through the product's seams: the frame boundary, the frame finish and every output, once each.
    for (int i = 0; i < 4; ++i)
    {
        s.driver.Frame(n);
        const int beforePost = g_detailCalls;
        s.driver.Post();
        EXPECT(g_detailCalls - beforePost == 1, "the HUD-less present body ran once for frame %u (%d)", n,
               g_detailCalls - beforePost);
        ++n;
        for (int k = 0; k < 3; ++k)
        {
            const int beforeOutput = g_detailCalls;
            s.driver.Output(n - 1);
            EXPECT(g_detailCalls - beforeOutput == 1, "the HUD-less overlay body ran once for an output (%d)",
                   g_detailCalls - beforeOutput);
        }
    }
    // A hidden frame through the seams: one record published, and it is still the drawable record afterwards.
    const Counters beforeFrame = s.Now();
    s.driver.Frame(n);
    EXPECT(s.driver.Mode(n) == FrameMode::Hidden, "frame %u hidden through the seams: %s", n, ModeName(s.driver.Mode(n)));
    s.driver.Post();
    EXPECT(s.Now().promoted - beforeFrame.promoted == 1, "one record per present through the seams (%llu)",
           static_cast<unsigned long long>(s.Now().promoted - beforeFrame.promoted));
    EXPECT(ext::hudless::plates::HideAllowed(), "the hidden record is still the drawable one after its present");
    const size_t beforeOutput = s.driver.Outputs().size();
    s.driver.Output(n);
    ExpectDrawn(s.driver.Outputs(), beforeOutput, s.driver.Outputs().size(), n);
    ++n;
    // The release seam: one call, one body, and the engine's slots are gone afterwards.
    const int beforeRelease = g_detailCalls;
    ext::hudless::OnReleaseInterop(s.driver.Bridge());
    EXPECT(g_detailCalls - beforeRelease == 1, "the HUD-less release body ran once (%d)", g_detailCalls - beforeRelease);
    s.driver.ThroughSeams(false);
    ext::SetActive(ext::F::Hudless, false);
    ext::SetActive(ext::F::Plate, false);
    Common(s, "S28");
    std::puts("    S28 seam routing: present, output and release each reach the HUD-less engine once, and a hidden record"
              " stays the drawable one through its present");
}

// S29: the key goes off while a pass read is still outstanding. The slots are handed back when their fences complete, on
// a later present, and the engine keeps its feature bit (and so its seams) until every slot is back.
void KeyOffWithLag()
{
    Scenario s(3);
    unsigned n = 1;
    ext::hudless::plates::SetPlateFeatureBitSink(&BitSinkWithFeature);
    ext::SetActive(ext::F::Hudless, true);
    ext::SetActive(ext::F::Plate, true);
    s.driver.ThroughSeams(true);
    WarmUp(s, n);
    Frame(s, n); // hidden
    const unsigned hiddenFrame = n - 1;
    EXPECT(s.driver.Mode(hiddenFrame) == FrameMode::Hidden, "hidden before the key goes off: %s",
           ModeName(s.driver.Mode(hiddenFrame)));
    BitCalls().clear();
    // One output whose GPU work cannot complete: the slot it reads stays out of service.
    s.driver.OutputDeferred(hiddenFrame);
    s.driver.Interp(PlateInterp::Off); // the key goes off at this instant
    const Counters before = s.Now();
    // The key-off present publishes a native record, so nothing is drawable any more, but the slots cannot go yet.
    Frame(s, n, 0);
    EXPECT(s.Now().slotsRetired - before.slotsRetired < 3,
           "the slot under the read is not handed back (%llu)",
           static_cast<unsigned long long>(s.Now().slotsRetired - before.slotsRetired));
    EXPECT(BitCalls().empty(), "and the feature bit stays until every slot is back");
    // The queue catches up; the next present finishes the hand-back and only then drops the bit.
    s.driver.FinishDeferred();
    Frame(s, n, 0);
    EXPECT(s.Now().slotsRetired - before.slotsRetired == 3, "every slot is handed back once its reader completed (%llu)",
           static_cast<unsigned long long>(s.Now().slotsRetired - before.slotsRetired));
    EXPECT(!BitCalls().empty() && BitCalls().back() == false, "and only then does the bit go");
    s.driver.ThroughSeams(false);
    ext::SetActive(ext::F::Hudless, false);
    ext::SetActive(ext::F::Plate, false);
    Common(s, "S29");
    std::puts("    S29 key off with a lagging queue: nothing is handed back while a slot is read, and all three slots and"
              " the feature bit follow once the fences complete");
}

// S30: the present clock. The slot ring's stale rule and the pass's reader are stamped on the same counter, so a pass
// read that has not completed is still respected after hundreds of presents (on two clocks it looked stale after about
// 120 and the slot was handed out under the presenting queue).
void LongPresentRun()
{
    Scenario s(3);
    unsigned n = 1;
    s.driver.Interp(PlateInterp::Endpoint);
    // 300 presents with no output: the pass never runs, but the engine's present clock runs far past the ring's
    // 240-frame stale window.
    for (int i = 0; i < 300; ++i)
        Frame(s, n, 0);
    WarmUp(s, n);
    const Counters before = s.Now();
    for (int i = 0; i < 3; ++i)
    {
        Frame(s, n, 0);
        EXPECT(s.driver.Mode(n - 1) == FrameMode::Hidden, "frame %u hidden long after the stale window: %s", n - 1,
               ModeName(s.driver.Mode(n - 1)));
        s.driver.OutputDeferred(n - 1); // its read cannot complete
        ++n;
    }
    s.driver.Frame(n);
    EXPECT(s.driver.Mode(n) == FrameMode::Declined,
           "with every slot still read by a submitted pass the frame is declined: %s", ModeName(s.driver.Mode(n)));
    EXPECT(s.driver.NativeDraws(n) == 1, "and the game draws its plates");
    s.driver.Post();
    s.driver.OutputDeferred(n);
    ++n;
    const Counters saturated = s.Now();
    EXPECT(saturated.declined - before.declined >= 1, "a frame was refused rather than handed a slot under a read");
    s.driver.FinishDeferred();
    EXPECT(s.Now().plated - before.plated == 3, "the three deferred passes drew their own images (%llu)",
           static_cast<unsigned long long>(s.Now().plated - before.plated));
    EXPECT(s.Now().pinsOutstanding == 0, "no pin survives the run");
    Common(s, "S30");
    std::puts("    S30 long present run: after 300 presents the pass's reader still holds its slot, so the frames under"
              " it are declined and no slot is rewritten while it is read");
}

// S31: the per-output shift path. With movers from the frame finish and a locked batch model, the first output of a batch
// draws the plate image shifted by its share and the last one draws it unshifted; endpoint never shifts.
void MoverFinish(ext::hudless::plates::PlateFrame& frame, void* user)
{
    (void) user;
    // One plate in the middle of the image that moved 20 px right in the frame: the pass moves it back by (1 - s) * 20.
    frame.moverCount = 1;
    frame.movers[0] = { 24.0f, 12.0f, 40.0f, 20.0f, -20.0f, 0.0f, false };
}

void MoverShift()
{
    Scenario s(3);
    unsigned n = 1;
    ext::hudless::plates::SetPlateFinishHook(&MoverFinish, nullptr);
    s.driver.Interp(PlateInterp::On);
    // Nine consistent batches of three outputs lock the phase model; every frame carries the mover.
    for (int i = 0; i < 12; ++i)
        Frame(s, n);
    EXPECT(s.driver.Mode(n - 1) == FrameMode::Hidden, "hidden with movers: %s", ModeName(s.driver.Mode(n - 1)));
    s.driver.Frame(n);
    EXPECT(s.driver.Mode(n) == FrameMode::Hidden, "the next frame is hidden: %s", ModeName(s.driver.Mode(n)));
    s.driver.Post();
    const size_t before = s.driver.Outputs().size();
    s.driver.Output(n); // k = 0 of 3: the shift is 13 px, so the plate leaves the centre pixel
    EXPECT(ext::hudless::plates::ShiftingActive(), "the first output of the batch draws a shifted plate");
    EXPECT(s.driver.Outputs()[before].drawn == 0, "the shifted plate is out from under the centre pixel (drew %u)",
           s.driver.Outputs()[before].drawn);
    s.driver.Output(n); // k = 1 of 3: 7 px, still over the centre
    EXPECT(ext::hudless::plates::ShiftingActive(), "the second output shifts too");
    EXPECT(s.driver.Outputs()[before + 1].drawn == n, "the smaller shift still covers the centre pixel (drew %u)",
           s.driver.Outputs()[before + 1].drawn);
    s.driver.Output(n); // k = 2 of 3: the real frame, unshifted
    EXPECT(!ext::hudless::plates::ShiftingActive(), "the last output of the batch draws the frame unshifted");
    EXPECT(s.driver.Outputs()[before + 2].drawn == n, "and covers the centre pixel (drew %u)",
           s.driver.Outputs()[before + 2].drawn);
    ++n;
    // Endpoint draws the same record whole, mover or not.
    s.driver.Interp(PlateInterp::Endpoint);
    Frame(s, n);
    EXPECT(!ext::hudless::plates::ShiftingActive(), "endpoint shifts nothing");
    ExpectDrawn(s.driver.Outputs(), s.driver.Outputs().size() - 3, s.driver.Outputs().size(), n - 1);
    ext::hudless::plates::SetPlateFinishHook(nullptr, nullptr);
    Common(s, "S31");
    std::puts("    S31 mover shift: the locked batch model shifts the first outputs of a batch by their share, the real"
              " frame unshifted, and endpoint never shifts");
}

// S32: a release during shutdown returns at once (no lock, no retirement, no wait: it can run under the loader lock).
void ShutdownRelease()
{
    Scenario s(3);
    unsigned n = 1;
    WarmUp(s, n);
    Frame(s, n);
    EXPECT(s.driver.Mode(n - 1) == FrameMode::Hidden, "hidden before the shutdown release: %s",
           ModeName(s.driver.Mode(n - 1)));
    const Counters before = s.Now();
    s.driver.ShuttingDown(true);
    s.driver.Release();
    EXPECT(s.Now().slotsRetired == before.slotsRetired, "a release during shutdown hands nothing back (%llu)",
           static_cast<unsigned long long>(s.Now().slotsRetired - before.slotsRetired));
    EXPECT(ext::hudless::plates::sync::Violations() == 0, "and takes no lock: %s",
           ext::hudless::plates::sync::LastViolation().c_str());
    s.driver.ShuttingDown(false);
    // The engine still works afterwards: the open frame is dropped by the next reservation and the slots are rebuilt.
    const int frames = UntilHidden(s, n);
    EXPECT(s.driver.Mode(n - 1) == FrameMode::Hidden, "the engine hides again after a shutdown release (%d frames)",
           frames);
    Common(s, "S32");
    std::puts("    S32 shutdown release: nothing is retired and no lock is taken, and the engine keeps working after it");
}

// S33: a release on another thread than the draws is not a two-thread break. A resize may move the render thread, so the
// release re-latches the drawing thread instead of failing the engine open for the session.
void ReleaseOnAnotherThread()
{
    Scenario s(3);
    unsigned n = 1;
    WarmUp(s, n);
    Frame(s, n);
    EXPECT(s.driver.Mode(n - 1) == FrameMode::Hidden, "hidden before the release: %s", ModeName(s.driver.Mode(n - 1)));
    const Counters before = s.Now();
    std::thread release([&] { s.driver.Release(); });
    release.join();
    const int frames = UntilHidden(s, n);
    EXPECT(s.driver.Mode(n - 1) == FrameMode::Hidden, "the engine hides again after a release on another thread (%d frames)",
           frames);
    EXPECT(s.Now().affinityBroken - before.affinityBroken == 0, "and no affinity break was latched (%llu)",
           static_cast<unsigned long long>(s.Now().affinityBroken - before.affinityBroken));
    Common(s, "S33");
    std::puts("    S33 release on another thread: the drawing thread is re-latched, not failed open");
}

// S34: a Hidden record the pass keeps sitting out holds the gate closed for a while after six in a row, so a fault that
// only hits hidden draws costs a few frames instead of every second one.
void HiddenSkipBreaker()
{
    Scenario s(3);
    unsigned n = 1;
    WarmUp(s, n);
    const Counters before = s.Now();
    for (int i = 0; i < 6; ++i)
    {
        Frame(s, n, 0);
        EXPECT(s.driver.Mode(n - 1) == FrameMode::Hidden, "frame %u hidden before the refused output: %s", n - 1,
               ModeName(s.driver.Mode(n - 1)));
        s.driver.FailNextOutput();
        s.driver.Output(n - 1);
        ++n;
        if (i < 5)
        {
            // The skip closed the gate; this shadow frame proves the pass again, and the next frame is hidden.
            Frame(s, n, 0);
            EXPECT(s.driver.Mode(n - 1) == FrameMode::Shadow, "the frame after skip %d is a shadow frame: %s", i + 1,
                   ModeName(s.driver.Mode(n - 1)));
            s.driver.Output(n - 1);
        }
    }
    EXPECT(s.Now().hiddenSkipped - before.hiddenSkipped == 6, "six hidden records were sat out (%llu)",
           static_cast<unsigned long long>(s.Now().hiddenSkipped - before.hiddenSkipped));
    // The pass has proved itself again, so the gate would be open; the breaker holds it closed instead.
    EXPECT(!ext::hudless::plates::HideAllowed(), "six hidden skips in a row hold the hide gate closed");
    Frame(s, n);
    EXPECT(s.driver.Mode(n - 1) == FrameMode::Shadow, "so the next frame is a shadow frame: %s",
           ModeName(s.driver.Mode(n - 1)));
    Frame(s, n, 0);
    EXPECT(s.driver.Mode(n - 1) == FrameMode::Shadow, "and the hold outlasts another frame: %s",
           ModeName(s.driver.Mode(n - 1)));
    s.driver.Output(n - 1);
    // Once the hold is over the engine hides again (the record published in between is fresh again).
    plate_test_clock::Advance(1100);
    Frame(s, n);
    EXPECT(s.driver.Mode(n - 1) == FrameMode::Shadow, "the frame after the jump is a shadow frame (stale record): %s",
           ModeName(s.driver.Mode(n - 1)));
    Frame(s, n);
    EXPECT(s.driver.Mode(n - 1) == FrameMode::Hidden, "and it hides again once the hold is over: %s",
           ModeName(s.driver.Mode(n - 1)));
    Common(s, "S34");
    std::puts("    S34 hidden-skip breaker: six hidden skips in a row hold the hide gate closed until the hold is over");
}

// S35: a pass that copies a Hidden record while the game publishes a Native one must not re-open the hide gate when it
// finishes: the alive proof belongs to the newest record only, or the next frame hides and loses its plates.
void LatePassKeepsGateClosed()
{
    Scenario s(3);
    unsigned n = 1;
    WarmUp(s, n);
    Frame(s, n); // hidden
    const unsigned hiddenFrame = n - 1;
    EXPECT(s.driver.Mode(hiddenFrame) == FrameMode::Hidden, "hidden before the native frame: %s",
           ModeName(s.driver.Mode(hiddenFrame)));
    PauseGate gate("pass.pinned");
    std::thread pass(
        [&]()
        {
            try
            {
                s.driver.OutputDeferred(hiddenFrame);
            }
            catch (const std::exception& error)
            {
                std::printf("    the presenting thread failed: %s\n", error.what());
                ++g_failures;
            }
        });
    EXPECT(gate.WaitReached(), "the pass reached its pinned point");
    // The game draws its plates itself and publishes a native record while the pass still holds the older record.
    s.driver.Frame(n, false);
    s.driver.Post();
    ++n;
    EXPECT(!ext::hudless::plates::HideAllowed(), "the native record closed the hide gate");
    gate.Release();
    pass.join();
    s.driver.FinishDeferred();
    EXPECT(s.Now().pinsOutstanding == 0, "the pin came back");
    EXPECT(!ext::hudless::plates::HideAllowed(), "the finished pass does not re-open the gate over the native frame");
    s.driver.Frame(n);
    EXPECT(s.driver.Mode(n) == FrameMode::Shadow, "the frame after the tearing pass is a shadow frame: %s",
           ModeName(s.driver.Mode(n)));
    s.driver.Post();
    s.driver.Output(n);
    ++n;
    const int frames = UntilHidden(s, n);
    EXPECT(s.driver.Mode(n - 1) == FrameMode::Hidden, "and it hides again afterwards (%d frames)", frames);
    Common(s, "S35");
    std::puts("    S35 late pass: a pass whose record was replaced by a native frame keeps the gate closed");
}

// The reservations the soak saw, and how many of them were handed a slot with a pass read pinned on it.
std::atomic<int> g_reservations { 0 };
std::atomic<int> g_reservationsWhilePinned { 0 };

void WatchReservations(int slot, unsigned pins)
{
    ++g_reservations;
    if (slot >= 0 && pins > 0)
        ++g_reservationsWhilePinned;
}

// A handshake between the two sides of a soak: waits, and ends the run with a failure instead of deadlocking the suite
// when the other side never gets there. The deadline only has to catch a stopped thread, never a slow one.
void WaitFor(const std::atomic<bool>& flag, int seconds)
{
    for (int i = 0; i < seconds * 1000; ++i)
    {
        if (flag.load(std::memory_order_acquire))
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::printf("    FAIL the other side of the two-thread soak did not reach its handshake within %d s\n", seconds);
    std::fflush(stdout);
    std::_Exit(1);
}

// S36: the two-thread soak. The game side runs only after the presenting side queued its reads (a handshake, not a sleep),
// and the present clock is driven far ahead of the frame records before the record is published: a pass reader stamped
// with anything but the record's present then looks stale, which is the state a slot read is dropped in. The checks are
// the ones no interleaving can change: the frames under the queued reads are declined, the pass drew the image it pinned
// (a slot rewritten under the read shows the newer frame), and no slot was handed out under a pin. The body runs in a
// task with a deadline, so a deadlock stops the scenario with a message instead of stalling the suite.
void SoakBody()
{
    Scenario s(3);
    unsigned n = 1;
    s.driver.Interp(PlateInterp::Endpoint);
    // 280 present entries with no frame record in between: the engine's present clock is now far ahead of the record
    // count. A reader stamped with the record serial would be past the ring's 240-frame stale window at its first check,
    // while the reader of the record's own present is not.
    for (int i = 0; i < 280; ++i)
        s.driver.PresentBegin();
    WarmUp(s, n);
    Frame(s, n);
    const unsigned imageFrame = n - 1;
    EXPECT(s.driver.Mode(imageFrame) == FrameMode::Hidden, "the frame before the soak is hidden: %s",
           ModeName(s.driver.Mode(imageFrame)));
    const Counters before = s.Now();
    const size_t firstOutput = s.driver.Outputs().size();
    g_reservations = 0;
    g_reservationsWhilePinned = 0;
    ext::hudless::plates::sync::SetReserveHook(&WatchReservations);
    // The presenting side queues three reads of the hidden record behind a blocked queue: the pin goes with each output
    // and the reader fence stays open until the queue is let go.
    constexpr int kOutputs = 3;
    std::atomic<bool> queued { false };
    std::thread presenting(
        [&]()
        {
            try
            {
                for (int i = 0; i < kOutputs; ++i)
                    s.driver.OutputDeferred(imageFrame);
            }
            catch (const std::exception& error)
            {
                std::printf("    the presenting thread failed: %s\n", error.what());
                ++g_failures;
            }
            queued.store(true, std::memory_order_release);
        });
    WaitFor(queued, 60);
    // Two of the three slots are free, so the first two frames take them; from then on the ring is held by the queued
    // reads and the frames are declined whatever order the two sides run in.
    constexpr int kFrames = 6;
    for (int i = 0; i < kFrames; ++i)
    {
        s.driver.Frame(n);
        s.driver.Post();
        ++n;
    }
    const unsigned firstFrame = n - kFrames;
    for (unsigned f = firstFrame; f < firstFrame + 2 && f < n; ++f)
        EXPECT(s.driver.Mode(f) != FrameMode::Declined, "frame %u takes one of the two free slots: %s", f,
               ModeName(s.driver.Mode(f)));
    for (unsigned f = firstFrame + 2; f < n; ++f)
        EXPECT(s.driver.Mode(f) == FrameMode::Declined, "frame %u under the queued reads is declined: %s", f,
               ModeName(s.driver.Mode(f)));
    presenting.join();
    s.driver.FinishDeferred();
    EXPECT(s.driver.Outputs().size() - firstOutput == static_cast<size_t>(kOutputs),
           "every queued output was read back (%zu)", s.driver.Outputs().size() - firstOutput);
    // The last queued output wrote the backbuffer after the frames ran: it must hold the image it pinned. A slot the game
    // side was handed and rewrote while the read was queued would show that frame's colour instead.
    EXPECT(s.driver.Outputs().back().drawn == imageFrame,
           "the pass under the queued reads drew the image it pinned (drew %u, expected %u)",
           s.driver.Outputs().back().drawn, imageFrame);
    EXPECT(s.Now().plated - before.plated == static_cast<uint64_t>(kOutputs),
           "and every queued output plated it (%llu)",
           static_cast<unsigned long long>(s.Now().plated - before.plated));
    // The concurrent run: both sides work at the same time, and every check below holds whichever side wins a race.
    const Counters mid = s.Now();
    const size_t secondFirst = s.driver.Outputs().size();
    constexpr int kOutputs2 = 2;
    constexpr int kFrames2 = 4;
    std::thread again(
        [&]()
        {
            try
            {
                for (int i = 0; i < kOutputs2; ++i)
                    s.driver.OutputDeferred(n - 1);
            }
            catch (const std::exception& error)
            {
                std::printf("    the presenting thread failed: %s\n", error.what());
                ++g_failures;
            }
        });
    for (int i = 0; i < kFrames2; ++i)
    {
        s.driver.Frame(n);
        s.driver.Post();
        ++n;
    }
    again.join();
    s.driver.FinishDeferred();
    ext::hudless::plates::sync::SetReserveHook(nullptr);
    EXPECT(s.driver.Outputs().size() - secondFirst == static_cast<size_t>(kOutputs2),
           "every output of the concurrent run was read back (%zu)", s.driver.Outputs().size() - secondFirst);
    // An output either drew the image it read or was skipped by the pass (a native record while the queue was held):
    // nothing is lost between the two, and no output is counted twice.
    const uint64_t plated = s.Now().plated - mid.plated;
    const uint64_t skipped = s.Now().skipped - mid.skipped;
    EXPECT(plated + skipped <= static_cast<uint64_t>(kOutputs2), "no output was plated or skipped twice (%llu plated,"
           " %llu skipped)", static_cast<unsigned long long>(plated), static_cast<unsigned long long>(skipped));
    EXPECT(g_reservationsWhilePinned == 0, "no frame was handed a slot with a pass read pinned on it (%d of %d)",
           g_reservationsWhilePinned.load(), g_reservations.load());
    for (const auto& output : s.driver.Outputs())
        if (output.pending)
            EXPECT(false, "an output was left pending after the run");
    Common(s, "S36");
    std::puts("    S36 two-thread soak: the frames under the queued reads are declined, the pass drew the image it pinned,"
              " no slot was handed out under a pin, and both sides finished");
}

void TwoThreadSoak()
{
    std::future<void> body = std::async(std::launch::async, &SoakBody);
    if (body.wait_for(std::chrono::seconds(120)) != std::future_status::ready)
    {
        std::printf("    FAIL the two-thread soak did not finish within 120 s: the engine stopped making progress\n");
        std::fflush(stdout);
        std::_Exit(1);
    }
    body.get();
}

// S37: a pass build that fails is latched for the pass set it belongs to, and a release clears the latch: the next build
// gets its own attempt. Without the reset the latch would hold for the rest of the session, every output would be skipped
// and every frame a shadow frame, which is a fail-open but a permanent one.
void PassBuildLatch()
{
    Scenario s(3);
    unsigned n = 1;
    WarmUp(s, n);
    Frame(s, n);
    EXPECT(s.driver.Mode(n - 1) == FrameMode::Hidden, "hidden before the failure: %s", ModeName(s.driver.Mode(n - 1)));
    // A release parks the pass objects, so the next output rebuilds them. No D3D12 creation call can be made to fail on a
    // software adapter, so the check build asks for the failure.
    s.driver.Release();
    ext::hudless::plates::sync::FailPassBuilds(1);
    WarmUp(s, n);
    const Counters before = s.Now();
    Frame(s, n);
    EXPECT(s.driver.Mode(n - 1) != FrameMode::Hidden, "no frame hides while the failed pass build is latched: %s",
           ModeName(s.driver.Mode(n - 1)));
    s.driver.Output(n - 1);
    EXPECT(s.driver.LastSkip() == "plate pass objects failed", "the output says the pass objects failed: %s",
           s.driver.LastSkip().c_str());
    EXPECT(s.Now().plated == before.plated, "and nothing was plated (%llu)",
           static_cast<unsigned long long>(s.Now().plated - before.plated));
    // The release ends the failure's session: the next output builds the pass again, and this build succeeds.
    s.driver.Release();
    WarmUp(s, n);
    Frame(s, n);
    EXPECT(s.driver.Mode(n - 1) == FrameMode::Hidden, "the engine hides again once the release cleared the latch: %s",
           ModeName(s.driver.Mode(n - 1)));
    const size_t beforeOutput = s.driver.Outputs().size();
    s.driver.Output(n - 1);
    ExpectDrawn(s.driver.Outputs(), beforeOutput, s.driver.Outputs().size(), n - 1);
    Common(s, "S37");
    std::puts("    S37 failed pass build: the failure is latched (no hide, every output says why) and a release clears it, "
              "so the next build draws again");
}

// S38: a device whose produce fence cannot be created is latched: the failure and its first dropped frame are logged once,
// not once per present, and a release clears the latch so the same device gets its own attempt.
void FailedDeviceLatch()
{
    Scenario s(3);
    unsigned n = 1;
    WarmUp(s, n);
    Frame(s, n);
    EXPECT(s.driver.Mode(n - 1) == FrameMode::Hidden, "hidden before the failure: %s", ModeName(s.driver.Mode(n - 1)));
    const int failuresBefore = CountLog("Plates: the plate produce fence could not be created");
    const int dropsBefore = CountLog("Plates: the device changed");
    // The release takes the device away; the next present binds it again, and that fence creation fails.
    s.driver.Release();
    ext::hudless::plates::sync::FailProduceFences(1);
    s.driver.Frame(n);
    s.driver.Post();
    ++n;
    EXPECT(CountLog("Plates: the plate produce fence could not be created") - failuresBefore == 1,
           "the failure is logged once (%d)", CountLog("Plates: the plate produce fence could not be created") - failuresBefore);
    EXPECT(CountLog("Plates: the device changed") - dropsBefore == 1, "and the frame it dropped is logged (%d)",
           CountLog("Plates: the device changed") - dropsBefore);
    const Counters before = s.Now();
    for (int i = 0; i < 5; ++i)
    {
        s.driver.Frame(n);
        s.driver.Post();
        ++n;
    }
    // The latched device is not retried on every present, and neither is its warning: the present rate must not become
    // the log rate.
    EXPECT(CountLog("Plates: the plate produce fence could not be created") - failuresBefore == 1,
           "the failed device is not rebuilt on every present (%d)",
           CountLog("Plates: the plate produce fence could not be created") - failuresBefore);
    EXPECT(CountLog("Plates: the device changed") - dropsBefore == 1, "and its drop is not logged on every present (%d)",
           CountLog("Plates: the device changed") - dropsBefore);
    EXPECT(s.Now().plated == before.plated, "nothing is plated while the device is latched (%llu)",
           static_cast<unsigned long long>(s.Now().plated - before.plated));
    // A release ends the session on that device: the next present binds it, this time with a fence, and the pass works.
    s.driver.Release();
    WarmUp(s, n);
    Frame(s, n);
    EXPECT(s.driver.Mode(n - 1) == FrameMode::Hidden, "the engine hides again once the release cleared the latch: %s",
           ModeName(s.driver.Mode(n - 1)));
    const size_t beforeOutput = s.driver.Outputs().size();
    s.driver.Output(n - 1);
    ExpectDrawn(s.driver.Outputs(), beforeOutput, s.driver.Outputs().size(), n - 1);
    Common(s, "S38");
    std::puts("    S38 failed device: one failure line and one drop line, no line per present, and a release clears the "
              "latch so the device is tried again");
}

// S39: a release waits for a pass that is running. A pass copies the record and takes a pin before it records its read, so
// the release may not retire the slots under it.
void ReleaseWaitsForPass()
{
    Scenario s(3);
    unsigned n = 1;
    WarmUp(s, n);
    Frame(s, n);
    const unsigned imageFrame = n - 1;
    EXPECT(s.driver.Mode(imageFrame) == FrameMode::Hidden, "hidden before the release: %s",
           ModeName(s.driver.Mode(imageFrame)));
    PauseGate gate("pass.pinned");
    std::thread pass(
        [&]()
        {
            try
            {
                s.driver.OutputDeferred(imageFrame);
            }
            catch (const std::exception& error)
            {
                std::printf("    the presenting thread failed: %s\n", error.what());
                ++g_failures;
            }
        });
    EXPECT(gate.WaitReached(), "the pass reached its pinned point");
    std::atomic<bool> returned { false };
    std::thread release(
        [&]()
        {
            s.driver.Release();
            returned.store(true);
        });
    // The release cannot take the pass mutex while the pass holds it: it has not returned yet.
    for (int i = 0; i < 500 && !returned.load(std::memory_order_acquire); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    EXPECT(!returned.load(), "the release waits for the pass that is running");
    gate.Release();
    pass.join();
    release.join();
    EXPECT(returned.load(), "and returns once the pass is done");
    s.driver.FinishDeferred();
    EXPECT(s.driver.Outputs().back().drawn == imageFrame,
           "the pass that ran under the release drew the frame it pinned (drew %u)", s.driver.Outputs().back().drawn);
    Common(s, "S39");
    std::puts("    S39 release under a running pass: the release waits for the pass mutex, then the pass finishes and the "
              "release hands the slots back");
}
} // namespace

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    SteadyState();
    FirstArmedFrame();
    PassSkip();
    ReleaseWithOpenFrame();
    DeviceMissing();
    DroppedPresent();
    KeyToggles();
    MultiplierOne();
    MultiplierTwo();
    PresetPause();
    Resize();
    RefusedReservation();
    ProduceFailure();
    PassPin();
    OneBatchLag();
    EarlyOutputs();
    EmptyHiddenFrame();
    FreshnessWindows();
    GpuLag();
    SlotPolicy();
    ShadowSignalFailure();
    NativeThenHidden();
    PinAcrossReservation();
    SeamRouting();
    KeyOffWithLag();
    LongPresentRun();
    MoverShift();
    ShutdownRelease();
    ReleaseOnAnotherThread();
    HiddenSkipBreaker();
    LatePassKeepsGateClosed();
    TwoThreadSoak();
    PassBuildLatch();
    FailedDeviceLatch();
    ReleaseWaitsForPass();
    ThreadAffinity(); // last: the thread latch is permanent

    if (g_failures != 0)
    {
        std::printf("FAIL: %d check(s)\n", g_failures.load());
        return 1;
    }
    std::puts("PASS: plate engine protocol: three slots, reserve before hide, one record per present, pinned reads, "
              "warm-up, skips, release, a missing bridge device, a dropped present, key drain, multiplier 1/2, pause, "
              "resize, refusal, produce failure, lag, early outputs, empty images, freshness windows, GPU lag, slot "
              "policy, pins across a reservation, seam routing, key off with a lagging queue, a long present run, mover "
              "shifts, a shutdown release, a release on another thread, the hidden-skip breaker, a late pass, a "
              "two-thread soak, a failed pass build, a failed device, a release under a running pass, thread affinity");
    return 0;
}
