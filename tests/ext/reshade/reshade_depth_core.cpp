// Automatic ReShade scene depth: the binder driven by a fake ReShade backend (no ReShade, no GPU). The fake keeps a reference
// count per view handle, a call log and a single-fault injector, so every path is checked for balance and for leaving ReShade's
// bindings and uniforms exactly as it found them.
#include "ext/reshade/ReShadeDepthCore.h"

#include <atomic>
#include <cstdio>
#include <format>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace ext::reshade::depth;

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

constexpr uint64_t kRt = 0x1f2321e1a60;
constexpr uint64_t kDevice = 0xD3D0;
constexpr uint64_t kCopy = 0xC0, kCopy2 = 0xC1;
constexpr uint64_t kOrig = 0xA1, kOrigSrgb = 0xA2;
constexpr uint64_t kOther = 0xB1, kOtherSrgb = 0xB2;
constexpr uint64_t kDepthVar = 0x501;

const char* const kDisabled = "Automatic ReShade depth disabled";
const char* const kWaiting = "Waiting for ReShade depth effects";
const char* const kUnavailable = "Scene depth unavailable; generic depth retained";
const char* const kManual = "Manual ReShade depth transform retained";
const char* const kNoInput = "No depth input found; generic depth retained";

enum class Fault { None, Throw, ThrowAfter, Fail };

struct Fake final : IBackend
{
    int64_t now = 1000;
    uint64_t frame = 100;
    bool d3d11 = true;
    uint64_t device = kDevice;
    uint32_t bbW = 3840, bbH = 2160;
    bool bbOk = true;

    struct Var
    {
        uint64_t handle;
        std::string name;
        std::string semantic;
        uint64_t srv, srgb;
    };
    struct Uni
    {
        uint64_t handle;
        std::string source;
        bool value;
    };
    std::vector<Var> vars;
    std::vector<Uni> unis;
    std::vector<EffectTechnique> techniques;
    bool techniquesOk = true;
    std::map<std::string, std::string> globalDefs;
    std::map<std::string, std::map<std::string, std::string>> effectDefs;

    std::map<uint64_t, int> refs; // baseline 1 = the owner's reference
    int violations = 0;           // Release below the owner's reference, or AddRef/Release of an unknown view

    struct Update
    {
        std::string semantic;
        uint64_t srv, srgb;
    };
    std::vector<Update> updates;
    std::vector<std::string> log;
    std::map<uint64_t, int> bindingReads;
    int defQueries = 0, textureEnums = 0, uniformEnums = 0;

    int calls = 0, faultAt = 0; // the faultAt-th backend call is disturbed
    Fault fault = Fault::None;

    Fake()
    {
        vars.push_back({ kDepthVar, "ReShade::DepthBufferTex", "DEPTH", kOrig, kOrigSrgb });
        unis.push_back({ 0x601, "bufready_depth", false });
        techniques.push_back({ "Main.fx", true });
        for (uint64_t v : { kCopy, kCopy2, kOrig, kOrigSrgb, kOther, kOtherSrgb })
            refs[v] = 1;
    }

    Update UpdateAt(size_t i) const { return i < updates.size() ? updates[i] : Update { "<missing>", ~0ull, ~0ull }; }
    bool Balanced() const
    {
        for (const auto& [view, count] : refs)
            if (count != 1)
                return false;
        return violations == 0;
    }
    int Calls(const char* name) const
    {
        int n = 0;
        for (const auto& l : log)
            n += l == name ? 1 : 0;
        return n;
    }
    std::string StateKey() const
    {
        std::string s;
        for (const auto& v : vars)
            s += std::format("{:x}:{:x}:{:x};", v.handle, v.srv, v.srgb);
        for (const auto& u : unis)
            s += std::format("u{:x}={};", u.handle, u.value);
        return s;
    }

    bool Hit(const char* name)
    {
        log.push_back(name);
        return ++calls == faultAt;
    }
    template <class T, class F> T Run(const char* name, T failed, F&& effect)
    {
        const bool hit = Hit(name);
        if (hit && fault == Fault::Throw)
            throw std::runtime_error(name);
        if (hit && fault == Fault::Fail)
            return failed;
        T result = effect();
        if (hit && fault == Fault::ThrowAfter)
            throw std::runtime_error(name);
        return result;
    }

    int64_t NowMs() override { return Run<int64_t>("NowMs", 0, [&]() -> int64_t { return now; }); }
    uint64_t Frame() override { return Run<uint64_t>("Frame", 0, [&]() -> uint64_t { return frame; }); }
    void AddRef(uint64_t view) override
    {
        if (Hit("AddRef"))
            throw std::runtime_error("AddRef"); // a COM AddRef either happens or does not
        const auto it = refs.find(view);
        if (it == refs.end())
            ++violations;
        else
            ++it->second;
    }
    void Release(uint64_t view) override
    {
        const bool hit = Hit("Release");
        const auto it = refs.find(view);
        if (it == refs.end() || it->second <= 1)
            ++violations;
        else
            --it->second;
        if (hit)
            throw std::runtime_error("Release"); // a COM Release has happened when it throws
    }
    bool IsD3D11(uint64_t) override { return Run<bool>("IsD3D11", false, [&]() -> bool { return d3d11; }); }
    uint64_t Device(uint64_t) override { return Run<uint64_t>("Device", 0, [&]() -> uint64_t { return device; }); }
    bool BackBufferSize(uint64_t, uint32_t& w, uint32_t& h) override
    {
        return Run<bool>("BackBufferSize", false,
                         [&]() -> bool
                         {
                             if (!bbOk)
                                 return false;
                             w = bbW;
                             h = bbH;
                             return true;
                         });
    }
    bool TextureVariables(uint64_t, std::vector<TextureVar>& out) override
    {
        return Run<bool>("TextureVariables", false,
                         [&]() -> bool
                         {
                             ++textureEnums;
                             out.clear();
                             for (const auto& v : vars)
                                 out.push_back({ v.handle, v.name });
                             return true;
                         });
    }
    bool TextureBinding(uint64_t, uint64_t variable, uint64_t& srv, uint64_t& srgb) override
    {
        return Run<bool>("TextureBinding", false,
                         [&]() -> bool
                         {
                             ++bindingReads[variable];
                             for (const auto& v : vars)
                                 if (v.handle == variable)
                                 {
                                     srv = v.srv;
                                     srgb = v.srgb;
                                     return true;
                                 }
                             return false;
                         });
    }
    bool UpdateTextureBindings(uint64_t, const char* semantic, uint64_t srv, uint64_t srgb) override
    {
        return Run<bool>("UpdateTextureBindings", false,
                         [&]() -> bool
                         {
                             updates.push_back({ semantic, srv, srgb });
                             for (auto& v : vars)
                                 if (v.semantic == semantic)
                                 {
                                     v.srv = srv;
                                     v.srgb = srgb;
                                 }
                             return true;
                         });
    }
    bool UniformVariables(uint64_t, std::vector<UniformVar>& out) override
    {
        return Run<bool>("UniformVariables", false,
                         [&]() -> bool
                         {
                             ++uniformEnums;
                             out.clear();
                             for (const auto& u : unis)
                                 out.push_back({ u.handle, u.source });
                             return true;
                         });
    }
    bool GetUniformBool(uint64_t, uint64_t variable, bool& value) override
    {
        return Run<bool>("GetUniformBool", false,
                         [&]() -> bool
                         {
                             for (const auto& u : unis)
                                 if (u.handle == variable)
                                 {
                                     value = u.value;
                                     return true;
                                 }
                             return false;
                         });
    }
    bool SetUniformBool(uint64_t, uint64_t variable, bool value) override
    {
        return Run<bool>("SetUniformBool", false,
                         [&]() -> bool
                         {
                             for (auto& u : unis)
                                 if (u.handle == variable)
                                 {
                                     u.value = value;
                                     return true;
                                 }
                             return false;
                         });
    }
    bool Techniques(uint64_t, std::vector<EffectTechnique>& out) override
    {
        return Run<bool>("Techniques", false,
                         [&]() -> bool
                         {
                             if (!techniquesOk)
                                 return false;
                             out = techniques;
                             return true;
                         });
    }
    bool Definition(uint64_t, const char* effect, const char* name, std::string& value) override
    {
        return Run<bool>("Definition", false,
                         [&]() -> bool
                         {
                             ++defQueries;
                             const auto& scope = effect == nullptr ? globalDefs : effectDefs[effect];
                             const auto it = scope.find(name);
                             if (it == scope.end())
                                 return false;
                             value = it->second;
                             return true;
                         });
    }
};

std::vector<std::pair<Counter, uint64_t>> g_logged;
void Hook(Counter counter, uint64_t count) { g_logged.emplace_back(counter, count); }

struct Rig
{
    Fake fake;
    DepthBinder binder { fake, &Hook };

    explicit Rig(bool enable = true)
    {
        g_logged.clear();
        binder.SetEnabled(enable);
        Publish();
    }
    DepthCapture Capture() const
    {
        DepthCapture c;
        c.valid = true;
        c.outputW = 3840;
        c.outputH = 2160;
        c.renderW = 1920;
        c.renderH = 1080;
        c.frame = fake.frame;
        c.tickMs = fake.now;
        c.copyView = kCopy;
        c.device = kDevice;
        return c;
    }
    void Publish() { binder.PublishCapture(Capture()); }
    void NextFrame() // a new frame with a fresh capture, as the capture side does after every evaluate
    {
        ++fake.frame;
        fake.now += 16;
        Publish();
    }
    Outcome Begin() { return binder.OnBeginEffects(kRt); }
    void Finish() { binder.OnFinishEffects(kRt); }
    // One pass in a new frame; true if it ended up bound.
    bool Pass()
    {
        NextFrame();
        const bool bound = Begin() == Outcome::Bound;
        Finish();
        return bound;
    }
};

void HappyPath()
{
    Rig r;
    const std::string before = r.fake.StateKey();
    CHECK(r.binder.Status() == kWaiting);
    CHECK(r.Begin() == Outcome::Bound);
    CHECK(r.binder.Status() == "Automatic scene depth: 1920x1080 -> 3840x2160");
    CHECK(r.fake.updates.size() == 1);
    CHECK(r.fake.UpdateAt(0).semantic == "DEPTH" && r.fake.UpdateAt(0).srv == kCopy && r.fake.UpdateAt(0).srgb == kCopy);
    CHECK(r.fake.vars[0].srv == kCopy && r.fake.vars[0].srgb == kCopy);
    CHECK(r.fake.unis[0].value);
    CHECK(r.fake.refs[kCopy] == 2 && r.fake.refs[kOrig] == 2 && r.fake.refs[kOrigSrgb] == 2); // held for the pass
    CHECK(r.fake.textureEnums == 1 && r.fake.uniformEnums == 1);
    CHECK(r.binder.GetCounters().passesBound == 1);

    r.Finish();
    CHECK(r.fake.updates.size() == 2);
    CHECK(r.fake.UpdateAt(1).semantic == "DEPTH" && r.fake.UpdateAt(1).srv == kOrig && r.fake.UpdateAt(1).srgb == kOrigSrgb);
    CHECK(r.fake.StateKey() == before);
    CHECK(r.fake.Balanced());
    CHECK(r.binder.GetCounters().overwrittenByOther == 0);

    // render size equal to the output size: one size in the status
    Rig q;
    DepthCapture c = q.Capture();
    c.renderW = 3840;
    c.renderH = 2160;
    q.binder.PublishCapture(c);
    CHECK(q.Begin() == Outcome::Bound);
    CHECK(q.binder.Status() == "Automatic scene depth: 3840x2160");
    q.Finish();

    // a new capture replaces the copy that gets bound
    Rig p;
    DepthCapture c2 = p.Capture();
    c2.copyView = kCopy2;
    p.binder.PublishCapture(c2);
    CHECK(p.Begin() == Outcome::Bound);
    CHECK(p.fake.UpdateAt(0).srv == kCopy2 && p.fake.UpdateAt(0).srgb == kCopy2);
    p.Finish();
    CHECK(p.fake.Balanced());
}

void OriginalBindings()
{
    { // "no depth selected": zero is a valid original and is passed back as zero
        Rig r;
        r.fake.vars[0].srv = r.fake.vars[0].srgb = 0;
        CHECK(r.Begin() == Outcome::Bound);
        CHECK(r.fake.refs[kCopy] == 2 && r.fake.refs[kOrig] == 1); // no reference to a null view
        r.Finish();
        CHECK(r.fake.updates.size() == 2 && r.fake.UpdateAt(1).srv == 0 && r.fake.UpdateAt(1).srgb == 0);
        CHECK(r.fake.Balanced());
    }
    { // only one of the two views set
        Rig r;
        r.fake.vars[0].srgb = 0;
        CHECK(r.Begin() == Outcome::Bound);
        r.Finish();
        CHECK(r.fake.UpdateAt(1).srv == kOrig && r.fake.UpdateAt(1).srgb == 0);
        CHECK(r.fake.Balanced());
    }
    { // the original is the copy itself (a leaked earlier bind): bound, but never "restored" to it
        Rig r;
        r.fake.vars[0].srv = r.fake.vars[0].srgb = kCopy;
        CHECK(r.Begin() == Outcome::Bound);
        r.Finish();
        CHECK(r.fake.updates.size() == 1);
        CHECK(r.fake.Balanced());
        CHECK(r.binder.GetCounters().overwrittenByOther == 0);
    }
    { // ... also when only one of the two original views is the copy
        Rig r;
        r.fake.vars[0].srv = kCopy;
        r.fake.vars[0].srgb = kOrigSrgb;
        CHECK(r.Begin() == Outcome::Bound);
        r.Finish();
        CHECK(r.fake.updates.size() == 1);
        CHECK(r.fake.Balanced());
    }
    { // the same view as srv and srgb: two references, two releases
        Rig r;
        r.fake.vars[0].srv = r.fake.vars[0].srgb = kOrig;
        CHECK(r.Begin() == Outcome::Bound);
        CHECK(r.fake.refs[kOrig] == 3);
        r.Finish();
        CHECK(r.fake.Balanced());
    }
}

void RestoreOnlyIfOurs()
{
    { // another add-on replaced both views mid-pass (Generic Depth picked a new buffer): leave them, count it
        Rig r;
        CHECK(r.Begin() == Outcome::Bound);
        r.fake.vars[0].srv = kOther;
        r.fake.vars[0].srgb = kOtherSrgb;
        r.Finish();
        CHECK(r.fake.updates.size() == 1); // no restore
        CHECK(r.fake.vars[0].srv == kOther && r.fake.vars[0].srgb == kOtherSrgb);
        CHECK(r.binder.GetCounters().overwrittenByOther == 1);
        CHECK(!r.fake.unis[0].value); // the bool is restored anyway
        CHECK(r.fake.Balanced());
    }
    { // only the srgb view differs: still not ours
        Rig r;
        CHECK(r.Begin() == Outcome::Bound);
        r.fake.vars[0].srgb = kOtherSrgb;
        r.Finish();
        CHECK(r.fake.updates.size() == 1 && r.fake.vars[0].srgb == kOtherSrgb);
        CHECK(r.binder.GetCounters().overwrittenByOther == 1);
        CHECK(r.fake.Balanced());
    }
    { // only the srv view differs
        Rig r;
        CHECK(r.Begin() == Outcome::Bound);
        r.fake.vars[0].srv = kOther;
        r.Finish();
        CHECK(r.fake.updates.size() == 1 && r.fake.vars[0].srv == kOther);
        CHECK(r.binder.GetCounters().overwrittenByOther == 1);
        CHECK(r.fake.Balanced());
    }
    { // replaced by "nothing": also not ours
        Rig r;
        CHECK(r.Begin() == Outcome::Bound);
        r.fake.vars[0].srv = r.fake.vars[0].srgb = 0;
        r.Finish();
        CHECK(r.fake.updates.size() == 1 && r.binder.GetCounters().overwrittenByOther == 1);
    }
    { // the binding cannot be read at finish: nothing is written, the references still go
        Rig r;
        CHECK(r.Begin() == Outcome::Bound);
        r.fake.vars[0].handle = 0x999; // the variable is gone
        r.Finish();
        CHECK(r.fake.updates.size() == 1);
        CHECK(r.fake.refs[kCopy] == 1 && r.fake.refs[kOrig] == 1 && r.fake.refs[kOrigSrgb] == 1);
        CHECK(!r.fake.unis[0].value);
    }
}

void PairingAndRuntimeLifetime()
{
    { // finish with no begin: not one backend call
        Rig r;
        r.Finish();
        CHECK(r.fake.log.empty());
    }
    { // finish twice
        Rig r;
        r.Begin();
        r.Finish();
        const size_t n = r.fake.log.size();
        r.Finish();
        CHECK(r.fake.log.size() == n && r.fake.updates.size() == 2);
    }
    { // destroy with a pass open: references released, ReShade not called (its handles are stale), finish is a no-op
        Rig r;
        r.Begin();
        r.fake.log.clear();
        r.binder.OnDestroyRuntime(kRt);
        for (const auto& l : r.fake.log)
            CHECK(l == "Release");
        CHECK(r.fake.Calls("Release") == 3);
        CHECK(r.fake.Balanced());
        CHECK(r.fake.vars[0].srv == kCopy); // untouched: restoring through stale handles is worse than a stale binding
        r.fake.log.clear();
        r.Finish();
        CHECK(r.fake.log.empty());
    }
    { // reloaded effects with a pass open: the same
        Rig r;
        r.Begin();
        r.fake.log.clear();
        r.binder.OnReloadedEffects(kRt);
        for (const auto& l : r.fake.log)
            CHECK(l == "Release");
        CHECK(r.fake.Calls("Release") == 3 && r.fake.Balanced());
        r.fake.log.clear();
        r.Finish();
        CHECK(r.fake.log.empty() && r.fake.updates.size() == 1);
    }
    { // destroy / reload with no pass open: nothing to do
        Rig r;
        r.binder.OnDestroyRuntime(kRt);
        r.binder.OnReloadedEffects(kRt);
        r.binder.OnDestroyRuntime(0x1234);
        CHECK(r.fake.log.empty());
    }
    { // a pass that was never finished is rolled back by the next begin: no references pile up
        Rig r;
        const std::string before = r.fake.StateKey();
        CHECK(r.Begin() == Outcome::Bound);
        r.NextFrame();
        CHECK(r.Begin() == Outcome::Bound);
        CHECK(r.fake.updates.size() == 3); // bind, put back, bind
        CHECK(r.fake.UpdateAt(1).srv == kOrig && r.fake.UpdateAt(2).srv == kCopy);
        CHECK(r.fake.refs[kOrig] == 2 && r.fake.refs[kCopy] == 2);
        r.Finish();
        CHECK(r.fake.StateKey() == before && r.fake.Balanced());
        CHECK(r.binder.GetCounters().overwrittenByOther == 0);
    }
    { // two runtimes at once are independent
        Rig r;
        const uint64_t other = kRt + 0x1000;
        CHECK(r.binder.OnBeginEffects(kRt) == Outcome::Bound);
        r.binder.OnFinishEffects(kRt);
        CHECK(r.binder.OnBeginEffects(other) == Outcome::Bound); // same frame, other runtime: not a duplicate
        r.binder.OnFinishEffects(other);
        CHECK(r.fake.Balanced());
    }
}

void OncePerFrame()
{
    Rig r;
    CHECK(r.Begin() == Outcome::Bound);
    r.Finish();
    r.fake.log.clear();
    CHECK(r.Begin() == Outcome::Duplicate);
    CHECK(r.fake.log.size() == 1 && r.fake.log[0] == "Frame"); // nothing else touched
    CHECK(r.binder.Status() == "Automatic scene depth: 1920x1080 -> 3840x2160");
    r.Finish();
    CHECK(r.fake.updates.size() == 2);
    CHECK(r.binder.GetCounters().passesBound == 1);

    r.NextFrame(); // the frame advance re-arms
    CHECK(r.Begin() == Outcome::Bound);
    r.Finish();
    CHECK(r.binder.GetCounters().passesBound == 2);

    // a begin that was refused still uses up the frame (and counts once)
    Rig q;
    q.fake.vars.clear();
    CHECK(q.Begin() == Outcome::NoDepthInput);
    CHECK(q.Begin() == Outcome::Duplicate);
    CHECK(q.binder.GetCounters().noDepthInput == 1);

    // reload and destroy re-arm, too
    Rig s;
    s.Begin();
    s.Finish();
    s.binder.OnReloadedEffects(kRt);
    CHECK(s.Begin() == Outcome::Bound);
    s.Finish();
    s.binder.OnDestroyRuntime(kRt);
    CHECK(s.Begin() == Outcome::Bound);
    s.Finish();
}

// Applies `breakIt` to a fresh rig and checks that the gate refuses with `outcome` and `status`, leaves ReShade alone and holds
// no reference.
template <class F> void Refuses(Outcome outcome, const char* status, F&& breakIt, const char* what)
{
    Rig r;
    breakIt(r);
    const std::string before = r.fake.StateKey();
    const Outcome got = r.Begin();
    if (got != outcome || r.binder.Status() != status)
        std::printf("  gate '%s': got outcome %d, status '%s'\n", what, int(got), r.binder.Status().c_str());
    CHECK(got == outcome);
    CHECK(r.binder.Status() == status);
    CHECK(r.fake.updates.empty());
    CHECK(r.fake.StateKey() == before);
    CHECK(r.fake.Balanced());
    r.Finish();
    CHECK(r.fake.updates.empty() && r.fake.Balanced());
}

void Gates()
{
    Refuses(Outcome::Disabled, kDisabled, [](Rig& r) { r.binder.SetEnabled(false); }, "disabled");
    { // disabled: not one backend call
        Rig r(false);
        CHECK(r.Begin() == Outcome::Disabled);
        CHECK(r.fake.log.empty());
    }
    Refuses(Outcome::OtherDevice, "ReShade runs on another device; generic depth retained", [](Rig& r) { r.fake.device = 0xBAD; },
            "other device");
    Refuses(Outcome::NotD3D11, "ReShade is not using D3D11; generic depth retained", [](Rig& r) { r.fake.d3d11 = false; },
            "not D3D11");
    Refuses(Outcome::CaptureUnavailable, kUnavailable, [](Rig& r) { r.binder.PublishCapture(DepthCapture {}); }, "no capture");
    Refuses(Outcome::CaptureUnavailable, kUnavailable,
            [](Rig& r)
            {
                DepthCapture c = r.Capture();
                c.valid = false;
                r.binder.PublishCapture(c);
            },
            "withdrawn capture");
    Refuses(Outcome::CaptureUnavailable, kUnavailable,
            [](Rig& r)
            {
                DepthCapture c = r.Capture();
                c.copyView = 0;
                r.binder.PublishCapture(c);
            },
            "capture without a copy");
    Refuses(Outcome::CaptureUnavailable, kUnavailable, [](Rig& r) { r.fake.now += 251; }, "251 ms old");
    Refuses(Outcome::CaptureUnavailable, kUnavailable, [](Rig& r) { r.fake.frame += 2; }, "2 frames old");
    Refuses(Outcome::CaptureUnavailable, kUnavailable, [](Rig& r) { r.fake.frame -= 1; }, "from the future");
    Refuses(Outcome::SizeMismatch, "ReShade size differs from the scene depth; generic depth retained",
            [](Rig& r) { r.fake.bbW = 1920; }, "width");
    Refuses(Outcome::SizeMismatch, "ReShade size differs from the scene depth; generic depth retained",
            [](Rig& r) { r.fake.bbH = 2161; }, "height");
    Refuses(Outcome::SizeMismatch, "ReShade size differs from the scene depth; generic depth retained",
            [](Rig& r) { r.fake.bbOk = false; }, "size unknown");
    Refuses(Outcome::ManualTransform, kManual, [](Rig& r) { r.fake.globalDefs["RESHADE_DEPTH_INPUT_X_SCALE"] = "2"; }, "manual");
    Refuses(Outcome::NoDepthInput, kNoInput, [](Rig& r) { r.fake.vars.clear(); }, "no variables");

    // boundary values pass
    { // 250 ms old, 1 frame old
        Rig r;
        r.fake.now += 250;
        r.fake.frame += 1;
        CHECK(r.Begin() == Outcome::Bound);
        r.Finish();
    }
    { // a capture of this very frame and moment
        Rig r;
        CHECK(r.Begin() == Outcome::Bound);
        r.Finish();
    }

    // counters of the gates that have one
    Rig r;
    r.binder.PublishCapture(DepthCapture {});
    r.Begin();
    CHECK(r.binder.GetCounters().staleCaptures == 1);
    r.NextFrame();
    r.fake.bbW = 100;
    r.Begin();
    CHECK(r.binder.GetCounters().sizeMismatches == 1);
    const Counters c = r.binder.GetCounters();
    CHECK(c.overwrittenByOther == 0 && c.manualTransformStandDowns == 0 && c.noDepthInput == 0 && c.passesBound == 0);
}

void GateOrder()
{
    // Everything is wrong at once; fixing the gates one by one must move the outcome down the list in order.
    Rig r(false);
    bool captureUsable = false;
    r.fake.device = 0xBAD;
    r.fake.d3d11 = false;
    r.fake.bbW = 100;
    r.fake.globalDefs["RESHADE_DEPTH_INPUT_Y_OFFSET"] = "3";
    r.fake.vars.clear();

    auto step = [&](Outcome want, const char* what)
    {
        ++r.fake.frame;
        r.fake.now += 16;
        DepthCapture c = r.Capture();
        c.valid = captureUsable; // the device stays known: a withdrawn capture still says which device it was made on
        r.binder.PublishCapture(c);
        const Outcome got = r.Begin();
        if (got != want)
            std::printf("  order: expected '%s', got outcome %d\n", what, int(got));
        CHECK(got == want);
        r.Finish();
    };
    step(Outcome::Disabled, "disabled");
    r.binder.SetEnabled(true);
    step(Outcome::OtherDevice, "other device");
    r.fake.device = kDevice;
    step(Outcome::NotD3D11, "not D3D11");
    r.fake.d3d11 = true;
    step(Outcome::CaptureUnavailable, "capture");
    CHECK(r.binder.GetCounters().staleCaptures == 1 && r.binder.GetCounters().sizeMismatches == 0);
    captureUsable = true;
    step(Outcome::SizeMismatch, "size");
    CHECK(r.binder.GetCounters().sizeMismatches == 1 && r.binder.GetCounters().manualTransformStandDowns == 0);
    r.fake.bbW = 3840;
    step(Outcome::ManualTransform, "manual transform");
    CHECK(r.fake.textureEnums == 0); // reported without looking for a depth input
    CHECK(r.binder.GetCounters().noDepthInput == 0);
    r.fake.globalDefs.clear();
    r.binder.OnReloadedEffects(kRt); // the manual check is cached until effects reload
    step(Outcome::NoDepthInput, "no depth input");
    r.fake.vars.push_back({ kDepthVar, "ReShade::DepthBufferTex", "DEPTH", kOrig, kOrigSrgb });
    step(Outcome::Bound, "bound");
    const Counters c = r.binder.GetCounters();
    CHECK(c.staleCaptures == 1 && c.sizeMismatches == 1 && c.manualTransformStandDowns == 1 && c.noDepthInput == 1 &&
          c.passesBound == 1);
    CHECK(r.fake.Balanced());
}

void ManualTransform()
{
    const char* const scales[] = { "RESHADE_DEPTH_INPUT_X_SCALE", "RESHADE_DEPTH_INPUT_Y_SCALE" };
    const char* const offsets[] = { "RESHADE_DEPTH_INPUT_X_OFFSET", "RESHADE_DEPTH_INPUT_Y_OFFSET",
                                    "RESHADE_DEPTH_INPUT_X_PIXEL_OFFSET", "RESHADE_DEPTH_INPUT_Y_PIXEL_OFFSET" };
    auto outcomeWith = [](const char* macro, const char* value, const char* effect = nullptr)
    {
        Rig r;
        if (effect == nullptr)
            r.fake.globalDefs[macro] = value;
        else
            r.fake.effectDefs[effect][macro] = value;
        const Outcome o = r.Begin();
        r.Finish();
        CHECK(r.fake.Balanced());
        return o;
    };
    auto expectOutcome = [&](const char* macro, const char* value, Outcome want)
    {
        const Outcome got = outcomeWith(macro, value);
        if (got != want)
            std::printf("  %s = '%s': %s\n", macro, value, want == Outcome::Bound ? "should be neutral" : "should stand down");
        CHECK(got == want);
    };
    for (const char* m : scales)
    {
        for (const char* v : { "1", "1.0", "1.0f", "1.0F", " 1 ", "\t1.00\t", "1.", "1e0", "10e-1" })
        {
            expectOutcome(m, v, Outcome::Bound);
        }
        for (const char* v : { "0", "2", "0.5", "1.0001", "abc", "", " ", "1 2", "1.0x", "1,0", "+1", "inf", "-inf", "nan", "0x1", "1ff",
                               "f", "(1.0)", "1.0 f", "-1", "1e1" })
            expectOutcome(m, v, Outcome::ManualTransform);
    }
    for (const char* m : offsets)
    {
        for (const char* v : { "0", "0.0", "0.0f", "-0", "-0.0", " 0 ", "0e5", "000" })
            expectOutcome(m, v, Outcome::Bound);
        for (const char* v : { "1", "0.001", "-0.5", "abc", "", "0 0", "inf", "nan", "1.0f" })
            expectOutcome(m, v, Outcome::ManualTransform);
    }

    // per effect: only effects with an enabled technique count, and any one non-neutral value ends it
    for (const char* m : scales)
        CHECK(outcomeWith(m, "2", "Main.fx") == Outcome::ManualTransform);
    for (const char* m : offsets)
        CHECK(outcomeWith(m, "4", "Main.fx") == Outcome::ManualTransform);
    CHECK(outcomeWith("RESHADE_DEPTH_INPUT_X_SCALE", "1.0", "Main.fx") == Outcome::Bound);
    {
        Rig r; // a disabled technique's effect is ignored
        r.fake.techniques = { { "Main.fx", true }, { "Off.fx", false } };
        r.fake.effectDefs["Off.fx"]["RESHADE_DEPTH_INPUT_X_SCALE"] = "2";
        CHECK(r.Begin() == Outcome::Bound);
        r.Finish();
    }
    {
        Rig r; // one enabled technique among disabled ones of the same effect is enough
        r.fake.techniques = { { "Mixed.fx", false }, { "Mixed.fx", true } };
        r.fake.effectDefs["Mixed.fx"]["RESHADE_DEPTH_INPUT_Y_SCALE"] = "2";
        CHECK(r.Begin() == Outcome::ManualTransform);
    }
    {
        Rig r; // the last of several effects
        r.fake.techniques = { { "A.fx", true }, { "B.fx", true }, { "C.fx", true } };
        r.fake.effectDefs["C.fx"]["RESHADE_DEPTH_INPUT_Y_PIXEL_OFFSET"] = "1";
        CHECK(r.Begin() == Outcome::ManualTransform);
    }
    {
        Rig r; // no enabled technique at all: only the global scope is left to check
        r.fake.techniques = { { "Main.fx", false } };
        r.fake.effectDefs["Main.fx"]["RESHADE_DEPTH_INPUT_X_SCALE"] = "2";
        CHECK(r.Begin() == Outcome::Bound);
        r.Finish();
        Rig q;
        q.fake.techniques.clear();
        q.fake.globalDefs["RESHADE_DEPTH_INPUT_X_SCALE"] = "2";
        CHECK(q.Begin() == Outcome::ManualTransform);
    }
    { // no partial application: a stand-down changes nothing at all
        Rig r;
        const std::string before = r.fake.StateKey();
        r.fake.globalDefs["RESHADE_DEPTH_INPUT_X_OFFSET"] = "0.1";
        CHECK(r.Begin() == Outcome::ManualTransform);
        CHECK(r.fake.updates.empty() && r.fake.StateKey() == before && r.fake.Balanced());
        CHECK(r.binder.GetCounters().manualTransformStandDowns == 1);
    }
    { // a technique query failure is neither "manual" nor "neutral" and is not cached
        Rig r;
        r.fake.techniquesOk = false;
        CHECK(r.Begin() == Outcome::Failed);
        r.fake.techniquesOk = true;
        r.NextFrame();
        CHECK(r.Begin() == Outcome::Bound);
        r.Finish();
    }
}

void ManualTransformCache()
{
    { // neutral is remembered: no new query until the effects reload
        Rig r;
        CHECK(r.Pass());
        const int queries = r.fake.defQueries;
        CHECK(queries > 0);
        r.fake.globalDefs["RESHADE_DEPTH_INPUT_X_SCALE"] = "2"; // a change ReShade only applies after a reload
        CHECK(r.Pass());
        CHECK(r.Pass());
        CHECK(r.fake.defQueries == queries);
        r.binder.OnReloadedEffects(kRt);
        r.NextFrame();
        CHECK(r.Begin() == Outcome::ManualTransform);
        CHECK(r.fake.defQueries > queries);
    }
    { // manual is remembered, too, and a reload sees the fix
        Rig r;
        r.fake.effectDefs["Main.fx"]["RESHADE_DEPTH_INPUT_X_PIXEL_OFFSET"] = "5";
        r.NextFrame();
        CHECK(r.Begin() == Outcome::ManualTransform);
        const int queries = r.fake.defQueries;
        r.fake.effectDefs.clear();
        r.NextFrame();
        CHECK(r.Begin() == Outcome::ManualTransform && r.fake.defQueries == queries);
        r.binder.OnReloadedEffects(kRt);
        r.NextFrame();
        CHECK(r.Begin() == Outcome::Bound);
        r.Finish();
    }
    { // destroy forgets it
        Rig r;
        r.fake.globalDefs["RESHADE_DEPTH_INPUT_Y_SCALE"] = "3";
        CHECK(r.Pass() == false);
        r.fake.globalDefs.clear();
        r.binder.OnDestroyRuntime(kRt);
        CHECK(r.Pass());
    }
    { // global scope is asked first: a global value spares the per-effect queries
        Rig r;
        r.fake.globalDefs["RESHADE_DEPTH_INPUT_X_SCALE"] = "2";
        r.Begin();
        CHECK(r.fake.defQueries == 1 && r.fake.Calls("Techniques") == 0);
    }
}

void DepthInputLookup()
{
    auto restoredOriginal = [](const char* name, uint64_t& restoredSrv)
    {
        Rig r;
        r.fake.vars[0].name = name;
        const Outcome o = r.Begin();
        r.Finish();
        restoredSrv = r.fake.updates.size() == 2 ? r.fake.UpdateAt(1).srv : 0;
        CHECK(r.fake.Balanced());
        return o;
    };
    uint64_t srv = 0;
    for (const char* name : { "ReShade::DepthBufferTex", "DepthBufferTex", "DepthInputTex" })
    {
        CHECK(restoredOriginal(name, srv) == Outcome::Bound);
        CHECK(srv == kOrig);
    }
    for (const char* name : { "MXAODepth", "texDepthMip", "depthbuffertex", "DepthBufferTex2", " DepthBufferTex", "DepthBufferTex ",
                              "ReShade::DepthBufferTexture", "Depth", "ReShade::depthbuffertex", "" })
        CHECK(restoredOriginal(name, srv) == Outcome::NoDepthInput);

    { // an MXAO-only runtime: DepthInputTex and decoys, no DepthBufferTex anywhere
        Rig r;
        r.fake.vars = { { 0x510, "MXAODepth", "", kOther, kOtherSrgb },
                        { 0x511, "texDepthMip", "", kOther, kOtherSrgb },
                        { 0x512, "DepthInputTex", "DEPTH", kOrig, kOrigSrgb } };
        const std::string before = r.fake.StateKey();
        CHECK(r.Begin() == Outcome::Bound);
        CHECK(r.fake.vars[2].srv == kCopy && r.fake.vars[0].srv == kOther && r.fake.vars[1].srv == kOther);
        r.Finish();
        CHECK(r.fake.StateKey() == before && r.fake.Balanced());
        CHECK(r.fake.bindingReads.count(0x510) == 0 && r.fake.bindingReads.count(0x511) == 0); // decoys are never read
    }
    { // decoys only: nothing is read, saved, bound or restored
        Rig r;
        r.fake.vars = { { 0x510, "MXAODepth", "", kOther, kOtherSrgb }, { 0x511, "texDepthMip", "DEPTH", kOrig, kOrigSrgb } };
        const std::string before = r.fake.StateKey();
        CHECK(r.Begin() == Outcome::NoDepthInput);
        r.Finish();
        CHECK(r.fake.bindingReads.empty() && r.fake.updates.empty());
        CHECK(r.fake.Calls("AddRef") == 1); // only the capture's copy, taken before the gates and released again
        CHECK(r.fake.StateKey() == before && r.fake.Balanced());
        CHECK(r.binder.GetCounters().noDepthInput == 1 && r.binder.Status() == kNoInput);
    }
    { // the list order decides, not the enumeration order
        Rig r;
        r.fake.vars = { { 0x520, "DepthInputTex", "DEPTH", kOther, kOtherSrgb },
                        { 0x521, "DepthBufferTex", "DEPTH", kOther, kOtherSrgb },
                        { 0x522, "ReShade::DepthBufferTex", "DEPTH", kOrig, kOrigSrgb } };
        CHECK(r.Begin() == Outcome::Bound);
        CHECK(r.fake.bindingReads.size() == 1 && r.fake.bindingReads.count(0x522) == 1);
        r.Finish();
        Rig q;
        q.fake.vars = { { 0x520, "DepthInputTex", "DEPTH", kOther, kOtherSrgb }, { 0x521, "DepthBufferTex", "DEPTH", kOrig, kOrigSrgb } };
        CHECK(q.Begin() == Outcome::Bound);
        CHECK(q.fake.bindingReads.size() == 1 && q.fake.bindingReads.count(0x521) == 1);
        q.Finish();
    }
    { // a name must match exactly, whatever the enumeration order says
        Rig r;
        r.fake.vars = { { 0x530, "DepthInputTexX", "DEPTH", kOther, kOtherSrgb }, { 0x531, "DepthInputTex", "DEPTH", kOrig, kOrigSrgb } };
        CHECK(r.Begin() == Outcome::Bound);
        CHECK(r.fake.bindingReads.size() == 1 && r.fake.bindingReads.count(0x531) == 1);
        r.Finish();
        CHECK(r.fake.UpdateAt(1).srv == kOrig);
    }
    { // enumerated once per pass; handles are never kept for the next one
        Rig r;
        CHECK(r.Pass());
        CHECK(r.fake.textureEnums == 1);
        r.fake.vars[0].handle = 0x777; // a partial reload gave the variable a new handle (the old one is dangling)
        CHECK(r.Pass());
        CHECK(r.fake.textureEnums == 2);
        CHECK(r.fake.bindingReads.count(0x777) == 1);
        CHECK(r.fake.Balanced());
    }
}

void BufReadyDepth()
{
    { // only the exact annotation value; other values are left alone
        Rig r;
        r.fake.unis = { { 0x601, "bufready_depth", false },
                        { 0x602, "bufready_depth_x", false },
                        { 0x603, "BUFREADY_DEPTH", false },
                        { 0x604, "", false },
                        { 0x605, "bufready_color", false },
                        { 0x606, " bufready_depth", false },
                        { 0x607, "bufready_depth", false } };
        const std::string before = r.fake.StateKey();
        CHECK(r.Begin() == Outcome::Bound);
        for (const auto& u : r.fake.unis)
            CHECK(u.value == (u.handle == 0x601 || u.handle == 0x607));
        r.Finish();
        CHECK(r.fake.StateKey() == before && r.fake.Balanced());
    }
    { // an old value of true stays true; false goes back to false
        Rig r;
        r.fake.unis = { { 0x601, "bufready_depth", true }, { 0x602, "bufready_depth", false } };
        const std::string before = r.fake.StateKey();
        CHECK(r.Begin() == Outcome::Bound);
        CHECK(r.fake.unis[0].value && r.fake.unis[1].value);
        r.Finish();
        CHECK(r.fake.unis[0].value && !r.fake.unis[1].value);
        CHECK(r.fake.StateKey() == before);
    }
    { // no such uniform: the pass still binds
        Rig r;
        r.fake.unis.clear();
        CHECK(r.Begin() == Outcome::Bound);
        r.Finish();
        CHECK(r.fake.Balanced());
    }
    { // enumerated every pass, handles never kept
        Rig r;
        CHECK(r.Pass());
        CHECK(r.fake.uniformEnums == 1);
        r.fake.unis[0].handle = 0x688;
        r.NextFrame();
        CHECK(r.Begin() == Outcome::Bound);
        CHECK(r.fake.unis[0].value && r.fake.uniformEnums == 2);
        r.Finish();
        CHECK(!r.fake.unis[0].value);
    }
    { // every refusal leaves the uniforms alone
        Rig r;
        r.fake.vars.clear();
        CHECK(r.Begin() == Outcome::NoDepthInput);
        CHECK(!r.fake.unis[0].value && r.fake.uniformEnums == 0);
    }
}

void EnableDisable()
{
    Rig r(false);
    CHECK(r.binder.Status() == kDisabled);
    r.binder.SetEnabled(true);
    CHECK(r.binder.Status() == kWaiting);
    CHECK(r.Begin() == Outcome::Bound);
    const std::string status = r.binder.Status();
    r.binder.SetEnabled(true); // a repeated "on" (a config poll) does not reset the status
    CHECK(r.binder.Status() == status);

    // switched off in the middle of a pass: finish still puts everything back
    const std::string before = Rig().fake.StateKey();
    r.binder.SetEnabled(false);
    CHECK(r.binder.Status() == kDisabled);
    r.Finish();
    CHECK(r.fake.StateKey() == before && r.fake.Balanced());
    CHECK(r.fake.updates.size() == 2 && r.fake.UpdateAt(1).srv == kOrig);

    r.NextFrame();
    CHECK(r.Begin() == Outcome::Disabled);
    r.binder.SetEnabled(true);

    // switched off, then a new begin before the finish arrived: the old pass is put back first
    Rig q;
    CHECK(q.Begin() == Outcome::Bound);
    q.binder.SetEnabled(false);
    q.NextFrame();
    CHECK(q.Begin() == Outcome::Disabled);
    CHECK(q.fake.StateKey() == before && q.fake.Balanced());
}

void FaultInjection()
{
    int beginCalls = 0, totalCalls = 0;
    {
        Rig r;
        r.Begin();
        beginCalls = r.fake.calls;
        r.Finish();
        totalCalls = r.fake.calls;
    }
    CHECK(beginCalls > 10 && totalCalls > beginCalls);

    const char* const names[] = { "throws", "throws after its effect", "fails" };
    const Fault faults[] = { Fault::Throw, Fault::ThrowAfter, Fault::Fail };
    for (int n = 1; n <= totalCalls; ++n)
        for (int f = 0; f < 3; ++f)
        {
            Rig r;
            const std::string before = r.fake.StateKey();
            r.fake.faultAt = n;
            r.fake.fault = faults[f];
            bool escaped = false;
            try
            {
                r.Begin();
                r.Finish();
            }
            catch (...)
            {
                escaped = true;
            }
            const bool inBegin = n <= beginCalls;
            const bool restored = r.fake.StateKey() == before;
            const bool balanced = r.fake.Balanced();
            if (escaped || !balanced || (inBegin && !restored))
                std::printf("  fault: call #%d (%s) %s: escaped=%d balanced=%d restored=%d\n", n,
                            n <= int(r.fake.log.size()) ? r.fake.log[n - 1].c_str() : "?", names[f], escaped, balanced, restored);
            CHECK(!escaped);
            CHECK(balanced);
            if (inBegin)
                CHECK(restored);
        }
    for (int release = 1; release <= 3; ++release) // a Release that throws must not keep the others from running
        for (int reload = 0; reload < 2; ++reload)
        {
            Rig d;
            d.Begin();
            d.fake.calls = 0;
            d.fake.faultAt = release;
            d.fake.fault = Fault::ThrowAfter;
            bool escaped = false;
            try
            {
                if (reload)
                    d.binder.OnReloadedEffects(kRt);
                else
                    d.binder.OnDestroyRuntime(kRt);
            }
            catch (...)
            {
                escaped = true;
            }
            CHECK(!escaped);
            CHECK(d.fake.Balanced());
        }
    { // a failure in the middle of the bind leaves a status that says so
        Rig r;
        r.fake.faultAt = 1;
        r.fake.fault = Fault::Throw;
        CHECK(r.Begin() == Outcome::Failed);
        CHECK(r.binder.Status() == "ReShade call failed; generic depth retained");
    }
}

void LogHook()
{
    Rig r;
    r.fake.vars.clear();
    for (int i = 0; i < 2001; ++i)
    {
        r.NextFrame();
        r.Begin();
    }
    CHECK(r.binder.GetCounters().noDepthInput == 2001);
    CHECK(g_logged.size() == 3);
    if (g_logged.size() == 3)
        CHECK(g_logged[0] == std::make_pair(Counter::NoDepthInput, uint64_t(1)) &&
              g_logged[1] == std::make_pair(Counter::NoDepthInput, uint64_t(1000)) &&
              g_logged[2] == std::make_pair(Counter::NoDepthInput, uint64_t(2000)));

    Rig q; // every counter reaches the hook on its first increment, with its own name
    q.fake.bbW = 1;
    q.Begin();
    q.NextFrame();
    q.fake.bbW = 3840;
    q.fake.globalDefs["RESHADE_DEPTH_INPUT_X_SCALE"] = "2";
    q.Begin();
    q.NextFrame();
    q.binder.PublishCapture(DepthCapture {});
    q.Begin();
    CHECK(g_logged.size() == 3);
    if (g_logged.size() == 3)
        CHECK(g_logged[0].first == Counter::SizeMismatches && g_logged[1].first == Counter::ManualTransformStandDowns &&
              g_logged[2].first == Counter::StaleCaptures);

    Rig p; // overwrittenByOther and passesBound too
    p.Begin();
    p.fake.vars[0].srv = kOther;
    p.Finish();
    CHECK(g_logged.size() == 2 && g_logged[0].first == Counter::PassesBound && g_logged[1].first == Counter::OverwrittenByOther);
}

void Threads()
{
    // Smoke test: the capture side and a status reader run against the event thread. (Only this thread touches the fake.)
    Rig r;
    std::atomic<bool> stop { false };
    const DepthCapture c = r.Capture();
    std::thread publisher(
        [&]
        {
            while (!stop.load())
            {
                r.binder.PublishCapture(c);
                (void)r.binder.Status();
                (void)r.binder.GetCounters();
            }
        });
    for (int i = 0; i < 2000; ++i)
    {
        ++r.fake.frame;
        r.Begin();
        r.Finish();
        (void)r.binder.Status();
    }
    stop = true;
    publisher.join();
    CHECK(r.fake.Balanced());
    CHECK(!r.binder.Status().empty());
}
} // namespace

int main()
{
    HappyPath();
    OriginalBindings();
    RestoreOnlyIfOurs();
    PairingAndRuntimeLifetime();
    OncePerFrame();
    Gates();
    GateOrder();
    ManualTransform();
    ManualTransformCache();
    DepthInputLookup();
    BufReadyDepth();
    EnableDisable();
    FaultInjection();
    LogHook();
    Threads();
    if (g_fail)
    {
        std::printf("reshade_depth_core: %d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("reshade_depth_core: all checks passed\n");
    return 0;
}
