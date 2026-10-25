#pragma once
// Shared by the depth binder tests: a fake ReShade backend, a rig around it and the CHECK macro.
//
// The fake models what the binder has to survive:
//  - ReShade's DEPTH semantic is ONE binding per runtime. Variables with that semantic read it; update_texture_bindings with
//    (0,0) erases the entry; a variable without a semantic keeps its own fixed binding. ReShade does not own the views it is
//    given, so a view that was freed while the map still holds it is a violation.
//  - Views are counted per handle: the owner's reference plus the core's. A view whose count reaches zero is freed, and any
//    AddRef, Release, bind or read of a freed view is recorded as a violation.
//  - One backend call can be made to throw, throw after its effect, or fail (fault injection by call number).
#include "ext/reshade/ReShadeDepthCore.h"

#include <atomic>
#include <cstdio>
#include <format>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace depthtest
{
using namespace ext::reshade::depth;

inline int g_fail = 0;
#define CHECK(c)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            std::printf("CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #c);                                           \
            ++::depthtest::g_fail;                                                                                     \
        }                                                                                                              \
    } while (0)

inline constexpr uint64_t kRt = 0x1f2321e1a60;
inline constexpr uint64_t kRt2 = 0x1f2321e2b70;
inline constexpr uint64_t kDevice = 0xD3D0;
inline constexpr uint64_t kCopy = 0xC0, kCopy2 = 0xC1;
inline constexpr uint64_t kOrig = 0xA1, kOrigSrgb = 0xA2;
inline constexpr uint64_t kOther = 0xB1, kOtherSrgb = 0xB2;
inline constexpr uint64_t kColor = 0xE1; // a view that is not a depth view
inline constexpr uint64_t kDepthVar = 0x501;

enum class Fault
{
    None,
    Throw,      // before the effect
    ThrowAfter, // after the effect
    Fail,       // returns false / 0 / Error without the effect
};

struct Fake final : IBackend
{
    std::atomic<int64_t> now { 1000 };
    std::atomic<uint64_t> frame { 100 };
    const std::atomic<uint64_t>* frameSource = nullptr; // when set, Frame() reads this counter instead (the depth-owned one)
    bool d3d11 = true;
    bool sameDevice = true;
    uint64_t device = kDevice; // the device of the runtimes the fake answers for (SameDevice)
    uint32_t bbW = 3840, bbH = 2160;
    bool bbOk = true;

    struct Var
    {
        uint64_t handle;
        std::string name;
        std::string semantic;
        uint64_t srv, srgb; // the fixed binding of a variable WITHOUT a semantic
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
    bool definitionError = false;
    std::set<uint64_t> depthViews { kOrig, kOrigSrgb, kOther, kOtherSrgb, kCopy, kCopy2 };

    // ReShade's "DEPTH" semantic binding of this runtime
    bool hasDepth = true;
    uint64_t depthSrv = kOrig, depthSrgb = kOrigSrgb;
    std::set<uint64_t> copyViews { kCopy, kCopy2 }; // the views the capture side makes

    // knobs for the restore paths
    bool failRestore = false; // update_texture_bindings with something that is not a copy returns false
    bool texturesOk = true;   // false: the texture variables cannot be enumerated
    bool slowAddRef = false;  // widen the window between the core reading a capture and holding its view
    bool logCalls = true;     // keep the call log (off: the allocation test must not count the fake's own allocations)

    // view counting
    mutable std::mutex refMutex;
    std::map<uint64_t, int> owner; // the owner's reference: 1 until OwnerRelease
    std::map<uint64_t, int> core;  // AddRef - Release by the core
    std::set<uint64_t> freed;
    std::atomic<int> violations { 0 };

    struct Update
    {
        std::string semantic;
        uint64_t srv, srgb;
    };
    std::vector<Update> updates;
    std::vector<std::string> log;
    std::map<uint64_t, int> bindingReads;
    int defQueries = 0, textureEnums = 0, uniformEnums = 0;

    int noCandidateCalls = 0; // NoDepthCandidate calls (the texture list is not kept: the allocation test counts the fake's allocations)
    size_t noCandidateTextures = 0;
    bool noCandidateThrows = false;

    int calls = 0, faultAt = 0; // the faultAt-th backend call is disturbed
    Fault fault = Fault::None;
    std::function<void(const char*)> afterCall; // runs after a call went through (not for AddRef: it runs under a lock)

    Fake()
    {
        vars.push_back({ kDepthVar, "ReShade::DepthBufferTex", "DEPTH", 0, 0 });
        unis.push_back({ 0x601, "bufready_depth", false });
        techniques.push_back({ "Main.fx", true });
        for (uint64_t v : { kCopy, kCopy2, kOrig, kOrigSrgb, kOther, kOtherSrgb, kColor })
            owner[v] = 1;
    }

    // ---- the DEPTH map
    void SetDepth(uint64_t srv, uint64_t srgb)
    {
        hasDepth = srv != 0 || srgb != 0;
        depthSrv = hasDepth ? srv : 0;
        depthSrgb = hasDepth ? srgb : 0;
    }
    // What a destroy of the runtime does to this map. Whether ResizeBuffers makes ReShade create a NEW runtime object (an empty map)
    // or RESET the same one (the pointer value comes back with its map) is unverified, so tests run both worlds.
    enum class DestroyWorld
    {
        MapDies, // a new object: the map starts empty
        Reset,   // the same object: the map survives
    };
    void RuntimeDestroyed(DestroyWorld world)
    {
        if (world == DestroyWorld::MapDies)
            SetDepth(0, 0);
    }
    std::pair<uint64_t, uint64_t> Depth() const { return { depthSrv, depthSrgb }; }
    bool DepthIs(uint64_t srv, uint64_t srgb) const { return depthSrv == srv && depthSrgb == srgb; }

    // ---- views
    bool Alive(uint64_t v) const { return (owner.count(v) ? owner.at(v) : 0) + (core.count(v) ? core.at(v) : 0) > 0; }
    void Violation() { ++violations; }
    uint64_t NewView() // a fresh view with the owner's reference
    {
        std::lock_guard lock(refMutex);
        static std::atomic<uint64_t> next { 0xC100 };
        const uint64_t v = next++;
        owner[v] = 1;
        copyViews.insert(v);
        depthViews.insert(v);
        return v;
    }
    void OwnerRelease(uint64_t v) // the owner drops its reference; the view dies if nothing else holds it
    {
        std::lock_guard lock(refMutex);
        owner[v] = 0;
        if (!Alive(v))
            freed.insert(v);
    }
    bool IsFreed(uint64_t v) const
    {
        std::lock_guard lock(refMutex);
        return freed.count(v) != 0;
    }

    // Every reference taken was given back and nothing dead was touched.
    bool Balanced() const
    {
        std::lock_guard lock(refMutex);
        for (const auto& [view, count] : core)
            if (count != 0)
                return false;
        return violations == 0;
    }
    int CoreRefs(uint64_t v) const
    {
        std::lock_guard lock(refMutex);
        return core.count(v) ? core.at(v) : 0;
    }
    // No variable and no semantic entry holds one of the capture side's views.
    bool HoldsCopy() const
    {
        if (hasDepth && (copyViews.count(depthSrv) || copyViews.count(depthSrgb)))
            return true;
        for (const auto& v : vars)
            if (v.semantic.empty() && (copyViews.count(v.srv) || copyViews.count(v.srgb)))
                return true;
        return false;
    }
    // The DEPTH entry itself (not a variable) holds one of the capture side's views.
    bool MapHoldsCopy() const { return hasDepth && (copyViews.count(depthSrv) || copyViews.count(depthSrgb)); }
    // The DEPTH entry holds a view that was freed: ReShade would bind a dangling pointer into the next effects it loads.
    bool DepthEntryDead() const
    {
        std::lock_guard lock(refMutex);
        return hasDepth && ((depthSrv != 0 && freed.count(depthSrv)) || (depthSrgb != 0 && freed.count(depthSrgb)));
    }
    // Reference counts the core still holds on the capture side's views.
    int CopyRefs() const
    {
        std::lock_guard lock(refMutex);
        int n = 0;
        for (const auto& [view, count] : core)
            if (copyViews.count(view))
                n += count;
        return n;
    }
    // While a restore is pending the copy may still be bound; the core must then still hold a reference to it.
    bool BoundCopiesReferenced() const
    {
        std::lock_guard lock(refMutex);
        for (uint64_t v : { depthSrv, depthSrgb })
            if (hasDepth && copyViews.count(v) && (freed.count(v) || !core.count(v) || core.at(v) < 1))
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
    Update UpdateAt(size_t i) const { return i < updates.size() ? updates[i] : Update { "<missing>", ~0ull, ~0ull }; }
    // ReShade's state as the tests compare it: the DEPTH map, the fixed bindings and the uniform values (not handles).
    std::string StateKey() const
    {
        return (hasDepth ? std::format("depth={:x}:{:x};", depthSrv, depthSrgb) : std::string("depth=none;")) + StateKeyNoDepth();
    }
    // The same without the DEPTH map: what a rollback that cannot tell what the entry held (and so erases it) must not change.
    std::string StateKeyNoDepth() const
    {
        std::string s;
        for (const auto& v : vars)
            if (v.semantic.empty())
                s += std::format("{}={:x}:{:x};", v.name, v.srv, v.srgb);
        for (size_t i = 0; i < unis.size(); ++i)
            s += std::format("u{}={};", i, unis[i].value);
        return s;
    }

    bool Hit(const char* name)
    {
        if (logCalls)
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
        if (afterCall)
            afterCall(name);
        return result;
    }

    void NoDepthCandidate(uint64_t, const std::vector<TextureVar>& textures) override
    {
        ++noCandidateCalls;
        noCandidateTextures = textures.size();
        if (noCandidateThrows)
            throw std::runtime_error("NoDepthCandidate");
    }
    int64_t NowMs() override { return Run<int64_t>("NowMs", 0, [&]() -> int64_t { return now; }); }
    uint64_t Frame() override
    {
        return Run<uint64_t>("Frame", 0, [&]() -> uint64_t { return frameSource != nullptr ? frameSource->load() : frame.load(); });
    }
    void AddRef(uint64_t view) override
    {
        if (Hit("AddRef"))
            throw std::runtime_error("AddRef"); // a COM AddRef either happens or does not
        if (slowAddRef)
            for (volatile int i = 0; i < 400; i = i + 1)
            {
            }
        std::lock_guard lock(refMutex);
        if (!owner.count(view) || !Alive(view))
        {
            Violation(); // unknown, or already freed
            return;
        }
        ++core[view];
    }
    void Release(uint64_t view) override
    {
        const bool hit = Hit("Release");
        {
            std::lock_guard lock(refMutex);
            if (!owner.count(view) || !Alive(view) || core[view] <= 0)
                Violation(); // unknown, freed, or more releases than references
            else
            {
                --core[view];
                if (!Alive(view))
                    freed.insert(view);
            }
        }
        if (hit)
            throw std::runtime_error("Release"); // a COM Release has happened when it throws
    }
    bool IsDepthView(uint64_t view) override
    {
        return Run<bool>("IsDepthView", false,
                         [&]() -> bool
                         {
                             std::lock_guard lock(refMutex); // the capture thread adds views
                             return depthViews.count(view) != 0;
                         });
    }
    bool IsD3D11(uint64_t) override { return Run<bool>("IsD3D11", false, [&]() -> bool { return d3d11; }); }
    bool SameDevice(uint64_t, uint64_t captureDevice) override
    {
        // 0 = no capture yet: the game's primary device
        return Run<bool>("SameDevice", false, [&]() -> bool { return sameDevice && (captureDevice == 0 || captureDevice == device); });
    }
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
                             if (!texturesOk)
                                 return false;
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
                                     srv = v.semantic == "DEPTH" ? depthSrv : v.srv;
                                     srgb = v.semantic == "DEPTH" ? depthSrgb : v.srgb;
                                     for (uint64_t view : { srv, srgb })
                                         if (view != 0 && IsFreed(view))
                                             Violation(); // ReShade hands out a dangling pointer
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
                             if (failRestore && !copyViews.count(srv))
                                 return false;
                             updates.push_back({ semantic, srv, srgb });
                             for (uint64_t view : { srv, srgb })
                                 if (view != 0 && IsFreed(view))
                                     Violation(); // binding a dead view
                             if (std::string(semantic) == "DEPTH")
                                 SetDepth(srv, srgb);
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
    DefinitionState Definition(uint64_t, const char* effect, const char* name, std::string& value) override
    {
        return Run<DefinitionState>("Definition", DefinitionState::Error,
                                    [&]() -> DefinitionState
                                    {
                                        ++defQueries;
                                        if (definitionError)
                                            return DefinitionState::Error;
                                        const auto& scope = effect == nullptr ? globalDefs : effectDefs[effect];
                                        const auto it = scope.find(name);
                                        if (it == scope.end())
                                            return DefinitionState::Undefined;
                                        value = it->second;
                                        return DefinitionState::Defined;
                                    });
    }
};

// Two runtimes with their own worlds: `a` answers for kRt, `b` for kRt2. Clock, frame counter and view counting come from `a`.
struct Router final : IBackend
{
    Fake& a;
    Fake& b;
    Router(Fake& first, Fake& second) : a(first), b(second) {}
    Fake& For(uint64_t rt) { return rt == kRt2 ? b : a; }
    int64_t NowMs() override { return a.NowMs(); }
    uint64_t Frame() override { return a.Frame(); }
    void AddRef(uint64_t v) override { a.AddRef(v); }
    void Release(uint64_t v) override { a.Release(v); }
    bool IsDepthView(uint64_t v) override { return a.IsDepthView(v); }
    bool IsD3D11(uint64_t rt) override { return For(rt).IsD3D11(rt); }
    bool SameDevice(uint64_t rt, uint64_t d) override { return For(rt).SameDevice(rt, d); }
    bool BackBufferSize(uint64_t rt, uint32_t& w, uint32_t& h) override { return For(rt).BackBufferSize(rt, w, h); }
    bool TextureVariables(uint64_t rt, std::vector<TextureVar>& o) override { return For(rt).TextureVariables(rt, o); }
    void NoDepthCandidate(uint64_t rt, const std::vector<TextureVar>& t) override { For(rt).NoDepthCandidate(rt, t); }
    bool TextureBinding(uint64_t rt, uint64_t v, uint64_t& s, uint64_t& g) override { return For(rt).TextureBinding(rt, v, s, g); }
    bool UpdateTextureBindings(uint64_t rt, const char* sem, uint64_t s, uint64_t g) override
    {
        return For(rt).UpdateTextureBindings(rt, sem, s, g);
    }
    bool UniformVariables(uint64_t rt, std::vector<UniformVar>& o) override { return For(rt).UniformVariables(rt, o); }
    bool GetUniformBool(uint64_t rt, uint64_t v, bool& x) override { return For(rt).GetUniformBool(rt, v, x); }
    bool SetUniformBool(uint64_t rt, uint64_t v, bool x) override { return For(rt).SetUniformBool(rt, v, x); }
    bool Techniques(uint64_t rt, std::vector<EffectTechnique>& o) override { return For(rt).Techniques(rt, o); }
    DefinitionState Definition(uint64_t rt, const char* e, const char* n, std::string& v) override
    {
        return For(rt).Definition(rt, e, n, v);
    }
};

inline std::vector<std::pair<Counter, uint64_t>> g_logged;
inline void Hook(Counter counter, uint64_t count) { g_logged.emplace_back(counter, count); }

struct Rig
{
    Fake fake;
    DepthBinder binder { fake, &Hook };
    uint64_t copyView = kCopy; // the view the capture side publishes

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
        c.copyView = copyView;
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
    // After a pass ended without a restore pending: ReShade holds nothing of ours and every reference is back.
    bool Clean() const { return !fake.HoldsCopy() && !fake.DepthEntryDead() && fake.Balanced(); }
};
} // namespace depthtest
