// Automatic ReShade scene depth, the whole loop of a session in one test: the REAL CaptureEngine over a WARP device (a fake parameter block
// and host, the real copy pass), the REAL DepthBinder over the fake ReShade world of the core tests, and the REAL EventDispatcher between
// them. The pieces have their own tests; this one checks that they agree where they meet:
//  - the counter ticks once per CAPTURE (the engine ticks it before it publishes), never at begin or finish; the stamp on a capture is
//    the frame the binder reads (delta 0), so every pass between two captures binds (frame generation runs several passes per
//    evaluate), the clock agrees, and the native device in the capture passes the backend's SameDevice;
//  - the consumer the binder records (the back buffer of the runtime that asked) is what the engine's size gate compares the NGX
//    Output with: a full cycle ends in Outcome::Bound, the DEPTH entry points at the copy only until finish, and the next frame again;
//  - a preview-sized Output, or a back buffer that is not the output size, never binds;
//  - a capture that stops being made goes stale by its AGE (250 ms), although the passes continue;
//  - refused passes and refused captures never move the counter.
#include "pch.h"

#include "reshade_depth_eval_rig.h"

#include "ext/reshade/ReShadeDepthDispatch.h"

#include "reshade_depth_fake.h"

using namespace depthtest;

namespace
{
struct EventHost final : IEventHost
{
    void NoteRuntime(uint64_t) override {}
    void ForgetRuntime(uint64_t) override {}
};

// The engine side (a rig on a WARP device) publishes into the binder; the binder's backend is the fake ReShade world, whose clock, frame
// counter and device are the engine's.
struct Loop final : ICaptureSink
{
    evaltest::Rig rig;
    Fake fake;
    DepthBinder binder { fake, nullptr };
    EventHost events;
    EventDispatcher dispatcher { binder, events };
    uint64_t startFrame = 0;

    ~Loop() { rig.engine.reset(); } // its last publication (valid = false) still has a binder to go to

    bool Create(UINT depthBind = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE)
    {
        if (!rig.Create(depthBind))
            return false;
        startFrame = rig.frame;
        fake.frameSource = &rig.frame; // the binder reads the counter the engine stamps with
        fake.device = reinterpret_cast<uint64_t>(rig.device.Get());
        fake.bbW = evaltest::kW; // the back buffer of ReShade's runtime is the size of the output
        fake.bbH = evaltest::kH;
        fake.now = rig.host.now;
        binder.SetEnabled(true);
        rig.sink.forward = this;
        rig.host.consumerSource = [this](int64_t nowMs) { return binder.GetConsumer(nowMs); };
        return true;
    }

    void Publish(const DepthCapture& capture) noexcept override
    {
        if (capture.valid)
            Adopt(capture.copyView);
        binder.PublishCapture(capture);
    }
    // The fake needs to know the copy's view: the owner's reference (the pass holds it) and a depth-family format.
    void Adopt(uint64_t view)
    {
        std::lock_guard lock(fake.refMutex);
        fake.owner.try_emplace(view, 1);
        fake.copyViews.insert(view);
        fake.depthViews.insert(view);
    }

    void Advance(int64_t ms)
    {
        rig.host.now += ms;
        fake.now = rig.host.now;
    }
    void Tick() { Advance(16); } // a new frame, 16 ms later
    void Eval() { rig.Eval(); }
    Outcome Begin() { return binder.OnBeginEffects(kRt); }
    void Finish() { dispatcher.Finish(kRt); }
    // One frame: the evaluate (unless skipped), then ReShade's begin_effects and finish_effects.
    Outcome Frame(bool evaluate = true)
    {
        Tick();
        if (evaluate)
            Eval();
        const Outcome outcome = Begin();
        Finish();
        return outcome;
    }
    const DepthCapture& LastCapture() const { return rig.sink.events.back().capture; }
};

void FullCycle()
{
    Loop l;
    if (!l.Create())
        return;
    // frame 1: nobody has asked for depth yet, so nothing is captured; the begin finds a trusted depth input, records the consumer
    // (the back buffer of that runtime) and answers "waiting"
    l.Tick();
    l.Eval();
    CHECK(l.rig.sink.events.empty());
    CHECK(l.Begin() == Outcome::NoCapture);
    const ConsumerInfo consumer = l.binder.GetConsumer(l.fake.now);
    CHECK(consumer.seen && consumer.width == evaltest::kW && consumer.height == evaltest::kH);
    l.Finish();
    CHECK(l.rig.frame == l.startFrame); // no capture, no tick (and a finish never ticks)

    // frame 2: the consumer is known, the engine captures (one tick), the begin binds
    l.Tick();
    l.Eval();
    CHECK(l.rig.sink.events.size() == 1);
    const DepthCapture c = l.LastCapture();
    CHECK(c.valid && c.copyView != 0);
    CHECK(l.rig.frame == l.startFrame + 1);
    CHECK(c.frame == l.rig.frame && c.frame == l.fake.Frame()); // delta 0: the stamp is the counter the binder reads
    CHECK(c.tickMs == l.fake.now);
    CHECK(c.device == l.fake.device);                           // the native device, which the backend accepts as the runtime's
    CHECK(c.outputW == evaltest::kW && c.outputH == evaltest::kH);
    CHECK(l.Begin() == Outcome::Bound);
    CHECK(l.fake.MapHoldsCopy() && l.fake.DepthIs(c.copyView, c.copyView));
    CHECK(l.binder.Status() == std::format("Automatic scene depth: {}x{}", evaltest::kW, evaltest::kH));
    l.Finish();
    CHECK(l.fake.DepthIs(kOrig, kOrigSrgb)); // ReShade's own binding is back
    CHECK(l.fake.Balanced() && !l.fake.HoldsCopy());
    CHECK(l.rig.frame == l.startFrame + 1); // the finish did not tick

    // the next frames: one tick each for the capture, stamped with it, bound again
    for (int i = 0; i < 3; ++i)
    {
        l.Tick();
        l.Eval();
        CHECK(l.rig.frame == l.startFrame + 2 + i);
        CHECK(l.LastCapture().frame == l.rig.frame && l.LastCapture().frame == l.fake.Frame());
        CHECK(l.Begin() == Outcome::Bound);
        l.Finish();
    }
    CHECK(l.binder.GetCounters().passesBound == 4);
    CHECK(l.binder.GetCounters().staleCaptures == 0);
    CHECK(l.fake.Balanced() && !l.fake.HoldsCopy());
}

// Frame generation: ReShade runs an effect pass for every OUTPUT frame, the evaluate runs once per real frame. Every pass between two
// evaluates sees the same stamp (delta 0) and binds, the depth of the real frame being the one that is there.
void ManyPassesPerEvaluate()
{
    Loop l;
    if (!l.Create())
        return;
    CHECK(l.Frame() == Outcome::NoCapture);
    l.Tick();
    l.Eval();
    const uint64_t counter = l.rig.frame;
    CHECK(l.rig.sink.events.size() == 1 && l.LastCapture().frame == counter);
    for (int pass = 0; pass < 5; ++pass)
    {
        CHECK(l.Begin() == Outcome::Bound);
        l.Finish();
        CHECK(l.fake.DepthIs(kOrig, kOrigSrgb));
        l.Tick(); // (a few ms between the passes of one real frame)
    }
    CHECK(l.binder.GetCounters().passesBound == 5 && l.binder.GetCounters().staleCaptures == 0);
    CHECK(l.rig.frame == counter); // five passes: not one tick

    // the next evaluate replaces the stamp, the next pass binds
    l.Tick();
    l.Eval();
    CHECK(l.rig.frame == counter + 1 && l.LastCapture().frame == counter + 1 && l.fake.Frame() == counter + 1);
    CHECK(l.Begin() == Outcome::Bound);
    l.Finish();
    CHECK(l.binder.GetCounters().passesBound == 6 && l.binder.GetCounters().staleCaptures == 0);
    CHECK(l.fake.Balanced() && !l.fake.HoldsCopy());
}

// An evaluate that stops (a menu, a loading screen, a cutscene without the upscaler): the passes go on, the last capture is caught by its
// AGE (250 ms), not by the counter, which only the capture side moves.
void ACaptureThatStopsGoesStaleByAge()
{
    Loop l;
    if (!l.Create())
        return;
    CHECK(l.Frame() == Outcome::NoCapture);
    CHECK(l.Frame() == Outcome::Bound);
    const uint64_t counter = l.rig.frame;
    l.Advance(250); // exactly the limit is still fresh
    CHECK(l.Begin() == Outcome::Bound);
    l.Finish();
    CHECK(l.binder.GetCounters().staleCaptures == 0);
    l.Advance(1);
    CHECK(l.Begin() == Outcome::CaptureUnavailable);
    l.Finish();
    CHECK(l.binder.GetCounters().staleCaptures == 1);
    for (int i = 0; i < 4; ++i)
    {
        l.Tick();
        CHECK(l.Begin() == Outcome::CaptureUnavailable);
        l.Finish();
    }
    CHECK(l.rig.frame == counter); // refused passes do not move the counter
    CHECK(l.fake.DepthIs(kOrig, kOrigSrgb)); // nothing of ours is in ReShade's map
    CHECK(l.Frame(true) == Outcome::Bound);  // and it recovers with the next evaluate
    CHECK(l.rig.frame == counter + 1);
    CHECK(l.fake.Balanced() && !l.fake.HoldsCopy());
}

void APreviewNeverBinds()
{
    Loop l;
    if (!l.Create())
        return;
    CHECK(l.Frame() == Outcome::NoCapture);
    l.rig.params.uints["OutWidth"] = 576; // the character preview's evaluate
    l.rig.params.uints["OutHeight"] = 960;
    for (int i = 0; i < 3; ++i)
        CHECK(l.Frame() == Outcome::NoCapture);
    CHECK(l.rig.sink.events.empty());
    CHECK(l.binder.GetCounters().passesBound == 0);
    l.rig.params.uints["OutWidth"] = evaltest::kW; // the real one is back
    l.rig.params.uints["OutHeight"] = evaltest::kH;
    CHECK(l.Frame() == Outcome::Bound);
}

void TheBackBufferIsTheOutputSize()
{
    { // a back buffer that is not the NGX Output size: the engine does not capture for it, so it never binds
        Loop l;
        if (!l.Create())
            return;
        l.fake.bbW = evaltest::kW * 2;
        l.fake.bbH = evaltest::kH * 2;
        for (int i = 0; i < 4; ++i)
            CHECK(l.Frame() == Outcome::NoCapture);
        CHECK(l.rig.sink.events.empty());
        CHECK(l.binder.GetCounters().passesBound == 0);
    }
    { // the back buffer changes between the evaluate and the begin (a resize): the binder's own size gate says no
        Loop l;
        if (!l.Create())
            return;
        CHECK(l.Frame() == Outcome::NoCapture);
        CHECK(l.Frame() == Outcome::Bound);
        l.Tick();
        l.Eval();
        l.fake.bbW = evaltest::kW * 2;
        l.fake.bbH = evaltest::kH * 2;
        CHECK(l.Begin() == Outcome::SizeMismatch);
        l.Finish();
        CHECK(l.binder.GetCounters().sizeMismatches == 1);
        CHECK(l.fake.DepthIs(kOrig, kOrigSrgb) && l.fake.Balanced());
    }
}

void AnotherDeviceNeverBinds()
{
    Loop l;
    if (!l.Create())
        return;
    CHECK(l.Frame() == Outcome::NoCapture);
    CHECK(l.Frame() == Outcome::Bound);
    l.fake.device = 0xBAD; // the runtime lives on a device that is not the one the copy was made on
    CHECK(l.Frame() == Outcome::OtherDevice);
    CHECK(l.binder.GetCounters().otherDevice == 1);
    l.fake.device = reinterpret_cast<uint64_t>(l.rig.device.Get());
    CHECK(l.Frame() == Outcome::Bound);
}

void ARefusedCaptureDoesNotMoveTheCounter()
{
    Loop l;
    if (!l.Create(D3D11_BIND_DEPTH_STENCIL)) // no shader resource bind flag: the pass refuses every capture
        return;
    CHECK(l.Frame() == Outcome::NoCapture);
    for (int i = 0; i < 3; ++i)
        CHECK(l.Frame() == Outcome::NoCapture);
    CHECK(l.rig.engine->Stats().refused == 3 && l.rig.engine->Stats().captured == 0);
    CHECK(l.rig.frame == l.startFrame); // nothing was published, nothing ticked
    CHECK(l.binder.GetCounters().passesBound == 0);
    CHECK(l.fake.DepthIs(kOrig, kOrigSrgb) && l.fake.Balanced());
}
} // namespace

int main()
{
    {
        Loop probe;
        if (!probe.Create())
        {
            std::puts("SKIP: no WARP D3D11 device");
            return 0;
        }
    }
    FullCycle();
    ManyPassesPerEvaluate();
    ACaptureThatStopsGoesStaleByAge();
    APreviewNeverBinds();
    TheBackBufferIsTheOutputSize();
    AnotherDeviceNeverBinds();
    ARefusedCaptureDoesNotMoveTheCounter();
    if (g_fail != 0)
    {
        std::printf("reshade_depth_loop: %d check(s) failed\n", g_fail);
        return 1;
    }
    std::puts("reshade_depth_loop: all checks passed");
    return 0;
}
