// S10 E: the decision logic behind ext::reshade::ApplyPresetsInPlace, driven by a fake ReShade/D3D11 backend (no ReShade, no GPU).
#include "ext/reshade/ReShadeBoundaryCore.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace ext::reshade;
using namespace ext::reshade::boundary;

namespace
{
int g_fail = 0;
#define CHECK(c)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            std::printf("CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #c);                                           \
            ++g_fail;                                                                                                  \
        }                                                                                                              \
    } while (0)

struct Fake final : probe::IBackend
{
    int64_t now = 0;
    bool effectsOn = true;
    bool effectsStateReadable = true;
    uint32_t total = 565, enabled = 9;
    bool countKnown = true;
    bool inspectOk = true;
    uint64_t inspectResource = 0; // 0 = the target passed to Apply
    bool renderOk = true, renderException = false, restored = true;
    uint64_t pickResult = 0; // 0 = first alive
    bool pickNone = false;
    // counters
    int picks = 0, counts = 0, inspects = 0, renders = 0, ends = 0, held = 0;
    uint64_t lastRuntime = 0;

    int64_t Qpc() override { return now += 5; }
    int64_t QpcFrequency() override { return 1'000'000; }
    bool Describe(uint64_t, bool, probe::RuntimeInfo&) override { return false; }
    uint64_t PickRuntime(void*, const std::vector<uint64_t>& alive, std::string& why) override
    {
        ++picks;
        why = pickNone ? "none owns the context's device" : "owns the context's device";
        if (pickNone || alive.empty())
            return 0;
        return pickResult ? pickResult : alive.front();
    }
    probe::BoundaryInfo Inspect(void* ctx, uint64_t) override
    {
        ++inspects;
        ++held; // Inspect keeps views until EndBoundary
        probe::BoundaryInfo b;
        b.ok = inspectOk;
        b.refusal = inspectOk ? "" : "the target is 352x352 but the runtime's back buffer is 3840x2160";
        b.context = reinterpret_cast<uint64_t>(ctx);
        b.resource = inspectResource ? inspectResource : 0x7000;
        b.rtv = 0x20;
        b.rtvSrgb = 0x30;
        return b;
    }
    probe::DepthReport QueryDepth(uint64_t, const probe::BoundaryInfo&) override { return {}; }
    void SnapshotBefore(void*, const probe::BoundaryInfo&) override {}
    probe::PixelDiff SnapshotAfterAndDiff(void*, const probe::BoundaryInfo&) override { return {}; }
    probe::CallOutcome RenderEffects(void*, uint64_t runtime, const probe::BoundaryInfo&) override
    {
        ++renders;
        lastRuntime = runtime;
        probe::CallOutcome o;
        o.ok = renderOk && !renderException;
        o.exception = renderException;
        o.code = renderException ? 0xC0000005u : 0u;
        o.what = renderException ? "access violation" : (renderOk ? "" : "preconditions no longer hold");
        o.stateRestored = restored;
        o.stateNote = restored ? "ok" : "restore raised";
        return o;
    }
    bool GetEffectsState(uint64_t, bool& e) override
    {
        e = effectsOn;
        return effectsStateReadable;
    }
    bool SetEffectsState(uint64_t, bool) override { return true; }
    bool CountTechniques(uint64_t, uint32_t& t, uint32_t& e) override
    {
        ++counts;
        t = total;
        e = enabled;
        return countKnown;
    }
    void EndBoundary() override
    {
        ++ends;
        held = 0;
    }
};

void* const kCtx = reinterpret_cast<void*>(0x1000);
void* const kRt = reinterpret_cast<void*>(0x7000);
constexpr uint64_t kRuntime = 0x1f2321e1a60;

struct Rig
{
    Fake fake;
    Applier applier { fake };
    ApplyResult Apply(PresetsMode m = PresetsMode::Auto, bool registered = true) { return applier.Apply(m, registered, kCtx, kRt); }
    ApplyResult Predict(PresetsMode m = PresetsMode::Auto, bool registered = true, void* ctx = kCtx) { return applier.Predict(m, registered, ctx); }
    void Ready(uint64_t runtime = kRuntime)
    {
        applier.OnInitRuntime(runtime);
        applier.OnReloadedEffects(runtime);
    }
};

void Parser()
{
    CHECK(ParsePresetsMode("auto") == PresetsMode::Auto);
    CHECK(ParsePresetsMode("") == PresetsMode::Auto);
    CHECK(ParsePresetsMode("AUTO") == PresetsMode::Auto);
    CHECK(ParsePresetsMode("garbage") == PresetsMode::Auto);
    CHECK(ParsePresetsMode("on") == PresetsMode::On);
    CHECK(ParsePresetsMode(" On ") == PresetsMode::On);
    CHECK(ParsePresetsMode("true") == PresetsMode::On);
    CHECK(ParsePresetsMode("off") == PresetsMode::Off);
    CHECK(ParsePresetsMode("OFF") == PresetsMode::Off);
    CHECK(ParsePresetsMode("false") == PresetsMode::Off);
    CHECK(ParsePresetsMode("0") == PresetsMode::Off);
    CHECK(std::string(PresetsModeName(PresetsMode::On)) == "on");
    CHECK(std::string(ApplyResultName(ApplyResult::Failed)) == "failed");
}

void Selection()
{
    { // off: no call at all, even with a ready runtime
        Rig r;
        r.Ready();
        CHECK(r.Apply(PresetsMode::Off) == ApplyResult::NotEnabled);
        CHECK(r.fake.renders == 0 && r.fake.inspects == 0 && r.fake.picks == 0);
    }
    { // registration failed / ReShade absent
        Rig r;
        r.Ready();
        CHECK(r.Apply(PresetsMode::Auto, false) == ApplyResult::NoReShade);
        CHECK(r.fake.renders == 0 && r.fake.inspects == 0);
    }
    { // registered, but no runtime was ever created / the runtime was destroyed
        Rig r;
        CHECK(r.Apply() == ApplyResult::NoRuntime);
        r.Ready();
        r.applier.OnDestroyRuntime(kRuntime);
        CHECK(r.Apply() == ApplyResult::NoRuntime);
        CHECK(r.fake.renders == 0);
        CHECK(r.applier.AliveRuntimes() == 0);
    }
    { // the backend finds no runtime for this context
        Rig r;
        r.Ready();
        r.fake.pickNone = true;
        CHECK(r.Apply() == ApplyResult::NoRuntime);
        CHECK(r.applier.GetStats().lastDetail.find("none owns") != std::string::npos);
        const int picks = r.fake.picks;
        CHECK(r.Apply() == ApplyResult::NoRuntime);
        CHECK(r.fake.picks == picks); // the negative choice is cached for this generation
    }
    { // effects loaded? not yet: nothing to apply, nothing called
        Rig r;
        r.applier.OnInitRuntime(kRuntime);
        CHECK(r.Apply() == ApplyResult::NothingToApply);
        CHECK(r.fake.renders == 0 && r.fake.inspects == 0);
        r.applier.OnReloadedEffects(kRuntime);
        CHECK(r.Apply() == ApplyResult::Applied);
    }
    { // effects disabled by the user
        Rig r;
        r.Ready();
        r.fake.effectsOn = false;
        CHECK(r.Apply() == ApplyResult::NothingToApply);
        CHECK(r.fake.renders == 0 && r.fake.inspects == 0);
        r.fake.effectsOn = true;
        CHECK(r.Apply() == ApplyResult::Applied);
    }
    { // zero enabled techniques: auto skips (ReShade's own pass stays), on applies anyway
        Rig r;
        r.Ready();
        r.fake.enabled = 0;
        CHECK(r.Apply(PresetsMode::Auto) == ApplyResult::NothingToApply);
        CHECK(r.fake.renders == 0 && r.fake.inspects == 0);
        r.applier.FrameEnded();
        CHECK(r.Apply(PresetsMode::On) == ApplyResult::Applied);
        CHECK(r.fake.renders == 1);
    }
    { // the happy path, and the state of the fake after it
        Rig r;
        r.Ready();
        CHECK(r.Apply() == ApplyResult::Applied);
        CHECK(r.fake.renders == 1 && r.fake.lastRuntime == kRuntime);
        CHECK(r.fake.held == 0 && r.fake.ends == 1); // no render target view stays referenced
        const auto s = r.applier.GetStats();
        CHECK(s.calls == 1 && s.results[size_t(ApplyResult::Applied)] == 1 && !s.inert);
        CHECK(s.callUsMax > 0);
    }
}

void OncePerFrame()
{
    Rig r;
    r.Ready();
    CHECK(r.Apply() == ApplyResult::Applied);
    CHECK(r.Apply() == ApplyResult::NothingToApply); // second call in the same frame: no ReShade call
    CHECK(r.Apply() == ApplyResult::NothingToApply);
    CHECK(r.fake.renders == 1);
    r.applier.FrameEnded();
    CHECK(r.Apply() == ApplyResult::Applied);
    CHECK(r.fake.renders == 2);
}

void RuntimeRecreated()
{
    Rig r;
    r.Ready();
    CHECK(r.Apply() == ApplyResult::Applied);
    r.applier.FrameEnded();
    const int picks = r.fake.picks, counts = r.fake.counts;
    CHECK(r.Apply() == ApplyResult::Applied); // cached: no new pick and no recount
    CHECK(r.fake.picks == picks && r.fake.counts == counts);
    r.applier.FrameEnded();

    // ResizeBuffers: ReShade destroys the runtime and creates it again with the SAME pointer value
    r.applier.OnDestroyRuntime(kRuntime);
    CHECK(r.Apply() == ApplyResult::NoRuntime);
    r.applier.OnInitRuntime(kRuntime);
    CHECK(r.Apply() == ApplyResult::NothingToApply); // initialised, effects not (re)loaded yet
    r.applier.OnReloadedEffects(kRuntime);
    CHECK(r.Apply() == ApplyResult::Applied);
    CHECK(r.fake.picks > picks && r.fake.counts > counts); // chosen and counted again for the new generation
    CHECK(r.applier.AliveRuntimes() == 1);
    CHECK(r.applier.GetStats().runtimeEvents == 3);
}

void TechniqueCount()
{
    Rig r;
    r.Ready();
    r.fake.enabled = 0;
    CHECK(r.Apply() == ApplyResult::NothingToApply);
    CHECK(r.fake.counts == 1);
    CHECK(r.Apply() == ApplyResult::NothingToApply);
    CHECK(r.fake.counts == 1); // not recounted every call
    r.fake.enabled = 4;
    CHECK(r.Apply() == ApplyResult::NothingToApply); // the change is only seen after an event
    r.applier.OnTechniquesChanged();                 // preset switched / technique toggled
    CHECK(r.Apply() == ApplyResult::Applied);
    CHECK(r.fake.counts == 2);

    // the safety recount
    Rig q;
    q.Ready();
    q.fake.enabled = 0;
    CHECK(q.Apply() == ApplyResult::NothingToApply);
    q.fake.enabled = 3;
    ApplyResult last = ApplyResult::NothingToApply;
    for (uint32_t i = 0; i < Applier::kRecountEvery + 2 && last != ApplyResult::Applied; ++i)
        last = q.Apply();
    CHECK(last == ApplyResult::Applied);

    // unknown count: do not block the presets
    Rig u;
    u.Ready();
    u.fake.countKnown = false;
    CHECK(u.Apply() == ApplyResult::Applied);
}

void Failures()
{
    { // boundary refused
        Rig r;
        r.Ready();
        r.fake.inspectOk = false;
        CHECK(r.Apply() == ApplyResult::Failed);
        CHECK(r.fake.renders == 0 && r.fake.held == 0);
        CHECK(r.applier.GetStats().lastDetail.find("352x352") != std::string::npos);
        r.fake.inspectOk = true;
        CHECK(r.Apply() == ApplyResult::Applied); // a refusal does not latch
    }
    { // the bound render target is not the one the caller means
        Rig r;
        r.Ready();
        r.fake.inspectResource = 0x9999;
        CHECK(r.Apply() == ApplyResult::Failed);
        CHECK(r.fake.renders == 0 && r.fake.held == 0);
    }
    { // preconditions no longer hold inside the backend
        Rig r;
        r.Ready();
        r.fake.renderOk = false;
        CHECK(r.Apply() == ApplyResult::Failed);
        CHECK(r.fake.held == 0);
        r.fake.renderOk = true;
        CHECK(r.Apply() == ApplyResult::Applied);
    }
    { // an exception inside the ReShade call latches the live path for the process
        Rig r;
        r.Ready();
        r.fake.renderException = true;
        CHECK(r.Apply() == ApplyResult::Failed);
        CHECK(r.fake.held == 0);
        CHECK(r.applier.GetStats().inert);
        r.fake.renderException = false;
        CHECK(r.Apply() == ApplyResult::Failed);
        CHECK(r.fake.renders == 1); // not called again
    }
    { // the effects ran but the state could not be restored: Applied for this frame, never again
        Rig r;
        r.Ready();
        r.fake.restored = false;
        CHECK(r.Apply() == ApplyResult::Applied);
        CHECK(r.applier.GetStats().inert);
        r.applier.FrameEnded();
        CHECK(r.Apply() == ApplyResult::Failed);
        CHECK(r.fake.renders == 1);
    }
    { // the effects state cannot be read
        Rig r;
        r.Ready();
        r.fake.effectsStateReadable = false;
        CHECK(r.Apply() == ApplyResult::Failed);
    }
}

// The early UI anchor asks before a draw whose bound target is not the one the effects would run on: Predict must return what Apply would
// return at that point (Applied = it would call render_effects), without inspecting the target, calling ReShade or counting a call.
void Prediction()
{
    // Predict, then check it left no trace: no Inspect, no render_effects, no call or result counted
    auto predictClean = [](Rig& r, ApplyResult expected, PresetsMode m = PresetsMode::Auto, bool registered = true, void* ctx = kCtx)
    {
        const ApplyStats before = r.applier.GetStats();
        const int inspects = r.fake.inspects, renders = r.fake.renders;
        const ApplyResult p = r.Predict(m, registered, ctx);
        CHECK(p == expected);
        const ApplyStats after = r.applier.GetStats();
        CHECK(after.calls == before.calls);
        for (size_t i = 0; i < kApplyResults; ++i)
            CHECK(after.results[i] == before.results[i]);
        CHECK(r.fake.inspects == inspects && r.fake.renders == renders && r.fake.held == 0);
        return p;
    };

    { // off, not registered: the same results as Apply, before any lock or backend call
        Rig r;
        r.Ready();
        predictClean(r, ApplyResult::NotEnabled, PresetsMode::Off);
        CHECK(r.Apply(PresetsMode::Off) == ApplyResult::NotEnabled);
        predictClean(r, ApplyResult::NoReShade, PresetsMode::Auto, false);
        CHECK(r.Apply(PresetsMode::Auto, false) == ApplyResult::NoReShade);
    }
    { // no runtime, and a runtime that does not own the context
        Rig r;
        predictClean(r, ApplyResult::NoRuntime);
        CHECK(r.Apply() == ApplyResult::NoRuntime);
        Rig q;
        q.Ready();
        q.fake.pickNone = true;
        predictClean(q, ApplyResult::NoRuntime);
        CHECK(q.Apply() == ApplyResult::NoRuntime);
    }
    { // effects disabled, effects not loaded yet: nothing to apply, and the reason is the detail text
        Rig r;
        r.Ready();
        r.fake.effectsOn = false;
        predictClean(r, ApplyResult::NothingToApply);
        CHECK(r.applier.GetStats().lastDetail == "ReShade effects are disabled");
        CHECK(r.Apply() == ApplyResult::NothingToApply);
        Rig q;
        q.applier.OnInitRuntime(kRuntime);
        predictClean(q, ApplyResult::NothingToApply);
        CHECK(q.Apply() == ApplyResult::NothingToApply);
    }
    { // zero enabled techniques: nothing under auto, a call under on (the count is not trusted there)
        Rig r;
        r.Ready();
        r.fake.enabled = 0;
        predictClean(r, ApplyResult::NothingToApply, PresetsMode::Auto);
        CHECK(r.Apply(PresetsMode::Auto) == ApplyResult::NothingToApply);
        predictClean(r, ApplyResult::Applied, PresetsMode::On);
        CHECK(r.Apply(PresetsMode::On) == ApplyResult::Applied);
    }
    { // enabled techniques: Applied, and the real call still happens once
        Rig r;
        r.Ready();
        predictClean(r, ApplyResult::Applied);
        predictClean(r, ApplyResult::Applied); // asking twice changes nothing
        CHECK(r.Apply() == ApplyResult::Applied);
        CHECK(r.fake.renders == 1);
        CHECK(r.applier.GetStats().calls == 1);
        predictClean(r, ApplyResult::NothingToApply); // already applied this frame, as a second Apply would say
        r.applier.FrameEnded();
        predictClean(r, ApplyResult::Applied);
    }
    { // the bound target is not inspected: a refusal only the call would see still predicts a call
        Rig r;
        r.Ready();
        r.fake.inspectOk = false;
        predictClean(r, ApplyResult::Applied);
        CHECK(r.Apply() == ApplyResult::Failed);
    }
    { // failures before the call: unreadable effects state, no context, latched inert
        Rig r;
        r.Ready();
        r.fake.effectsStateReadable = false;
        predictClean(r, ApplyResult::Failed);
        CHECK(r.Apply() == ApplyResult::Failed);
        Rig q;
        q.Ready();
        predictClean(q, ApplyResult::Failed, PresetsMode::Auto, true, nullptr);
        Rig u;
        u.Ready();
        u.fake.renderException = true;
        CHECK(u.Apply() == ApplyResult::Failed);
        CHECK(u.applier.GetStats().inert);
        predictClean(u, ApplyResult::Failed);
    }
}
} // namespace

int main()
{
    Parser();
    Selection();
    OncePerFrame();
    RuntimeRecreated();
    TechniqueCount();
    Failures();
    Prediction();
    if (g_fail)
    {
        std::printf("reshade_boundary_apply: %d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("reshade_boundary_apply: all checks passed\n");
    return 0;
}
