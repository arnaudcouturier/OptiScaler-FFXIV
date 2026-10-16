// S10 D0: the armed one-frame probe as a state machine, driven by a fake ReShade/D3D11 backend: happy path for both skip
// methods, the verdict lines, every refusal and failure path, the exception latch, the arm and await timeouts, the
// destroy-while-awaiting case, and that an unarmed ProbeAtBoundary costs nothing (the backend is never called).
#include "ext/reshade/ReShadeProbe.h"

#include <cassert>
#include <cstdio>
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
};

struct FakeBackend final : IBackend
{
    Session* session = nullptr;
    int64_t now = 0;
    // knobs
    bool effectsEnabled = true;
    Api api = Api::D3D11;
    uint64_t pickResult = 0x1000; // 0 = none
    bool boundaryOk = true;
    bool callOk = true;
    bool callException = false;
    bool stateRestored = true;
    bool firePassInsideCall = true;
    bool diffValid = true;
    double changedFraction = 0.4;
    bool effectsState = true;
    bool depthSizeMatches = true;
    // counters
    int renderCalls = 0;
    int inspectCalls = 0;
    std::vector<bool> setStates;

    int64_t Qpc() override { return now += 10; }
    int64_t QpcFrequency() override { return 1'000'000; }
    bool Describe(uint64_t id, bool withTechniques, RuntimeInfo& out) override
    {
        out = {};
        out.id = id;
        out.api = api;
        out.effectsEnabled = effectsEnabled;
        out.backBufferWidth = 3840;
        out.backBufferHeight = 2160;
        out.techniquesKnown = withTechniques;
        if (withTechniques)
            out.techniques = { { "Tonemap", "T.fx", true } };
        return true;
    }
    uint64_t PickRuntime(void*, const std::vector<uint64_t>& alive, std::string& why) override
    {
        why = alive.empty() ? "none" : "the only D3D11 runtime";
        return pickResult == 0 ? 0 : alive.front();
    }
    BoundaryInfo Inspect(void* ctx, uint64_t) override
    {
        inspectCalls++;
        BoundaryInfo b;
        b.ok = boundaryOk;
        b.refusal = boundaryOk ? "" : "size mismatch";
        b.context = reinterpret_cast<uint64_t>(ctx);
        b.width = 3840;
        b.height = 2160;
        b.format = 28;
        b.samples = 1;
        b.arraySize = 1;
        b.rtv = 0x20;
        b.rtvSrgb = 0x30;
        return b;
    }
    DepthReport QueryDepth(uint64_t, const BoundaryInfo&) override
    {
        DepthReport d;
        d.anyDepthBound = true;
        d.sizeMatchesBackBuffer = depthSizeMatches;
        d.summary = "DepthBufferTex -> 3840x2160 format 40 (back buffer 3840x2160)";
        d.lines.push_back("[F4] DepthBufferTex bound to a 3840x2160 d32_float resource");
        return d;
    }
    void SnapshotBefore(void*, const BoundaryInfo&) override {}
    PixelDiff SnapshotAfterAndDiff(void*, const BoundaryInfo&) override
    {
        PixelDiff d;
        d.valid = diffValid;
        d.note = diffValid ? "every 64th pixel" : "staging copy failed";
        d.changedFraction = changedFraction;
        d.meanAbs = 3.5;
        d.maxDelta = 40;
        d.sampled = 1000;
        d.readbackUs = 100;
        return d;
    }
    CallOutcome RenderEffects(void*, uint64_t runtime, const BoundaryInfo&) override
    {
        renderCalls++;
        CallOutcome o;
        o.ok = callOk && !callException;
        o.exception = callException;
        o.code = callException ? 0xC0000005u : 0u;
        o.what = callException ? "access violation" : (callOk ? "" : "command list mismatch");
        o.stateRestored = stateRestored;
        o.stateNote = "full D3D11 state block";
        if (firePassInsideCall && session != nullptr && o.ok)
        {
            EffectsEvent e;
            e.runtime = runtime;
            e.begin = true;
            e.qpc = 1000;
            e.fromProbe = InProbeCall(); // what the live callbacks do
            session->OnEffects(e);
            e.begin = false;
            e.qpc = 1400;
            session->OnEffects(e);
        }
        return o;
    }
    bool GetEffectsState(uint64_t, bool& enabled) override
    {
        enabled = effectsState;
        return true;
    }
    bool SetEffectsState(uint64_t, bool enabled) override
    {
        setStates.push_back(enabled);
        effectsState = enabled;
        return true;
    }
};

struct Rig
{
    FakeBackend be;
    CollectReport rep;
    Session s { be, rep };
    uint64_t rt = 0x1000;
    Rig()
    {
        be.session = &s;
        s.Begin("registration: OK");
        s.OnInitRuntime(rt);
        s.OnReloadedEffects(rt);
        s.OnReshadePresent(rt); // a first normal frame: events are flowing
    }
    bool nullRtv = false; // the end-of-frame pass is a uniforms-only call (rtv = 0)
    void NormalFrame(bool endOfFramePass)
    {
        if (endOfFramePass)
        {
            EffectsEvent e;
            e.runtime = rt;
            e.begin = true;
            e.qpc = 5000;
            e.rtv = nullRtv ? 0 : 0x20;
            s.OnEffects(e);
            e.begin = false;
            e.qpc = 5300;
            s.OnEffects(e);
        }
        s.OnReshadePresent(rt);
    }
};

void TestHappyPathMethod1()
{
    Rig r;
    assert(!r.s.Armed());
    r.s.ProbeAtBoundary(nullptr); // not armed: nothing happens, backend untouched
    assert(r.be.inspectCalls == 0 && r.be.renderCalls == 0);

    std::string why;
    assert(r.s.Arm(SkipMethod::RenderEffectsOnly, &why));
    assert(r.s.Armed());
    assert(!r.s.Arm(SkipMethod::RenderEffectsOnly, &why) && !why.empty()); // already armed

    r.s.ProbeAtBoundary(reinterpret_cast<void*>(0xC0));
    assert(r.be.renderCalls == 1 && !r.s.Armed());
    assert(r.s.ProbeStatusCopy().state == Session::ProbeState::AwaitFrameEnd);
    assert(r.rep.Has("passes fired inside the call: begin=1 finish=1"));
    assert(r.rep.Has("changed 40.00%"));
    assert(r.rep.Has("VERDICT F2: render_effects returned normally in place"));
    assert(r.rep.Has("VERDICT F4: a depth-format texture of the back buffer size is bound: DepthBufferTex"));
    assert(r.rep.Has("VERDICT F5: render_effects took"));
    assert(r.rep.Has("pipeline state restored: yes"));

    r.NormalFrame(false); // no end-of-frame pass after the probe: skipped
    assert(r.s.ProbeStatusCopy().state == Session::ProbeState::Idle);
    assert(r.rep.Has("VERDICT F3 (method: render_effects only (documented implicit skip)): ReShade's own end-of-frame effect pass was SKIPPED"));
    assert(r.be.setStates.empty()); // method 1 never touches the effects state
    assert(r.s.ProbeStatusCopy().lastResult.find("skipped") != std::string::npos);
    assert(r.s.ProbeStatusCopy().runs == 1);

    // run twice: a second arm works and appends
    assert(r.s.Arm(SkipMethod::RenderEffectsOnly));
    r.s.ProbeAtBoundary(nullptr);
    r.NormalFrame(true); // this time the end-of-frame pass ran as well
    assert(r.rep.Has("STILL RAN"));
    assert(r.rep.Has("rtv 0x20: effects would be applied twice"));
    assert(r.rep.Has("try the other skip method"));
    assert(r.s.ProbeStatusCopy().runs == 2);
}

void TestUniformsOnlyAndDepthSize()
{
    Rig r; // the end-of-frame pass after the probe is a uniforms-only call: not "applied twice"
    r.nullRtv = true;
    r.be.depthSizeMatches = false;
    r.s.Arm(SkipMethod::RenderEffectsOnly);
    r.s.ProbeAtBoundary(nullptr);
    assert(r.rep.Has("VERDICT F4: a depth-format texture is bound but its size differs from the back buffer"));
    r.NormalFrame(true);
    assert(r.rep.Has("ran with a NULL render target view"));
    assert(r.rep.Has("uniform update only"));
    assert(!r.rep.Has("STILL RAN"));
}

void TestMethod2RestoresState()
{
    Rig r;
    assert(r.s.Arm(SkipMethod::AlsoDisableEffects));
    r.s.ProbeAtBoundary(nullptr);
    assert(r.be.setStates.size() == 1 && r.be.setStates[0] == false && r.be.effectsState == false);
    assert(r.rep.Has("set_effects_state(false) for the rest of the frame: applied"));

    r.NormalFrame(false);
    assert(r.be.setStates.size() == 2 && r.be.setStates[1] == true && r.be.effectsState == true);
    assert(r.rep.Has("effects state restored to enabled: verified"));
    assert(r.rep.Has("was SKIPPED"));
    assert(r.s.ProbeStatusCopy().state == Session::ProbeState::Idle);
}

void TestRefusals()
{
    {
        Rig r; // effects disabled in ReShade
        r.be.effectsEnabled = false;
        r.s.Arm(SkipMethod::RenderEffectsOnly);
        r.s.ProbeAtBoundary(nullptr);
        assert(r.be.renderCalls == 0 && r.rep.Has("effects are disabled") && !r.s.ProbeStatusCopy().inert);
        assert(r.s.ProbeStatusCopy().state == Session::ProbeState::Idle);
        assert(r.s.ProbeStatusCopy().lastResult.find("FAILED") != std::string::npos);
    }
    {
        Rig r; // not a D3D11 runtime
        r.be.api = Api::D3D12;
        r.s.Arm(SkipMethod::RenderEffectsOnly);
        r.s.ProbeAtBoundary(nullptr);
        assert(r.be.renderCalls == 0 && r.rep.Has("not D3D11"));
    }
    {
        Rig r; // no runtime for this context
        r.be.pickResult = 0;
        r.s.Arm(SkipMethod::RenderEffectsOnly);
        r.s.ProbeAtBoundary(nullptr);
        assert(r.be.renderCalls == 0 && r.rep.Has("no runtime matches"));
    }
    {
        Rig r; // boundary target refused
        r.be.boundaryOk = false;
        r.s.Arm(SkipMethod::RenderEffectsOnly);
        r.s.ProbeAtBoundary(nullptr);
        assert(r.be.renderCalls == 0 && r.rep.Has("boundary target refused: size mismatch"));
        assert(r.s.Arm(SkipMethod::RenderEffectsOnly)); // refusals do not latch
    }
    {
        Rig r; // render_effects not executed (e.g. command list mismatch): no latch
        r.be.callOk = false;
        r.s.Arm(SkipMethod::RenderEffectsOnly);
        r.s.ProbeAtBoundary(nullptr);
        assert(r.rep.Has("render_effects was not executed: command list mismatch"));
        assert(!r.s.ProbeStatusCopy().inert && r.s.ProbeStatusCopy().state == Session::ProbeState::Idle);
    }
    {
        // no event ever delivered: the explicit "registered but nothing arrives" warning
        FakeBackend be;
        CollectReport rep;
        Session s(be, rep);
        be.session = &s;
        s.Begin("registration: OK");
        s.Arm(SkipMethod::RenderEffectsOnly);
        s.ProbeAtBoundary(nullptr);
        assert(rep.Has("registered but ReShade delivered NO event at all"));
        assert(rep.Has("ext::game::OnD3D11DeviceHooked"));
        assert(be.renderCalls == 0);
    }
    {
        // events flow but no runtime is alive
        FakeBackend be;
        CollectReport rep;
        Session s(be, rep);
        be.session = &s;
        s.OnQueuePresent();
        s.Arm(SkipMethod::RenderEffectsOnly);
        s.ProbeAtBoundary(nullptr);
        assert(rep.Has("no live effect runtime"));
    }
}

void TestExceptionLatches()
{
    Rig r;
    r.be.callException = true;
    r.s.Arm(SkipMethod::RenderEffectsOnly);
    r.s.ProbeAtBoundary(nullptr);
    assert(r.rep.Has("EXCEPTION in render_effects: code 0xc0000005"));
    assert(r.rep.Has("latched INERT"));
    assert(r.s.ProbeStatusCopy().inert && r.s.ProbeStatusCopy().state == Session::ProbeState::Idle);
    std::string why;
    assert(!r.s.Arm(SkipMethod::RenderEffectsOnly, &why));
    assert(why.find("latched inert") != std::string::npos);
}

void TestTimeouts()
{
    {
        Rig r; // armed, ProbeAtBoundary never called (not wired)
        r.s.Arm(SkipMethod::RenderEffectsOnly);
        for (uint32_t i = 0; i < Session::kArmTimeoutFrames - 1; i++)
            r.NormalFrame(true);
        assert(r.s.Armed());
        r.NormalFrame(true);
        assert(!r.s.Armed() && r.s.ProbeStatusCopy().state == Session::ProbeState::Idle);
        assert(r.rep.Has("no boundary call within 240 ReShade frames"));
        assert(r.s.Arm(SkipMethod::RenderEffectsOnly)); // can arm again
        r.s.Disarm();
        assert(!r.s.Armed() && r.rep.Has("disarmed by the user"));
    }
    {
        Rig r; // frame end of the probed runtime never arrives (another runtime keeps presenting)
        r.s.OnInitRuntime(0x2000);
        r.s.Arm(SkipMethod::AlsoDisableEffects);
        r.s.ProbeAtBoundary(nullptr);
        assert(r.be.effectsState == false);
        for (uint32_t i = 0; i < Session::kAwaitTimeoutFrames; i++)
            r.s.OnReshadePresent(0x2000);
        assert(r.s.ProbeStatusCopy().state == Session::ProbeState::Idle);
        assert(r.be.effectsState == true); // restored when giving up
        assert(r.rep.Has("frame end not observed"));
    }
    {
        Rig r; // runtime destroyed while awaiting: no restore call on a dead runtime
        r.s.Arm(SkipMethod::AlsoDisableEffects);
        r.s.ProbeAtBoundary(nullptr);
        const size_t sets = r.be.setStates.size();
        r.s.OnDestroyRuntime(r.rt);
        assert(r.s.ProbeStatusCopy().state == Session::ProbeState::Idle);
        assert(r.be.setStates.size() == sets);
        assert(r.rep.Has("destroyed before its frame ended"));
    }
}
} // namespace

int main()
{
    TestHappyPathMethod1();
    TestUniformsOnlyAndDepthSize();
    TestMethod2RestoresState();
    TestRefusals();
    TestExceptionLatches();
    TestTimeouts();
    std::puts("PASS: probe state machine: both skip methods, verdicts, refusals, exception latch, timeouts, destroy while awaiting");
    return 0;
}
