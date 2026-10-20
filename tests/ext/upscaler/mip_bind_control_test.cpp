// [MIP-BIND] The control logic between the game's events and the engine (ext/upscaler/MipBindControl.h): OnSamplerCreated,
// OnEvaluate and the install plan, on real WARP samplers and immediate contexts with a log collector.
#include "ext/upscaler/MipBindControl.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace ext::upscaler::mipbind;

static int g_failed = 0;

#define CHECK(cond)                                                                                              \
    do                                                                                                           \
    {                                                                                                            \
        if (!(cond))                                                                                             \
        {                                                                                                        \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);                                                  \
            ++g_failed;                                                                                          \
        }                                                                                                        \
    } while (0)

static bool Near(double a, double b, double tolerance = 1e-5) { return std::fabs(a - b) <= tolerance; }

static std::vector<std::pair<LogLevel, std::string>> g_log;
static void Sink(LogLevel level, const std::string& text) { g_log.emplace_back(level, text); }
static int CountLog(const char* part, int level = -1)
{
    int n = 0;
    for (const auto& l : g_log)
        if (l.second.find(part) != std::string::npos && (level < 0 || static_cast<int>(l.first) == level))
            ++n;
    return n;
}

static D3D11_SAMPLER_DESC Desc(float bias, D3D11_FILTER filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR, UINT aniso = 1)
{
    D3D11_SAMPLER_DESC d {};
    d.Filter = filter;
    d.AddressU = d.AddressV = d.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    d.MipLODBias = bias;
    d.MaxAnisotropy = aniso;
    d.ComparisonFunc = D3D11_COMPARISON_NEVER;
    d.MaxLOD = D3D11_FLOAT32_MAX;
    return d;
}

static float BiasOf(ID3D11SamplerState* s)
{
    D3D11_SAMPLER_DESC d {};
    s->GetDesc(&d);
    return d.MipLODBias;
}

struct Rig
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    std::array<SetSamplersFn, kStages> set {};
    std::array<GetSamplersFn, kStages> get {};
    Engine engine;
    Controller controller { engine, &Sink };

    bool Init()
    {
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                                     nullptr, &context)))
            return false;
        void** ctxVt = *reinterpret_cast<void***>(context.Get());
        void** devVt = *reinterpret_cast<void***>(device.Get());
        for (unsigned s = 0; s < kStages; ++s)
        {
            set[s] = reinterpret_cast<SetSamplersFn>(ctxVt[kContextSetSamplersSlots[s]]);
            get[s] = reinterpret_cast<GetSamplersFn>(ctxVt[kContextGetSamplersSlots[s]]);
        }
        engine.SetCreateFn(reinterpret_cast<CreateSamplerFn>(devVt[kDeviceCreateSamplerStateSlot]));
        engine.Attach(context.Get(), set, get);
        return true;
    }

    // A sampler the game makes: through the device, then the create hook's controller entry.
    ComPtr<ID3D11SamplerState> Game(float bias, D3D11_FILTER f = D3D11_FILTER_MIN_MAG_MIP_LINEAR, UINT aniso = 1, bool bypassed = false)
    {
        ComPtr<ID3D11SamplerState> s;
        const auto d = Desc(bias, f, aniso);
        CHECK(SUCCEEDED(device->CreateSamplerState(&d, &s)));
        controller.OnSamplerCreated(s.Get(), device.Get(), d, bypassed);
        return s;
    }

    void Bind(unsigned stage, UINT slot, ID3D11SamplerState* s) { engine.Set(stage, context.Get(), slot, 1, &s); }

    ComPtr<ID3D11SamplerState> Bound(unsigned stage, UINT slot)
    {
        ComPtr<ID3D11SamplerState> s;
        get[stage](context.Get(), slot, 1, s.GetAddressOf());
        return s;
    }

    // What is bound in `slot` of the pixel stage, as a bias (NaN when nothing is bound).
    double BoundBias(UINT slot)
    {
        auto s = Bound(0, slot);
        return s ? BiasOf(s.Get()) : std::nan("");
    }
};

static Settings On(double offset = 0.0)
{
    Settings s;
    s.autoEnabled = true;
    s.offset = offset;
    return s;
}

static EvalInput Eval(unsigned render, unsigned output, std::uint64_t now)
{
    EvalInput e;
    e.renderW = render;
    e.outputW = output;
    e.nowMs = now;
    return e;
}

// The whole flow with the numbers of the logged FFXIV sessions and his offset of -0.18. Samplers: the tracking family T (-1 at DLAA,
// -1.5625 at Quality) and an art sampler A (-0.5).
static void FullFlowWithReplace()
{
    g_log.clear();
    Rig r;
    CHECK(r.Init());
    const double kQ = -0.584963;

    // login screen, native resolution: the first evaluate sets the target 0 + offset
    r.controller.OnEvaluate(On(-0.18), Eval(3840, 3840, 1000));
    CHECK(r.engine.Active() && r.engine.HasTarget() && Near(r.engine.Base(), 0.0) && r.engine.CurrentMode() == Mode::Add);
    CHECK(CountLog("first measurement") == 1 && CountLog("target -0.1800") == 1);

    // the game makes its samplers at DLAA and binds them
    auto stale = r.Game(-1.0f);
    auto art = r.Game(-0.5f, D3D11_FILTER_ANISOTROPIC, 8); // another family (anisotropic)
    r.controller.OnEvaluate(On(-0.18), Eval(3840, 3840, 1100)); // the evaluate seam services the new samplers
    r.Bind(0, 0, stale.Get());
    r.Bind(0, 1, art.Get());
    CHECK(Near(r.BoundBias(0), -1.18) && Near(r.BoundBias(1), -0.68)); // ADD until a pair is seen: game + target (0 + offset)

    // the quality preset moves to Quality: ADD still (no evidence yet)
    r.controller.OnEvaluate(On(-0.18), Eval(2560, 3840, 2000));
    CHECK(Near(r.engine.Base(), kQ) && CountLog("target -0.7650") == 1);
    CHECK(Near(r.BoundBias(0), -1.764963) && Near(r.BoundBias(1), -1.264963));

    // the game re-creates its tracking sampler at Quality with -1.5625: the next evaluate sees the pair, REPLACE (provisional)
    auto fresh = r.Game(-1.5625f);
    CHECK(Near(r.engine.CreateBase(), kQ));
    r.controller.OnEvaluate(On(-0.18), Eval(2560, 3840, 2300));
    CHECK(r.engine.CurrentMode() == Mode::Replace && CountLog("provisional") == 1 && CountLog("replace mode") == 1);
    r.Bind(0, 2, fresh.Get());
    // REPLACE: only the change since the sampler was made, plus the offset: the stale -1 gets the game's new value (+ offset),
    // the one made at Quality is the game's own (+ offset), the art sampler is shifted like the manual add
    CHECK(Near(r.BoundBias(0), -1.764963) && Near(r.BoundBias(2), -1.7425) && Near(r.BoundBias(1), -1.264963));

    // back to DLAA: the tracking family returns to its DLAA value (+ offset), nothing is 0
    r.controller.OnEvaluate(On(-0.18), Eval(3840, 3840, 5000));
    CHECK(Near(r.engine.Base(), 0.0));
    CHECK(Near(r.BoundBias(0), -1.18) && Near(r.BoundBias(2), -1.1575, 1e-4) && Near(r.BoundBias(1), -0.68));
    // the game reads back its own samplers all along
    ComPtr<ID3D11SamplerState> back[3];
    r.engine.Get(0, r.context.Get(), 0, 3, reinterpret_cast<ID3D11SamplerState**>(&back));
    CHECK(back[0].Get() == stale.Get() && back[1].Get() == art.Get() && back[2].Get() == fresh.Get());
}

// M2: a hand-set MipmapBiasOverride blocks the substitution, not the classification, and releasing it resumes in the same session.
static void ManualOverride()
{
    g_log.clear();
    Rig r;
    CHECK(r.Init());
    Settings manual = On();
    manual.manualOverride = true;

    auto early = r.Game(-1.0f);
    r.controller.OnEvaluate(manual, Eval(2560, 3840, 1000));
    CHECK(!r.engine.Active());
    CHECK(CountLog("set by hand", static_cast<int>(LogLevel::Warn)) == 1);
    CHECK(CountLog("press Reset") == 1 && CountLog("resumes at once") == 1); // the advice is true now: see below

    // samplers the game makes while the override is set are classified (their stored bias is the game's own)
    auto during = r.Game(-0.5f, D3D11_FILTER_ANISOTROPIC, 4);
    CHECK(r.engine.table().Find(during.Get()) != nullptr && r.engine.table().Find(during.Get())->eligible.load());
    CHECK(Near(r.engine.table().Find(during.Get())->original.load(), -0.5f));
    r.Bind(0, 0, early.Get());
    CHECK(r.Bound(0, 0).Get() == early.Get()); // nothing is substituted while the override wins
    for (int i = 0; i < 5; ++i) // one WARN for the whole episode
        r.controller.OnEvaluate(manual, Eval(2560, 3840, 1100 + 100ull * i));
    CHECK(CountLog("set by hand") == 1);

    // Reset: the override is empty, the very next evaluate substitutes (no restart, no new samplers needed)
    r.controller.OnEvaluate(On(), Eval(2560, 3840, 2000));
    CHECK(r.engine.Active() && Near(r.engine.Base(), -0.584963));
    r.Bind(0, 0, early.Get());
    r.Bind(0, 1, during.Get());
    CHECK(Near(r.BoundBias(0), -1.584963) && Near(r.BoundBias(1), -1.084963));

    // set by hand again: the game's samplers come back, the WARN comes once more
    r.controller.OnEvaluate(manual, Eval(2560, 3840, 3000));
    CHECK(!r.engine.Active() && r.Bound(0, 0).Get() == early.Get() && CountLog("a manual Mipmap Bias value is set") == 1);
    CHECK(CountLog("set by hand") == 2);

    // the setting off: no WARN, the samplers are the game's
    Settings off;
    off.autoEnabled = false;
    off.manualOverride = true;
    g_log.clear();
    r.controller.OnEvaluate(off, Eval(2560, 3840, 4000));
    CHECK(CountLog("set by hand") == 0);

    // the override cleared while the setting is off, then set again with the setting on: the WARN comes again (a new episode)
    Settings offClear;
    offClear.autoEnabled = false;
    r.controller.OnEvaluate(offClear, Eval(2560, 3840, 4500));
    r.controller.OnEvaluate(manual, Eval(2560, 3840, 5000));
    CHECK(CountLog("set by hand") == 1);}

// Duplicate creates, the fork's own samplers, address reuse and the full table.
static void CreatesAndTable()
{
    g_log.clear();
    Rig r;
    CHECK(r.Init());

    auto a = r.Game(-1.0f);
    for (int i = 0; i < 100; ++i)
        r.Game(-1.0f); // the runtime returns the existing object: no new entry
    CHECK(r.engine.table().Count() == 1);
    int drained = 0;
    r.engine.DrainNewEntries([&](const SamplerTable::Entry&) { ++drained; });
    CHECK(drained == 1);

    // the fork's own sampler (bypass scope) never takes part ...
    auto own = r.Game(-0.25f, D3D11_FILTER_MIN_MAG_MIP_LINEAR, 1, true);
    CHECK(r.engine.table().Find(own.Get()) == nullptr && r.engine.table().Count() == 1);
    // ... and one that is not eligible is not stored
    auto flat = r.Game(0.0f);
    auto cmp = r.Game(-1.0f, D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR);
    CHECK(r.engine.table().Count() == 1);
    // a known address made by the fork (bypassed) switches the entry off
    ComPtr<ID3D11SamplerState> again;
    const auto d = Desc(-1.0f);
    r.device->CreateSamplerState(&d, &again);
    CHECK(again.Get() == a.Get());
    r.controller.OnSamplerCreated(again.Get(), r.device.Get(), d, true);
    CHECK(!r.engine.table().Find(a.Get())->eligible.load());
    r.controller.OnSamplerCreated(again.Get(), r.device.Get(), d, false);
    CHECK(r.engine.table().Find(a.Get())->eligible.load());

    // the table fills at 256 distinct eligible samplers; one WARN, the rest keep the game's bias
    std::vector<ComPtr<ID3D11SamplerState>> many;
    for (int i = 0; i < 270; ++i)
        many.push_back(r.Game(-0.05f - 0.01f * static_cast<float>(i)));
    CHECK(r.engine.table().Count() == kTableCapacity);
    CHECK(CountLog("more than 256") == 1 && r.engine.counters().tableFull.load() >= 12);
}

// M5: a second feature at another size, noise and a fast stream of changes.
static void SteadyTarget()
{
    g_log.clear();
    Rig r;
    CHECK(r.Init());
    r.controller.OnEvaluate(On(), Eval(2560, 3840, 0));
    const auto rev = r.engine.Revision();
    for (int i = 1; i <= 400; ++i) // the main feature and a preview window alternate, every frame
    {
        const bool main = (i & 1) == 0;
        r.controller.OnEvaluate(On(), main ? Eval(2560, 3840, 16ull * i) : Eval(576, 1152, 16ull * i));
    }
    CHECK(r.engine.Revision() == rev && CountLog("target ") == 1);
    // noise in the render width (2558 for 2560)
    for (int i = 0; i < 50; ++i)
        r.controller.OnEvaluate(On(), Eval(2558 + (i & 3), 3840, 7000 + 16ull * i));
    CHECK(r.engine.Revision() == rev);
    // a real change is taken
    r.controller.OnEvaluate(On(), Eval(1920, 3840, 9000));
    CHECK(r.engine.Revision() != rev && CountLog("target ") == 2);
}

// M8: the hooks sit on another context than the one the game uses.
static void RelatchAndWatch()
{
    g_log.clear();
    Rig r;
    CHECK(r.Init());
    Rig other; // a second device: its immediate context is of the same class
    CHECK(other.Init());

    auto art = r.Game(-1.0f);
    r.controller.OnEvaluate(On(), Eval(2560, 3840, 0));
    ID3D11SamplerState* s = art.Get();
    r.engine.Set(0, r.context.Get(), 0, 1, &s);
    CHECK(r.engine.Latched() == r.context.Get());

    EvalInput move = Eval(2560, 3840, 100);
    move.relatchTo = other.context.Get();
    r.controller.OnEvaluate(On(), move);
    CHECK(r.engine.Latched() == other.context.Get() && r.engine.Active());
    CHECK(CountLog("moving the hooks", static_cast<int>(LogLevel::Warn)) == 1);
    ComPtr<ID3D11SamplerState> oldBound;
    r.get[0](r.context.Get(), 0, 1, oldBound.GetAddressOf());
    CHECK(oldBound.Get() != art.Get()); // the OLD context is left alone (it may belong to a device that is gone)

    // the latch watch: samplers known, substitution on, not one bind in 600 evaluates
    g_log.clear();
    Rig w;
    CHECK(w.Init());
    auto k = w.Game(-1.0f);
    for (unsigned i = 0; i < LatchWatch::kEvaluates + 50; ++i)
        w.controller.OnEvaluate(On(), Eval(2560, 3840, 1000 + 20ull * i));
    CHECK(CountLog("not one bind of one of them seen", static_cast<int>(LogLevel::Warn)) == 1);
    (void) k;
}

// Native stand-ins that record which context every call went to (M-E: the old context must not be called after a relatch).
static std::vector<ID3D11DeviceContext*> g_nativeCalls;
static void STDMETHODCALLTYPE CountingSet(ID3D11DeviceContext* c, UINT, UINT, ID3D11SamplerState* const*) { g_nativeCalls.push_back(c); }
static void STDMETHODCALLTYPE CountingGet(ID3D11DeviceContext* c, UINT, UINT, ID3D11SamplerState**) { g_nativeCalls.push_back(c); }

static void RelatchLeavesOldContextAlone()
{
    g_log.clear();
    Rig r;
    Rig other;
    CHECK(r.Init() && other.Init());
    std::array<SetSamplersFn, kStages> fs;
    std::array<GetSamplersFn, kStages> fg;
    fs.fill(&CountingSet);
    fg.fill(&CountingGet);
    r.engine.Attach(r.context.Get(), fs, fg);

    auto art = r.Game(-1.0f);
    r.controller.OnEvaluate(On(), Eval(2560, 3840, 0));
    CHECK(r.engine.Active() && !g_nativeCalls.empty());

    g_nativeCalls.clear();
    EvalInput move = Eval(2560, 3840, 100);
    move.relatchTo = other.context.Get();
    r.controller.OnEvaluate(On(), move);
    CHECK(r.engine.Latched() == other.context.Get());
    CHECK(!g_nativeCalls.empty()); // the new context was rebound ...
    for (auto* c : g_nativeCalls)
        CHECK(c == other.context.Get()); // ... and the old one was never called
    (void) art;
}

// L-5: at native resolution with the default offset 0 nothing has to change, so no bind is "substituted"; the latch watch must
// not take that for a wrong context as long as the game binds eligible samplers.
static void WatchAtNativeResolution()
{
    g_log.clear();
    Rig r;
    CHECK(r.Init());
    auto art = r.Game(-1.0f);
    for (unsigned i = 0; i < LatchWatch::kEvaluates + 100; ++i)
    {
        r.controller.OnEvaluate(On(0.0), Eval(3840, 3840, 100 + 20ull * i));
        if (i == 5)
            r.Bind(0, 0, art.Get()); // the game binds its sampler once (the record keeps it)
    }
    CHECK(r.engine.SubstitutedBinds() == 0 && r.engine.EligibleBinds() >= 1);
    CHECK(CountLog("not one bind of one of them seen") == 0);
}

// M-F: the address of a released game sampler comes back with the same description and another bias: the detector must see it.
static void ReCreateFeedsDetector()
{
    g_log.clear();
    Rig r;
    CHECK(r.Init());
    const double kQ = -0.584963;
    auto s = r.Game(-1.0f);
    r.controller.OnEvaluate(On(), Eval(3840, 3840, 1000)); // DLAA: the sampler is observed with -1.0 at base 0
    r.controller.OnEvaluate(On(), Eval(2560, 3840, 2000)); // Quality
    CHECK(Near(r.engine.Base(), kQ) && r.controller.detector().mode() == Mode::Add);

    // the game re-creates "the same" sampler (same description, the runtime returns the same object) with the Quality bias
    const auto d = Desc(-1.5625f);
    CHECK(r.controller.OnSamplerCreated(s.Get(), r.device.Get(), d, false) == SamplerTable::Result::Refreshed);
    r.controller.OnEvaluate(On(), Eval(2560, 3840, 2400));
    CHECK(r.controller.detector().mode() == Mode::Replace && CountLog("provisional") == 1);
    CHECK(r.engine.CurrentMode() == Mode::Replace);
    // the table's view of the sampler is the new one and its variant is rebuilt for it (generation bumped)
    const auto* entry = r.engine.table().Find(s.Get());
    CHECK(entry != nullptr && Near(entry->original.load(), -1.5625f) && Near(entry->baseCreate.load(), kQ) && entry->generation.load() >= 1);
}

// L-8: the target is followed while a hand-set value blocks the feature, so samplers made meanwhile carry the right creation base.
static void BaseTrackedWhileManual()
{
    g_log.clear();
    Rig r;
    CHECK(r.Init());
    Settings manual = On();
    manual.manualOverride = true;
    r.controller.OnEvaluate(manual, Eval(3840, 3840, 1000));
    r.controller.OnEvaluate(manual, Eval(2560, 3840, 2000)); // Quality while the override is set
    CHECK(!r.engine.Active() && Near(r.engine.Base(), -0.584963));
    auto during = r.Game(-1.5625f);
    CHECK(Near(r.engine.table().Find(during.Get())->baseCreate.load(), -0.584963));
    r.controller.OnEvaluate(On(), Eval(2560, 3840, 3000)); // cleared: resumes with the right creation base
    CHECK(r.engine.Active());
    r.Bind(0, 0, during.Get());
    CHECK(Near(r.BoundBias(0), -1.5625 - 0.584963, 1e-4)); // ADD (no pair seen): the game's bias plus the target
}

// M-C: a preview window evaluates first, the main scene comes later and every frame.
static void PreviewFirst()
{
    g_log.clear();
    Rig r;
    CHECK(r.Init());
    r.controller.OnEvaluate(On(), Eval(576, 1152, 0)); // the preview (ratio 2) anchors first
    CHECK(Near(r.engine.Base(), -1.0));
    std::uint64_t t = 0;
    for (int i = 1; i <= 60; ++i)
    {
        t += 16;
        r.controller.OnEvaluate(On(), Eval(2560, 3840, t)); // the main scene, Quality
        if (i % 4 == 0)
            r.controller.OnEvaluate(On(), Eval(576, 1152, t)); // and the preview now and then
    }
    CHECK(Near(r.engine.Base(), -0.584963)); // the main scene's ratio is the target
}
// The status the card shows.
static void Status()
{
    g_log.clear();
    Rig r;
    CHECK(r.Init());
    auto s0 = r.controller.Status(true, false);
    CHECK(!s0.hasTarget && !s0.running && s0.installed && s0.samplers == 0);
    auto art = r.Game(-1.0f);
    r.controller.OnEvaluate(On(), Eval(2560, 3840, 0));
    r.Bind(0, 0, art.Get());
    auto s1 = r.controller.Status(true, false);
    CHECK(s1.hasTarget && s1.running && Near(s1.target, -0.584963) && s1.samplers == 1 && s1.substitutedBinds == 1 &&
          s1.variants == 1 && s1.mode == Mode::Add && !s1.decided);
    CHECK(r.controller.Status(true, true).manualWins);
}

// Which hooks are attached when.
static void Plan()
{
    using A = InstallPlan::Action;
    {
        InstallPlan p; // the normal FFXIV session: enabled by default
        CHECK(p.OnFeatureCreate() == A::None); // no device yet, nothing installed
        CHECK(p.OnDeviceHooked(true) == A::InstallBind);
        CHECK(p.BindResult(true) == A::InstallCreate); // a feature was already created: the create hook follows
        p.CreateResult(true);
        CHECK(p.owns() && p.createHooked() && p.OnFeatureCreate() == A::None && p.OnDeviceHooked(true) == A::None);
        p.CreateRemoved();
        CHECK(p.bindInstalled() && !p.createHooked());
        p.BindRemoved();
        CHECK(!p.owns());
    }
    {
        InstallPlan p; // the create hook waits for the first feature create
        CHECK(p.OnDeviceHooked(true) == A::InstallBind);
        CHECK(p.BindResult(true) == A::None);
        CHECK(p.OnFeatureCreate() == A::InstallCreate);
        p.CreateResult(true);
        CHECK(p.OnFeatureCreate() == A::None);
    }
    {
        InstallPlan p; // off at the device seam, on later from the menu
        CHECK(p.OnDeviceHooked(false) == A::None);
        CHECK(p.OnSettingChanged(false) == A::None);
        CHECK(p.OnSettingChanged(true) == A::InstallBind);
        CHECK(p.BindResult(true) == A::None);
        CHECK(p.OnFeatureCreate() == A::InstallCreate);
    }
    {
        InstallPlan p; // the menu before any device: nothing to attach to
        CHECK(p.OnSettingChanged(true) == A::None && p.OnDeviceHooked(true) == A::InstallBind);
    }
    {
        InstallPlan p; // a failed bind install is never tried again; the older module keeps the work
        CHECK(p.OnDeviceHooked(true) == A::InstallBind);
        CHECK(p.BindResult(false) == A::None && p.bindFailed() && !p.owns());
        CHECK(p.OnSettingChanged(true) == A::None && p.OnDeviceHooked(true) == A::None && p.OnFeatureCreate() == A::None);
    }
    {
        InstallPlan p; // a failed create hook is not retried, the bind hooks stay (lazy classification covers the rest)
        p.OnDeviceHooked(true);
        p.BindResult(true);
        CHECK(p.OnFeatureCreate() == A::InstallCreate);
        p.CreateResult(false);
        CHECK(p.createFailed() && p.owns() && p.OnFeatureCreate() == A::None);
    }
}

int main()
{
    FullFlowWithReplace();
    ManualOverride();
    CreatesAndTable();
    SteadyTarget();
    RelatchAndWatch();
    RelatchLeavesOldContextAlone();
    WatchAtNativeResolution();
    ReCreateFeedsDetector();
    BaseTrackedWhileManual();
    PreviewFirst();
    Status();
    Plan();

    if (g_failed == 0)
        std::printf("mip_bind_control: all passed\n");
    return g_failed == 0 ? 0 : 1;
}
