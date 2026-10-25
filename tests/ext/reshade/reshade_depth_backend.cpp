// Automatic ReShade scene depth, the live backend (ReShadeDepthBackend.h) over a FAKE raw layer: the size-query-first string reading
// (the NUL stripped, never a truncated value), the tri-state definition, the handle lists that outgrow the first capacity, the
// untrusted views (a faulting AddRef / Release / format query), SameDevice (native devices, the only-runtime fallback, the capture
// device 0), the registry behind the frame counter, the first-bind readback log, and the real binder on top with a fault injected at
// every raw call of a pass. The real raw layer (ReShadeDepthRaw.cpp, SEH thunks over effect_runtime) is not covered here: it needs a
// real ReShade.
#include "pch.h"

#include "ext/reshade/ReShadeDepthBackend.h"
#include "ext/reshade/ReShadeDepthCore.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
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

constexpr uint64_t kRt = 0x1f2321e1a60, kRt2 = 0x1f2321e2b70, kRt3 = 0x1f2321e3c80;
constexpr uint64_t kDev = 0xD3D0, kDev2 = 0xD3D1;
constexpr uint64_t kCopy = 0xC0, kOrig = 0xA1, kOrigSrgb = 0xA2;
constexpr uint64_t kDepthVar = 0x501;

// ---- the fake raw layer ----------------------------------------------------------------------------------------------------------
// ReShade's string convention: buffer == nullptr -> the length including the NUL; otherwise copy what fits (the capacity includes the
// NUL), and `*size` becomes the length copied including the NUL.
void CopyString(const std::string& s, char* buffer, size_t* size)
{
    if (buffer == nullptr)
    {
        *size = s.size() + 1;
        return;
    }
    const size_t capacity = *size;
    if (capacity == 0)
        return;
    const size_t n = std::min(capacity - 1, s.size());
    std::memcpy(buffer, s.data(), n);
    buffer[n] = '\0';
    *size = n + 1;
}

struct FakeRaw final : IRawApi
{
    struct Rt
    {
        bool hasDevice = true;
        bool d3d11 = true;
        uint64_t native = kDev;
    };
    struct Tex
    {
        uint64_t handle;
        std::string name;
        std::string semantic; // "DEPTH" reads the DEPTH map
        uint64_t srv = 0, srgb = 0;
    };
    struct Uni
    {
        uint64_t handle;
        bool hasSource;
        std::string source;
        bool value;
    };
    struct Tech
    {
        uint64_t handle;
        std::string effect;
        bool enabled;
    };

    std::map<uint64_t, Rt> runtimes;
    uint32_t bbW = 3840, bbH = 2160;
    std::vector<Tex> textures { { kDepthVar, "ReShade::DepthBufferTex", "DEPTH", 0, 0 } };
    std::vector<Uni> unis { { 0x601, true, "bufready_depth", false } };
    std::vector<Tech> techs { { 0x701, "Main.fx", true } };
    std::map<std::string, std::string> globalDefs;
    std::map<std::string, std::map<std::string, std::string>> effectDefs;
    std::map<uint64_t, uint32_t> viewFormats;
    uint64_t depthSrv = kOrig, depthSrgb = kOrigSrgb; // the DEPTH map of every runtime (one world)
    bool hasDepth = true;

    // views
    std::map<uint64_t, int> owner { { kCopy, 1 }, { kOrig, 1 }, { kOrigSrgb, 1 } };
    std::map<uint64_t, int> core;
    std::set<uint64_t> freed;
    int violations = 0;

    // fault injection and the call log
    int calls = 0, faultAt = 0;
    std::vector<std::string> log;
    std::vector<size_t> firstSizes; // `*size` / buffer presence of the string calls, in order
    bool growBetween = false;       // the second string call returns a longer string
    bool noTerminator = false;      // the second string call leaves the last byte as it is
    bool interiorNul = false;       // ... or puts a NUL inside
    bool hugeSize = false;          // the size query answers an absurd length
    bool zeroSize = false;          // the size query answers "exists, length 0"
    uint32_t updates = 0;

    Rt& R(uint64_t rt) { return runtimes.try_emplace(rt).first->second; }
    bool Hit(const char* name)
    {
        log.push_back(name);
        return ++calls == faultAt;
    }
    bool Alive(uint64_t v) { return owner[v] + core[v] > 0; }

    bool DeviceInfo(uint64_t rt, bool& hasDevice, bool& d3d11, uint64_t& native) override
    {
        if (Hit("DeviceInfo"))
            return false;
        const Rt& r = R(rt);
        hasDevice = r.hasDevice;
        d3d11 = r.d3d11;
        native = r.native;
        return true;
    }
    bool BackBuffer(uint64_t, uint32_t& w, uint32_t& h) override
    {
        if (Hit("BackBuffer"))
            return false;
        w = bbW;
        h = bbH;
        return true;
    }
    bool List(const char* name, size_t n, uint64_t first, uint64_t* out, size_t cap, size_t& count)
    {
        if (Hit(name))
            return false;
        count = n;
        for (size_t i = 0; i < n && i < cap; ++i)
            out[i] = first + i;
        return true;
    }
    size_t extraTextures = 0; // more texture variables than the first capacity (names "Pad<i>")
    size_t listGrowsBy = 0;   // the second enumeration finds this many more (the list changed in between)
    int textureLists = 0;
    bool ListTextures(uint64_t, uint64_t* out, size_t cap, size_t& count) override
    {
        if (Hit("ListTextures"))
            return false;
        count = textures.size() + extraTextures + (++textureLists > 1 ? listGrowsBy : 0);
        for (size_t i = 0; i < count && i < cap; ++i)
            out[i] = i < textures.size() ? textures[i].handle : 0x10000 + i;
        return true;
    }
    bool ListUniforms(uint64_t, uint64_t* out, size_t cap, size_t& count) override
    {
        if (Hit("ListUniforms"))
            return false;
        count = unis.size();
        for (size_t i = 0; i < count && i < cap; ++i)
            out[i] = unis[i].handle;
        return true;
    }
    bool ListTechniques(uint64_t, uint64_t* out, size_t cap, size_t& count) override
    {
        if (Hit("ListTechniques"))
            return false;
        count = techs.size();
        for (size_t i = 0; i < count && i < cap; ++i)
            out[i] = techs[i].handle;
        return true;
    }

    // The string calls: the first one (buffer null) gives the size, the second copies. The knobs bend the second.
    Call Str(const char* name, const std::string& value, char* buffer, size_t* size)
    {
        if (Hit(name))
            return Call::Fault;
        if (buffer == nullptr)
        {
            CopyString(value, buffer, size);
            if (hugeSize)
                *size = kMaxStringBytes + 2;
            if (zeroSize)
                *size = 0;
            return Call::Yes;
        }
        std::string v = growBetween ? value + "XYZ" : value;
        CopyString(v, buffer, size);
        if (noTerminator && *size > 0)
            buffer[*size - 1] = 'Q';
        if (interiorNul && *size > 2)
            buffer[0] = '\0';
        return Call::Yes;
    }
    Call TextureName(uint64_t, uint64_t variable, char* buffer, size_t* size) override
    {
        for (const Tex& t : textures)
            if (t.handle == variable)
                return Str("TextureName", t.name, buffer, size);
        return Str("TextureName", "Pad" + std::to_string(variable - 0x10000), buffer, size);
    }
    Call UniformSource(uint64_t, uint64_t variable, char* buffer, size_t* size) override
    {
        for (const Uni& u : unis)
            if (u.handle == variable)
            {
                if (!u.hasSource)
                    return Hit("UniformSource") ? Call::Fault : Call::No;
                return Str("UniformSource", u.source, buffer, size);
            }
        return Call::Fault;
    }
    Call TechniqueEffect(uint64_t, uint64_t technique, char* buffer, size_t* size) override
    {
        for (const Tech& t : techs)
            if (t.handle == technique)
                return Str("TechniqueEffect", t.effect, buffer, size);
        return Call::Fault;
    }
    std::vector<std::string> definitionScopes; // the effect passed to each Definition call ("" = global)
    Call Definition(uint64_t, const char* effect, const char* name, char* buffer, size_t* size) override
    {
        if (buffer == nullptr)
            definitionScopes.push_back(effect == nullptr ? "" : effect);
        const auto& scope = effect == nullptr ? globalDefs : effectDefs[effect];
        const auto it = scope.find(name);
        if (it == scope.end())
            return Hit("Definition") ? Call::Fault : Call::No;
        if (vanishOnSecond && buffer != nullptr)
            return Call::No;
        return Str("Definition", it->second, buffer, size);
    }
    bool vanishOnSecond = false;
    bool TechniqueState(uint64_t, uint64_t technique, bool& enabled) override
    {
        if (Hit("TechniqueState"))
            return false;
        for (const Tech& t : techs)
            if (t.handle == technique)
                enabled = t.enabled;
        return true;
    }

    bool TextureBinding(uint64_t, uint64_t variable, uint64_t& srv, uint64_t& srvSrgb) override
    {
        if (Hit("TextureBinding"))
            return false;
        for (const Tex& t : textures)
            if (t.handle == variable)
            {
                srv = t.semantic == "DEPTH" ? depthSrv : t.srv;
                srvSrgb = t.semantic == "DEPTH" ? depthSrgb : t.srgb;
                for (uint64_t v : { srv, srvSrgb })
                    if (v != 0 && freed.count(v))
                        ++violations;
                return true;
            }
        return false;
    }
    std::vector<std::string> semantics;
    bool UpdateTextureBindings(uint64_t, const char* semantic, uint64_t srv, uint64_t srvSrgb) override
    {
        if (Hit("UpdateTextureBindings"))
            return false;
        semantics.push_back(semantic);
        ++updates;
        for (uint64_t v : { srv, srvSrgb })
            if (v != 0 && freed.count(v))
                ++violations;
        if (std::string(semantic) == "DEPTH")
        {
            depthSrv = srv;
            depthSrgb = srvSrgb;
            hasDepth = srv != 0 || srvSrgb != 0;
        }
        return true;
    }
    bool GetUniformBool(uint64_t, uint64_t variable, bool& value) override
    {
        if (Hit("GetUniformBool"))
            return false;
        for (const Uni& u : unis)
            if (u.handle == variable)
                value = u.value;
        return true;
    }
    bool SetUniformBool(uint64_t, uint64_t variable, bool value) override
    {
        if (Hit("SetUniformBool"))
            return false;
        for (Uni& u : unis)
            if (u.handle == variable)
                u.value = value;
        return true;
    }

    bool AddRefView(uint64_t view) override
    {
        if (Hit("AddRefView"))
            return false;
        if (!owner.count(view) || !Alive(view))
        {
            ++violations;
            return true;
        }
        ++core[view];
        return true;
    }
    bool ReleaseView(uint64_t view) override
    {
        if (Hit("ReleaseView"))
            return false;
        if (!owner.count(view) || !Alive(view) || core[view] <= 0)
            ++violations;
        else
        {
            --core[view];
            if (!Alive(view))
                freed.insert(view);
        }
        return true;
    }
    bool ViewFormat(uint64_t view, uint32_t& format) override
    {
        if (Hit("ViewFormat"))
            return false;
        const auto it = viewFormats.find(view);
        format = it == viewFormats.end() ? 0 : it->second;
        return true;
    }
    int CoreRefs(uint64_t v) { return core[v]; }
    bool Balanced()
    {
        for (const auto& [v, n] : core)
            if (n != 0)
                return false;
        return violations == 0;
    }
};

int64_t g_now = 5000;
uint64_t g_frame = 77;
uint64_t g_primary = kDev;
int64_t Now() { return g_now; }
uint64_t FrameNow() { return g_frame; }
uint64_t Primary() { return g_primary; }
BackendEnv Env() { return BackendEnv { &Now, &FrameNow, &Primary }; }

size_t LogLines(const char* needle)
{
    size_t n = 0;
    for (const std::string& line : depthtest_log::Lines())
        n += line.find(needle) != std::string::npos ? 1 : 0;
    return n;
}

// ---- strings ---------------------------------------------------------------------------------------------------------------------
void TextureNames()
{
    FakeRaw raw;
    raw.textures.push_back({ 0x502, "", "", 0, 0 });
    raw.textures.push_back({ 0x503, std::string(300, 'n'), "", 0, 0 }); // longer than any small fixed buffer
    raw.textures.push_back({ 0x504, std::string(5000, 'm'), "", 0, 0 });
    LiveDepthBackend b(raw, Env());
    std::vector<TextureVar> out;
    CHECK(b.TextureVariables(kRt, out));
    CHECK(out.size() == 4);
    if (out.size() == 4)
    {
        CHECK(out[0].handle == kDepthVar && out[0].name == "ReShade::DepthBufferTex"); // the NUL is not part of the name
        CHECK(out[0].name.size() == 23);
        CHECK(out[1].handle == 0x502 && out[1].name.empty());
        CHECK(out[2].name == std::string(300, 'n'));  // never cut
        CHECK(out[3].name == std::string(5000, 'm'));
    }
    // the size query comes first for every name: the log alternates ListTextures, then (TextureName x2) per variable
    size_t names = 0;
    for (const std::string& l : raw.log)
        names += l == "TextureName" ? 1 : 0;
    CHECK(names == 8);

    // a list that outgrows the first capacity is read whole
    FakeRaw big;
    big.extraTextures = 1500;
    LiveDepthBackend bb(big, Env());
    std::vector<TextureVar> all;
    CHECK(bb.TextureVariables(kRt, all));
    CHECK(all.size() == 1501);
    if (all.size() == 1501)
    {
        CHECK(all[1500].name == "Pad1500" && all[1500].handle == 0x10000 + 1500);
        CHECK(all[0].handle == kDepthVar);
    }
    // ... but a list that grows between the two reads is a failure, not a cut
    FakeRaw grow;
    grow.extraTextures = 1500;
    grow.listGrowsBy = 1;
    LiveDepthBackend bg(grow, Env());
    std::vector<TextureVar> g;
    CHECK(!bg.TextureVariables(kRt, g) && g.empty());
    // ... up to a limit, beyond which it is a failure, not a cut
    FakeRaw huge;
    huge.extraTextures = kMaxHandles + 1;
    LiveDepthBackend bh(huge, Env());
    std::vector<TextureVar> none { { 1, "stale" } };
    CHECK(!bh.TextureVariables(kRt, none));
    CHECK(none.empty()); // nothing half-filled is left behind
}

void BadStrings()
{
    for (int knob = 0; knob < 6; ++knob)
    {
        FakeRaw raw;
        raw.growBetween = knob == 0;
        raw.noTerminator = knob == 1;
        raw.interiorNul = knob == 2;
        raw.hugeSize = knob == 3;
        raw.zeroSize = knob == 4;
        raw.globalDefs["RESHADE_DEPTH_INPUT_X_SCALE"] = "1.05";
        raw.unis[0].source = "bufready_depth";
        LiveDepthBackend b(raw, Env());
        std::vector<TextureVar> t;
        std::vector<UniformVar> u;
        std::vector<EffectTechnique> e;
        std::string v;
        if (knob < 5)
        {
            // every string of a read that disagrees with itself fails the whole read, and a definition is an error
            CHECK(!b.TextureVariables(kRt, t));
            CHECK(!b.UniformVariables(kRt, u));
            CHECK(!b.Techniques(kRt, e));
            CHECK(b.Definition(kRt, nullptr, "RESHADE_DEPTH_INPUT_X_SCALE", v) == DefinitionState::Error);
            CHECK(b.Definition(kRt, "Main.fx", "X", v) == DefinitionState::Undefined); // nothing defined there: no string read
        }
        else
        {
            // the baseline of the loop: the same world with no knob works
            CHECK(b.TextureVariables(kRt, t) && b.UniformVariables(kRt, u) && b.Techniques(kRt, e));
            CHECK(b.Definition(kRt, nullptr, "RESHADE_DEPTH_INPUT_X_SCALE", v) == DefinitionState::Defined && v == "1.05");
        }
    }
}

void Definitions()
{
    FakeRaw raw;
    raw.globalDefs["A"] = "1.05";
    raw.globalDefs["B"] = "";
    raw.globalDefs["C"] = std::string(400, 'c');
    raw.effectDefs["Main.fx"]["A"] = "0.5";
    LiveDepthBackend b(raw, Env());
    std::string v = "junk";
    CHECK(b.Definition(kRt, nullptr, "A", v) == DefinitionState::Defined && v == "1.05"); // not "1.0"
    CHECK(b.Definition(kRt, nullptr, "B", v) == DefinitionState::Defined && v.empty());   // defined, empty
    CHECK(b.Definition(kRt, nullptr, "C", v) == DefinitionState::Defined && v == std::string(400, 'c'));
    CHECK(b.Definition(kRt, nullptr, "Z", v) == DefinitionState::Undefined);
    CHECK(b.Definition(kRt, "Main.fx", "A", v) == DefinitionState::Defined && v == "0.5"); // the effect's own scope
    CHECK(b.Definition(kRt, "Other.fx", "A", v) == DefinitionState::Undefined);
    raw.globalDefs["HUGE"] = std::string(kMaxStringBytes + 10, 'x'); // too long to read: an error, never a cut value
    CHECK(b.Definition(kRt, nullptr, "HUGE", v) == DefinitionState::Error);
    raw.globalDefs["BIG"] = std::string(kMaxStringBytes - 10, 'x');
    CHECK(b.Definition(kRt, nullptr, "BIG", v) == DefinitionState::Defined && v.size() == kMaxStringBytes - 10);
    CHECK(raw.definitionScopes.size() >= 6);
    if (raw.definitionScopes.size() >= 6)
    {
        CHECK(raw.definitionScopes[0].empty());
        CHECK(raw.definitionScopes[4] == "Main.fx");
    }

    // a fault in the size query, in the second call, or a definition that is gone by the second call: an error, never "undefined"
    for (int n = 1; n <= 2; ++n)
    {
        FakeRaw f;
        f.globalDefs["A"] = "1.05";
        LiveDepthBackend fb(f, Env());
        f.faultAt = n;
        CHECK(fb.Definition(kRt, nullptr, "A", v) == DefinitionState::Error);
        CHECK(fb.Definition(kRt, nullptr, "A", v) == DefinitionState::Defined && v == "1.05"); // the next query is fine
    }
    FakeRaw gone;
    gone.globalDefs["A"] = "1";
    gone.vanishOnSecond = true;
    LiveDepthBackend gb(gone, Env());
    CHECK(gb.Definition(kRt, nullptr, "A", v) == DefinitionState::Error);
    // a fault while asking whether it exists at all
    FakeRaw f;
    LiveDepthBackend fb(f, Env());
    f.faultAt = 1;
    CHECK(fb.Definition(kRt, nullptr, "A", v) == DefinitionState::Error);
}

void UniformsAndTechniques()
{
    FakeRaw raw;
    raw.unis.push_back({ 0x602, false, "", true }); // no source annotation
    raw.unis.push_back({ 0x603, true, "frametime", false });
    raw.techs.push_back({ 0x702, "Other.fx", false });
    LiveDepthBackend b(raw, Env());
    std::vector<UniformVar> u;
    CHECK(b.UniformVariables(kRt, u));
    CHECK(u.size() == 3);
    if (u.size() == 3)
    {
        CHECK(u[0].handle == 0x601 && u[0].source == "bufready_depth");
        CHECK(u[1].handle == 0x602 && u[1].source.empty()); // no annotation: empty, not a failure
        CHECK(u[2].source == "frametime");
    }
    std::vector<EffectTechnique> t;
    CHECK(b.Techniques(kRt, t));
    CHECK(t.size() == 2);
    if (t.size() == 2)
    {
        CHECK(t[0].effect == "Main.fx" && t[0].enabled);
        CHECK(t[1].effect == "Other.fx" && !t[1].enabled);
    }
    bool value = true;
    CHECK(b.GetUniformBool(kRt, 0x601, value) && !value);
    CHECK(b.SetUniformBool(kRt, 0x601, true));
    CHECK(raw.unis[0].value);
    // a faulting read or write is a failure, and the read leaves the caller's value alone
    raw.faultAt = raw.calls + 1;
    value = true;
    CHECK(!b.GetUniformBool(kRt, 0x601, value) && value);
    raw.faultAt = raw.calls + 1;
    CHECK(!b.SetUniformBool(kRt, 0x601, false) && raw.unis[0].value);

    // a fault in any call of a read fails the read and leaves nothing behind; a read that the fault does not reach works
    for (int n = 1; n <= 10; ++n)
    {
        FakeRaw f;
        f.unis.push_back({ 0x602, false, "", true });
        f.techs.push_back({ 0x702, "Other.fx", false });
        LiveDepthBackend fb(f, Env());
        std::vector<UniformVar> uu { { 9, "stale" } };
        f.faultAt = n;
        const bool okU = fb.UniformVariables(kRt, uu);
        const bool hitU = f.calls >= n;
        CHECK(okU == !hitU);
        CHECK(okU ? uu.size() == 2 : uu.empty());

        FakeRaw g;
        g.techs.push_back({ 0x702, "Other.fx", false });
        LiveDepthBackend gb(g, Env());
        std::vector<EffectTechnique> tt { { "stale", true } };
        g.faultAt = n;
        const bool okT = gb.Techniques(kRt, tt);
        const bool hitT = g.calls >= n;
        CHECK(okT == !hitT);
        CHECK(okT ? tt.size() == 2 : tt.empty());
    }
}

// ---- views, runtimes -------------------------------------------------------------------------------------------------------------
void Views()
{
    FakeRaw raw;
    raw.viewFormats[0xD1] = kFmtD32Float;
    raw.viewFormats[0xD2] = kFmtR32Typeless;
    raw.viewFormats[0xD3] = kFmtD24UnormS8;
    raw.viewFormats[0xD4] = 28; // R8G8B8A8_UNORM
    raw.viewFormats[0xD5] = 0;  // not a 2D texture
    raw.viewFormats[0xD6] = kFmtR32Float;
    raw.viewFormats[0xD7] = kFmtD16UnormS8;
    LiveDepthBackend b(raw, Env());
    CHECK(b.IsDepthView(0xD1) && b.IsDepthView(0xD2) && b.IsDepthView(0xD3) && b.IsDepthView(0xD6) && b.IsDepthView(0xD7));
    CHECK(!b.IsDepthView(0xD4) && !b.IsDepthView(0xD5) && !b.IsDepthView(0xD8));
    raw.faultAt = raw.calls + 1;
    CHECK(!b.IsDepthView(0xD1)); // a faulting query is "no": the core distrusts the input, it does not crash
    // the family is the probe's: every value of the shared list, and only those
    for (uint32_t f = 0; f < 120; ++f)
    {
        const bool expected = f == 19 || f == 20 || f == 21 || f == 39 || f == 40 || f == 41 || f == 44 || f == 45 || f == 46 ||
                              f == 47 || f == 53 || f == 55 || f == 56;
        CHECK(IsDepthFormatValue(f) == expected);
    }
    CHECK(IsDepthFormatValue(0x38363144) && !IsDepthFormatValue(0x38343244)); // D168 yes, D248 no (as the probe has it)

    // AddRef / Release
    FakeRaw r2;
    LiveDepthBackend b2(r2, Env());
    b2.AddRef(kCopy);
    CHECK(r2.CoreRefs(kCopy) == 1);
    b2.Release(kCopy);
    CHECK(r2.Balanced());
    r2.faultAt = r2.calls + 1;
    bool threw = false;
    try
    {
        b2.AddRef(kCopy);
    }
    catch (const std::exception&)
    {
        threw = true;
    }
    CHECK(threw && r2.CoreRefs(kCopy) == 0); // a fault in AddRef is not an AddRef
    b2.AddRef(kCopy);
    r2.faultAt = r2.calls + 1;
    threw = false;
    try
    {
        b2.Release(kCopy);
    }
    catch (const std::exception&)
    {
        threw = true;
    }
    CHECK(threw); // ... and a fault in Release says so (the core swallows it)
}

void RuntimeQueries()
{
    FakeRaw raw;
    LiveDepthBackend b(raw, Env());
    CHECK(b.IsD3D11(kRt));
    raw.R(kRt2).d3d11 = false;
    CHECK(!b.IsD3D11(kRt2));
    raw.R(kRt3).hasDevice = false;
    CHECK(!b.IsD3D11(kRt3));
    raw.faultAt = raw.calls + 1;
    CHECK(!b.IsD3D11(kRt));

    uint32_t w = 1, h = 1;
    CHECK(b.BackBufferSize(kRt, w, h) && w == 3840 && h == 2160);
    raw.bbW = 0;
    raw.bbH = 0;
    CHECK(!b.BackBufferSize(kRt, w, h)); // no back buffer (or no 2D texture)
    raw.bbW = 100;
    raw.bbH = 0;
    CHECK(!b.BackBufferSize(kRt, w, h));
    raw.bbW = 1920;
    raw.bbH = 1080;
    raw.faultAt = raw.calls + 1;
    CHECK(!b.BackBufferSize(kRt, w, h));

    // clock and frame come from the environment; without one they are 0
    CHECK(b.NowMs() == 5000 && b.Frame() == 77);
    g_now = 9000;
    g_frame = 80;
    CHECK(b.NowMs() == 9000 && b.Frame() == 80);
    LiveDepthBackend none(raw, BackendEnv {});
    CHECK(none.NowMs() == 0 && none.Frame() == 0);
    g_now = 5000;
    g_frame = 77;
}

void SameDeviceSemantics()
{
    g_primary = kDev;
    { // an existing capture: native devices must be equal
        FakeRaw raw;
        LiveDepthBackend b(raw, Env());
        CHECK(b.SameDevice(kRt, kDev));
        CHECK(!b.SameDevice(kRt, kDev2));
        raw.R(kRt).native = kDev2;
        CHECK(!b.SameDevice(kRt, kDev)); // a different device is refused also when it is the only runtime (never bind another device's view)
        CHECK(b.SameDevice(kRt, kDev2));
    }
    { // captureDevice 0 = the game's primary device
        FakeRaw raw;
        LiveDepthBackend b(raw, Env());
        CHECK(b.SameDevice(kRt, 0)); // equal
        raw.R(kRt).native = kDev2;
        CHECK(b.SameDevice(kRt, 0)); // differs, but it is the only D3D11 runtime (the boundary's fallback)
        b.NoteRuntime(kRt);
        CHECK(b.SameDevice(kRt, 0));
        b.NoteRuntime(kRt2); // a second D3D11 runtime: the fallback is gone
        CHECK(!b.SameDevice(kRt, 0));
        raw.R(kRt2).native = kDev; // ... unless that one is the primary device's, which is then the right answer for kRt2 only
        CHECK(b.SameDevice(kRt2, 0) && !b.SameDevice(kRt, 0));
        b.ForgetRuntime(kRt2);
        CHECK(b.SameDevice(kRt, 0)); // the fallback is back
    }
    { // a runtime that is not D3D11 never counts, and never is a candidate
        FakeRaw raw;
        raw.R(kRt2).d3d11 = false;
        LiveDepthBackend b(raw, Env());
        raw.R(kRt).native = kDev2;
        b.NoteRuntime(kRt);
        b.NoteRuntime(kRt2); // not D3D11: not remembered
        CHECK(b.SameDevice(kRt, 0));
        CHECK(!b.SameDevice(kRt2, 0));
        CHECK(!b.SameDevice(kRt2, kDev));
    }
    { // the primary device unknown: the fallback decides
        g_primary = 0;
        FakeRaw raw;
        raw.R(kRt).native = kDev2;
        LiveDepthBackend b(raw, Env());
        CHECK(b.SameDevice(kRt, 0));
        b.NoteRuntime(kRt);
        b.NoteRuntime(kRt2);
        CHECK(!b.SameDevice(kRt, 0));
        g_primary = kDev;
    }
    { // no device, a fault
        FakeRaw raw;
        LiveDepthBackend b(raw, Env());
        raw.R(kRt).hasDevice = false;
        CHECK(!b.SameDevice(kRt, 0) && !b.SameDevice(kRt, kDev));
        raw.R(kRt).hasDevice = true;
        raw.faultAt = raw.calls + 1;
        CHECK(!b.SameDevice(kRt, kDev));
    }
    { // the fallback says so once
        FakeRaw raw;
        raw.R(kRt).native = kDev2;
        LiveDepthBackend b(raw, Env());
        const size_t lines = LogLines("only D3D11 runtime");
        CHECK(b.SameDevice(kRt, 0) && b.SameDevice(kRt, 0) && b.SameDevice(kRt, 0));
        CHECK(LogLines("only D3D11 runtime") == lines + 1);
    }
}

void RegistryAndOurRuntime()
{
    g_primary = kDev;
    FakeRaw raw;
    LiveDepthBackend b(raw, Env());
    // IsOurRuntime follows the capture device that was set
    CHECK(b.IsOurRuntime(kRt)); // no capture: the primary device
    b.SetCaptureDevice(kDev);
    CHECK(b.IsOurRuntime(kRt));
    b.SetCaptureDevice(kDev2);
    CHECK(!b.IsOurRuntime(kRt));
    b.SetCaptureDevice(0);
    CHECK(b.IsOurRuntime(kRt));
    raw.R(kRt).d3d11 = false;
    CHECK(!b.IsOurRuntime(kRt));
    raw.R(kRt).d3d11 = true;
    raw.faultAt = raw.calls + 1;
    CHECK(!b.IsOurRuntime(kRt));

    // noting the same runtime twice remembers it once (a second runtime appears only for another pointer)
    FakeRaw r2;
    r2.R(kRt).native = kDev2;
    LiveDepthBackend b2(r2, Env());
    b2.NoteRuntime(kRt);
    b2.NoteRuntime(kRt);
    b2.NoteRuntime(kRt);
    CHECK(b2.SameDevice(kRt, 0));
    b2.ForgetRuntime(kRt);
    b2.ForgetRuntime(kRt); // forgetting what is not there is fine
    CHECK(b2.SameDevice(kRt, 0)); // not remembered, still the only one
    // a fault while a runtime is looked at does not remember it
    FakeRaw r3;
    LiveDepthBackend b3(r3, Env());
    r3.faultAt = r3.calls + 1;
    b3.NoteRuntime(kRt);
    r3.R(kRt).native = kDev2;
    r3.R(kRt2).native = kDev2;
    b3.NoteRuntime(kRt2);
    CHECK(b3.SameDevice(kRt2, 0)); // kRt was not remembered, so kRt2 is the only one
}

// ---- the first bind --------------------------------------------------------------------------------------------------------------
void FirstBindReadback()
{
    FakeRaw raw;
    raw.textures.push_back({ 0x502, "DepthBufferTex", "DEPTH", 0, 0 });
    raw.textures.push_back({ 0x503, "DepthInputTex", "DEPTH", 0, 0 });
    raw.textures.push_back({ 0x504, "Other", "DEPTH", 0, 0 });
    raw.textures.push_back({ 0x505, "Another", "DEPTH", 0, 0 });
    raw.textures.push_back({ 0x506, "AndMore", "DEPTH", 0, 0 });
    LiveDepthBackend b(raw, Env());
    uint64_t s = 0, g = 0;
    const size_t before = LogLines("first bind");
    // reads before any bind are not logged
    CHECK(b.TextureBinding(kRt, kDepthVar, s, g));
    CHECK(LogLines("first bind") == before);
    // a bind of the copy arms it: the next readbacks of that runtime are logged, at most four
    CHECK(b.UpdateTextureBindings(kRt, "DEPTH", kCopy, kCopy));
    CHECK(b.TextureBinding(kRt, kDepthVar, s, g) && s == kCopy && g == kCopy);
    CHECK(LogLines("first bind") == before + 1);
    for (uint64_t v : { 0x502u, 0x503u, 0x504u, 0x505u, 0x506u })
        CHECK(b.TextureBinding(kRt, v, s, g));
    CHECK(LogLines("first bind") == before + 4);
    // the line says what ReShade answered, and what was bound
    bool said = false;
    for (const std::string& l : depthtest_log::Lines())
        said = said || (l.find("first bind") != std::string::npos && l.find("0xc0") != std::string::npos);
    CHECK(said);
    { // ... both: a variable that reads back something else than the bound view
        FakeRaw other;
        other.textures.push_back({ 0x507, "NoSemantic", "", 0xA7, 0xA8 });
        LiveDepthBackend ob(other, Env());
        CHECK(ob.UpdateTextureBindings(kRt, "DEPTH", kCopy, kCopy));
        // a read of another runtime during the observation is not logged
        const size_t quiet = LogLines("first bind");
        CHECK(ob.TextureBinding(kRt2, 0x507, s, g));
        CHECK(LogLines("first bind") == quiet);
        CHECK(ob.TextureBinding(kRt, 0x507, s, g));
        bool both = false;
        for (const std::string& l : depthtest_log::Lines())
            both = both || (l.find("first bind") != std::string::npos && l.find("0xa7") != std::string::npos && l.find("0xa8") != std::string::npos &&
                            l.find("bound 0xc0") != std::string::npos);
        CHECK(both);
        CHECK(LogLines("first bind") == quiet + 1);
    }
    const size_t afterBlock = LogLines("first bind"); // (that backend's line is in the shared log)
    // only the first bind of the process: a later one logs nothing
    CHECK(b.UpdateTextureBindings(kRt, "DEPTH", kOrig, kOrigSrgb)); // the restore
    CHECK(b.UpdateTextureBindings(kRt, "DEPTH", kCopy, kCopy));
    CHECK(b.TextureBinding(kRt, kDepthVar, s, g));
    CHECK(LogLines("first bind") == afterBlock);

    // a first bind that is undone by an erase before anything was read: no log, and the next real bind is still the first
    FakeRaw r2;
    LiveDepthBackend b2(r2, Env());
    const size_t lines = LogLines("first bind");
    CHECK(b2.UpdateTextureBindings(kRt, "DEPTH", kCopy, kCopy));
    CHECK(b2.UpdateTextureBindings(kRt, "DEPTH", 0, 0));
    CHECK(b2.TextureBinding(kRt, kDepthVar, s, g));
    CHECK(LogLines("first bind") == lines);
    CHECK(b2.UpdateTextureBindings(kRt, "DEPTH", kCopy, kCopy));
    CHECK(b2.TextureBinding(kRt, kDepthVar, s, g));
    CHECK(LogLines("first bind") == lines + 1);
    // another semantic is not a depth bind
    FakeRaw r3;
    LiveDepthBackend b3(r3, Env());
    const size_t l3 = LogLines("first bind");
    CHECK(b3.UpdateTextureBindings(kRt, "COLOR", kCopy, kCopy));
    CHECK(b3.TextureBinding(kRt, kDepthVar, s, g));
    CHECK(LogLines("first bind") == l3);
}

// ---- the binder on top -----------------------------------------------------------------------------------------------------------
struct Rig
{
    FakeRaw raw;
    LiveDepthBackend backend { raw, Env() };
    DepthBinder binder { backend, nullptr };
    Rig()
    {
        raw.viewFormats[kOrig] = kFmtD32Float; // the original depth views of ReShade's map
        raw.viewFormats[kOrigSrgb] = kFmtD32Float;
        g_primary = kDev;
        binder.SetEnabled(true);
        Publish();
    }
    void Publish()
    {
        DepthCapture c;
        c.valid = true;
        c.outputW = 3840;
        c.outputH = 2160;
        c.renderW = 1920;
        c.renderH = 1080;
        c.frame = g_frame;
        c.tickMs = g_now;
        c.copyView = kCopy;
        c.device = kDev;
        binder.PublishCapture(c);
    }
    void NextFrame()
    {
        ++g_frame;
        g_now += 16;
        Publish();
    }
    bool Pass()
    {
        NextFrame();
        const bool bound = binder.OnBeginEffects(kRt) == Outcome::Bound;
        binder.OnFinishEffects(kRt);
        return bound;
    }
    bool Clean() { return !(raw.depthSrv == kCopy || raw.depthSrgb == kCopy) && raw.Balanced(); }
};

void BinderOverTheBackend()
{
    { // a whole pass through the marshalling
        Rig r;
        CHECK(r.Pass());
        CHECK(r.Clean() && r.raw.depthSrv == kOrig && r.raw.depthSrgb == kOrigSrgb);
        CHECK(r.binder.GetCounters().passesBound == 1);
        CHECK(r.raw.unis[0].value == false); // bufready_depth put back
    }
    { // the strings decide: a neutral macro, then a 1.05 one (a fixed buffer would have cut it to 1.0)
        Rig r;
        r.raw.globalDefs["RESHADE_DEPTH_INPUT_X_SCALE"] = "1.0";
        CHECK(r.Pass());
        Rig q;
        q.raw.globalDefs["RESHADE_DEPTH_INPUT_X_SCALE"] = "1.05";
        CHECK(!q.Pass());
        CHECK(q.binder.GetCounters().manualTransformStandDowns == 1);
        Rig p;
        p.raw.effectDefs["Main.fx"]["RESHADE_DEPTH_INPUT_Y_OFFSET"] = std::string(80, '0') + "1"; // a long value is read whole
        CHECK(!p.Pass());
    }
    { // a real runtime for another device is not a consumer
        Rig r;
        r.raw.R(kRt).native = kDev2;
        r.binder.OnDestroyRuntime(kRt); // (nothing to forget)
        DepthCapture c;
        c.valid = true;
        c.outputW = 3840;
        c.outputH = 2160;
        c.renderW = 1920;
        c.renderH = 1080;
        c.frame = g_frame;
        c.tickMs = g_now;
        c.copyView = kCopy;
        c.device = kDev; // the capture's device is a real one: the runtime's differs
        r.binder.PublishCapture(c);
        CHECK(r.binder.OnBeginEffects(kRt) == Outcome::OtherDevice);
    }
}

// A fault at every raw call of a pass (the real layer turns an access violation into this): ReShade's map never keeps our copy for good,
// no freed view is touched, and the pass works again afterwards
void FaultAtEveryRawCall()
{
    int total = 0;
    {
        Rig r;
        r.NextFrame();
        const int base = r.raw.calls;
        r.binder.OnBeginEffects(kRt);
        r.binder.OnFinishEffects(kRt);
        total = r.raw.calls - base;
    }
    CHECK(total > 15);
    for (int n = 1; n <= total + 2; ++n)
    {
        Rig r;
        r.NextFrame();
        r.raw.calls = 0;
        r.raw.faultAt = n;
        r.binder.OnBeginEffects(kRt);
        r.binder.OnFinishEffects(kRt);
        r.raw.faultAt = 0;
        // the next passes repair whatever the fault left (a dirty runtime retries first)
        r.binder.OnFinishEffects(kRt);
        for (int i = 0; i < 3; ++i)
            r.Pass();
        const bool holdsCopy = r.raw.depthSrv == kCopy || r.raw.depthSrgb == kCopy;
        if (holdsCopy || r.raw.violations != 0)
            std::printf("fault at raw call %d: holdsCopy %d violations %d\n", n, holdsCopy ? 1 : 0, r.raw.violations);
        CHECK(!holdsCopy);
        CHECK(r.raw.violations == 0);
        // every reference taken is back, unless the call that gives one back is the one that faulted (a leak, never a double release)
        const std::string faulted = static_cast<size_t>(n) <= r.raw.log.size() ? r.raw.log[n - 1] : std::string();
        if (faulted != "ReleaseView")
            CHECK(r.raw.Balanced());
    }
}
} // namespace

int main()
{
    TextureNames();
    BadStrings();
    Definitions();
    UniformsAndTechniques();
    Views();
    RuntimeQueries();
    SameDeviceSemantics();
    RegistryAndOurRuntime();
    FirstBindReadback();
    BinderOverTheBackend();
    FaultAtEveryRawCall();
    if (g_fail != 0)
    {
        std::printf("reshade_depth_backend: %d check(s) failed\n", g_fail);
        return 1;
    }
    std::puts("reshade_depth_backend: all checks passed");
    return 0;
}
