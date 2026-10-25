// Automatic ReShade scene depth, the live backend (ReShadeDepthBackend.h) over a FAKE raw layer: the size-query-first string reading
// (the NUL stripped, never a truncated value), the tri-state definition, the handle lists that outgrow the first capacity, the
// untrusted views (a faulting AddRef / Release / format query), SameDevice (native devices, the only-runtime fallback, the capture
// device 0), the registry behind the frame counter, the first-bind readback log, and the real binder on top with a fault injected at
// every raw call of a pass. The real raw layer (ReShadeDepthRaw.cpp, SEH thunks over effect_runtime) is covered by reshade_depth_raw.cpp
// (garbage handles, exceptions, the argument wiring, against a fake runtime derived from ReShade's abstract class); what real ReShade
// answers is not covered by any test and is what the logs of the first run in the game settle.
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
// ReShade's string convention, as OBSERVED in game 2026-10-25 (ReShade 6.8.0.2155, std::string::copy semantics): buffer == nullptr ->
// the length + 1 (the NUL counted); otherwise copy what fits (the capacity includes the NUL: min(capacity - 1, length) characters, then
// the NUL), and `*size` becomes the NUMBER OF CHARACTERS COPIED, without the NUL. The second mode is the vendored header's wording
// (`*size` becomes the count including the NUL); the backend accepts both, and every test runs under both.
bool g_headerWording = false;

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
    *size = g_headerWording ? n + 1 : n;
}

struct FakeRaw final : IRawApi
{
    struct Rt
    {
        bool hasDevice = true;
        bool d3d11 = true;
        uint64_t native = kDev;
        uint64_t backDevice = 0; // the device of the back buffer's resource; 0 = the same as `native`
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
    std::vector<Tex> textures { { kDepthVar, "DepthBufferTex", "DEPTH", 0, 0 } };
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
    bool shrinkBetween = false;     // the second string call returns a shorter string (the last character gone)
    bool writesNothing = false;     // the second string call answers "yes" and leaves the buffer as it is
    bool lyingSize = false;         // the second string call reports more than the capacity
    std::vector<size_t> needs, capacities; // the size query answers, and the capacities of the copies
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
    int backDeviceCalls = 0;
    bool BackBufferDevice(uint64_t rt, uint64_t& device) override
    {
        ++backDeviceCalls;
        if (Hit("BackBufferDevice"))
            return false;
        const Rt& r = R(rt);
        device = r.backDevice != 0 ? r.backDevice : r.native;
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
            needs.push_back(*size);
            if (hugeSize)
                *size = kMaxStringBytes + 2;
            if (zeroSize)
                *size = 0;
            return Call::Yes;
        }
        capacities.push_back(*size);
        if (writesNothing)
            return Call::Yes;
        std::string v = growBetween ? value + "XYZ" : shrinkBetween && !value.empty() ? value.substr(0, value.size() - 1) : value;
        CopyString(v, buffer, size);
        if (noTerminator)
            buffer[std::strlen(buffer)] = 'Q';
        if (interiorNul && std::strlen(buffer) > 1)
            buffer[0] = '\0';
        if (lyingSize)
            *size = capacities.back() + 5;
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

std::string LastLine(const char* needle)
{
    std::string found;
    for (const std::string& l : depthtest_log::Lines())
        if (l.find(needle) != std::string::npos)
            found = l;
    return found;
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
        CHECK(out[0].handle == kDepthVar && out[0].name == "DepthBufferTex"); // the NUL is not part of the name
        CHECK(out[0].name.size() == 14);
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

// The contract in numbers: the backend asks for `need` (length + 1), copies into need + 1 bytes, and takes the string if it ends exactly
// at need - 1. Both wordings of the second size are accepted; a string that came back different is not.
void StringContract()
{
    { // the capacity of every copy is the query's answer plus one, and the value is whole (also an empty one)
        FakeRaw raw;
        raw.textures.push_back({ 0x502, "", "", 0, 0 });
        raw.textures.push_back({ 0x503, "a", "", 0, 0 });
        raw.textures.push_back({ 0x504, std::string(kMaxStringBytes - 1, 'z'), "", 0, 0 }); // need == the limit: accepted
        LiveDepthBackend b(raw, Env());
        std::vector<TextureVar> t;
        CHECK(b.TextureVariables(kRt, t) && t.size() == 4);
        if (t.size() == 4)
            CHECK(t[1].name.empty() && t[2].name == "a" && t[3].name.size() == kMaxStringBytes - 1);
        CHECK(raw.needs.size() == 4 && raw.capacities.size() == 4);
        for (size_t i = 0; i < raw.needs.size() && i < raw.capacities.size(); ++i)
            CHECK(raw.capacities[i] == raw.needs[i] + 1);
        CHECK(raw.needs.size() == 4 && raw.needs[1] == 1 && raw.needs[2] == 2); // an empty string asks for 1 (just the NUL)
    }
    { // one character more than the limit allows: a failure, not a cut
        FakeRaw raw;
        raw.textures.push_back({ 0x502, std::string(kMaxStringBytes, 'z'), "", 0, 0 }); // need == limit + 1
        LiveDepthBackend b(raw, Env());
        std::vector<TextureVar> t;
        CHECK(!b.TextureVariables(kRt, t));
        CHECK(raw.capacities.size() == 1); // only the first variable's copy; the long one was refused after its query
    }
    { // an interior NUL cannot be a ReShade name: the C string would be shorter than the query said
        FakeRaw raw;
        raw.textures.push_back({ 0x502, std::string("ab\0cd", 5), "", 0, 0 });
        LiveDepthBackend b(raw, Env());
        std::vector<TextureVar> t;
        const size_t lines = LogLines("string contract");
        CHECK(!b.TextureVariables(kRt, t) && t.empty());
        CHECK(LogLines("string contract") == lines + 1);
        const std::string line = LastLine("string contract");
        CHECK(line.find("first size 6") != std::string::npos && line.find("string length 2") != std::string::npos);
    }
    { // a copy that writes nothing leaves an empty C string: only the empty value is acceptable that way
        FakeRaw raw;
        raw.writesNothing = true;
        LiveDepthBackend b(raw, Env());
        std::vector<TextureVar> t;
        CHECK(!b.TextureVariables(kRt, t));
        FakeRaw empty;
        empty.writesNothing = true;
        empty.textures = { { 0x502, "", "", 0, 0 } };
        LiveDepthBackend e(empty, Env());
        CHECK(e.TextureVariables(kRt, t) && t.size() == 1 && t[0].name.empty());
    }
    { // a size that exceeds the capacity is a lie, whatever the buffer holds
        FakeRaw raw;
        raw.lyingSize = true;
        LiveDepthBackend b(raw, Env());
        std::vector<TextureVar> t;
        CHECK(!b.TextureVariables(kRt, t));
        const std::string line = LastLine("string contract");
        CHECK(line.find("answers disagree") != std::string::npos);
    }
    { // grown / shrunk between the two calls
        for (int shrink = 0; shrink < 2; ++shrink)
        {
            FakeRaw raw;
            raw.growBetween = shrink == 0;
            raw.shrinkBetween = shrink == 1;
            LiveDepthBackend b(raw, Env());
            std::vector<TextureVar> t;
            CHECK(!b.TextureVariables(kRt, t));
        }
    }
    { // a definition: defined with an empty value is defined; the neutral value is read whole
        FakeRaw raw;
        raw.globalDefs["E"] = "";
        raw.globalDefs["ONE"] = "1";
        LiveDepthBackend b(raw, Env());
        std::string v = "junk";
        CHECK(b.Definition(kRt, nullptr, "E", v) == DefinitionState::Defined && v.empty());
        CHECK(b.Definition(kRt, nullptr, "ONE", v) == DefinitionState::Defined && v == "1");
    }
}

void BadStrings()
{
    for (int knob = 0; knob < 9; ++knob)
    {
        FakeRaw raw;
        raw.growBetween = knob == 0;
        raw.noTerminator = knob == 1;
        raw.interiorNul = knob == 2;
        raw.hugeSize = knob == 3;
        raw.zeroSize = knob == 4;
        raw.shrinkBetween = knob == 5;
        raw.writesNothing = knob == 6;
        raw.lyingSize = knob == 7;
        raw.globalDefs["RESHADE_DEPTH_INPUT_X_SCALE"] = "1.05";
        raw.unis[0].source = "bufready_depth";
        LiveDepthBackend b(raw, Env());
        std::vector<TextureVar> t;
        std::vector<UniformVar> u;
        std::vector<EffectTechnique> e;
        std::string v;
        if (knob < 8)
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
    gone.globalDefs["E"] = ""; // an empty value that is gone by the second call: the empty buffer must not read as "defined, empty"
    CHECK(gb.Definition(kRt, nullptr, "E", v) == DefinitionState::Error);
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
    { // an existing capture: equal devices match; with a second runtime around, a different device is refused
        FakeRaw raw;
        LiveDepthBackend b(raw, Env());
        b.NoteRuntime(kRt);
        b.NoteRuntime(kRt2);
        CHECK(b.SameDevice(kRt, kDev));
        CHECK(!b.SameDevice(kRt, kDev2));
        raw.R(kRt).native = kDev2;
        CHECK(!b.SameDevice(kRt, kDev)); // two runtimes: the strict comparison stands
        CHECK(b.SameDevice(kRt, kDev2));
    }
    { // the ONLY D3D11 runtime is the game's, whatever pointers say (ReShade's proxy and native device never compare equal)
        FakeRaw raw;
        LiveDepthBackend b(raw, Env());
        raw.R(kRt).native = kDev2;
        CHECK(b.SameDevice(kRt, kDev)); // with a capture, too
        b.NoteRuntime(kRt);
        CHECK(b.SameDevice(kRt, kDev));
        b.NoteRuntime(kRt2); // a second D3D11 runtime: the fallback is gone
        CHECK(!b.SameDevice(kRt, kDev));
        b.ForgetRuntime(kRt2);
        CHECK(b.SameDevice(kRt, kDev));
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

// The device identity that counts is the device of real resources: the runtime's back buffer against the capture (the game's depth
// texture). ReShade's get_native() came back as its own wrapper object in game (0x228aa25f390 against the game's 0x228b20940d0).
void SameDeviceByResources()
{
    constexpr uint64_t kReal = 0xF390; // what get_native() answers: the REAL device, never the pointer the game sees
    g_primary = kDev;
    { // the log of 2026-10-25: the capture carries the proxy (kDev); get_native() is the real device; the back buffer answers the proxy
        FakeRaw raw;
        raw.R(kRt).native = kReal;
        raw.R(kRt).backDevice = kDev;
        LiveDepthBackend b(raw, Env());
        b.NoteRuntime(kRt);
        b.NoteRuntime(kRt2); // (two runtimes: no fallback, so the match below is the back buffer's)
        raw.R(kRt2).native = kDev2;
        const size_t lines = LogLines("device mismatch");
        CHECK(b.SameDevice(kRt, kDev));
        CHECK(!b.SameDevice(kRt, kDev2));   // another capture device is refused
        CHECK(LogLines("device mismatch") == lines + 1);
        CHECK(b.SameDevice(kRt, 0));
    }
    { // a capture device that is a proxy-like pointer, different from get_native(), and a back buffer that says the real device: the
      // native device matches nothing here, the back buffer's device (the real one) is the capture's? no: the capture says the proxy
        FakeRaw raw;
        raw.R(kRt).native = kReal;
        raw.R(kRt).backDevice = kReal; // the back buffer answered the real device this time
        LiveDepthBackend b(raw, Env());
        b.NoteRuntime(kRt);
        b.NoteRuntime(kRt2);
        CHECK(!b.SameDevice(kRt, kDev)); // proxy capture, native back buffer, two runtimes: refused
        CHECK(b.SameDevice(kRt, kReal)); // a capture made on the real device matches get_native() and the back buffer
    }
    { // equal by get_native() alone still counts (and then the resource is not even asked)
        FakeRaw raw;
        raw.R(kRt).native = kDev;
        raw.R(kRt).backDevice = kDev2;
        LiveDepthBackend b(raw, Env());
        CHECK(b.SameDevice(kRt, kDev));
        CHECK(raw.backDeviceCalls == 0);
    }
    { // neither matches and there are two runtimes: refused, with ALL the pointers in the one line
        FakeRaw raw;
        raw.R(kRt).native = kReal;
        raw.R(kRt).backDevice = kDev2;
        LiveDepthBackend b(raw, Env());
        b.NoteRuntime(kRt);
        b.NoteRuntime(kRt2);
        CHECK(!b.OnlyD3D11Runtime(kRt) && !b.OnlyD3D11Runtime(kRt2));
        const size_t lines = LogLines("device mismatch");
        CHECK(!b.SameDevice(kRt, kDev) && !b.SameDevice(kRt, kDev));
        CHECK(LogLines("device mismatch") == lines + 1);
        const std::string line = LastLine("device mismatch");
        CHECK(line.find("0xf390") != std::string::npos && line.find("0xd3d1") != std::string::npos && line.find("0xd3d0") != std::string::npos);
        CHECK(line.find("primary 0xd3d0") != std::string::npos);
        // before any capture a refusal is logged all the same (once per backend)
        FakeRaw raw2;
        raw2.R(kRt).native = kReal;
        raw2.R(kRt).backDevice = kDev2;
        LiveDepthBackend b2(raw2, Env());
        b2.NoteRuntime(kRt);
        b2.NoteRuntime(kRt2);
        const size_t before = LogLines("device mismatch");
        CHECK(!b2.SameDevice(kRt, 0));
        CHECK(LogLines("device mismatch") == before + 1);
        CHECK(!b2.OnlyD3D11Runtime(kRt3));
    }
    { // neither matches and it is the only runtime: taken (one device, one proxy/native pair), the line says all the pointers, once
        FakeRaw raw;
        raw.R(kRt).native = kReal;
        raw.R(kRt).backDevice = kDev2;
        LiveDepthBackend b(raw, Env());
        b.NoteRuntime(kRt);
        CHECK(b.OnlyD3D11Runtime(kRt));
        const size_t lines = LogLines("only D3D11 runtime");
        const size_t refusals = LogLines("device mismatch");
        CHECK(b.SameDevice(kRt, kDev) && b.SameDevice(kRt, kDev) && b.SameDevice(kRt, 0));
        CHECK(LogLines("only D3D11 runtime") == lines + 1 && LogLines("device mismatch") == refusals);
        const std::string line = LastLine("only D3D11 runtime");
        CHECK(line.find("0xf390") != std::string::npos && line.find("0xd3d1") != std::string::npos && line.find("0xd3d0") != std::string::npos);
    }
    { // no capture yet: the primary device matched through the back buffer needs no fallback (and says nothing about one)
        FakeRaw raw;
        raw.R(kRt).native = kReal;
        raw.R(kRt).backDevice = kDev;
        LiveDepthBackend b(raw, Env());
        b.NoteRuntime(kRt);
        b.NoteRuntime(kRt2);
        raw.R(kRt2).native = kReal;
        raw.R(kRt2).backDevice = kDev2;
        CHECK(b.SameDevice(kRt, 0));   // by the resource's device, although a second runtime exists
        CHECK(!b.SameDevice(kRt2, 0)); // a runtime of another device with a rival: no fallback
    }
    { // a faulting back buffer query is "unknown": the rest decides (never a crash, never a match)
        FakeRaw raw;
        raw.R(kRt).native = kReal;
        raw.R(kRt).backDevice = kDev;
        LiveDepthBackend b(raw, Env());
        b.NoteRuntime(kRt);
        b.NoteRuntime(kRt2);
        raw.faultAt = raw.calls + 2; // DeviceInfo is the next call, BackBufferDevice the one after
        CHECK(!b.SameDevice(kRt, kDev));
        CHECK(b.SameDevice(kRt, kDev)); // and the next query is fine
    }
}

void Registry()
{
    g_primary = kDev;
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

// ---- one-shot logs that settle what the headers cannot ----------------------------------------------------------------------------

// The first string that breaks ReShade's size contract is described once: the query kind, both sizes and the length found
void StringContractLog()
{
    { // a string that grows between the two calls: first size 15 (the query), the copy has one byte more than that and writes 15
      // characters (the second size says 15 characters, or 16 in the header's wording), the length found is 15
        FakeRaw raw;
        raw.growBetween = true;
        LiveDepthBackend b(raw, Env());
        const size_t before = LogLines("string contract");
        std::vector<TextureVar> t;
        std::vector<UniformVar> u;
        CHECK(!b.TextureVariables(kRt, t));
        CHECK(!b.UniformVariables(kRt, u));
        CHECK(!b.TextureVariables(kRt, t));
        CHECK(LogLines("string contract") == before + 1); // the first one only
        const std::string line = LastLine("string contract");
        CHECK(line.find("texture name") != std::string::npos);
        CHECK(line.find("first size 15") != std::string::npos);
        CHECK(line.find(g_headerWording ? "second size 16" : "second size 15") != std::string::npos);
        CHECK(line.find("string length 15") != std::string::npos);
    }
    { // a call that faults: no sizes to report, and it says so
        FakeRaw raw;
        LiveDepthBackend b(raw, Env());
        const size_t before = LogLines("string contract");
        raw.faultAt = 2; // ListTextures is call 1, the size query of the first name call 2
        std::vector<TextureVar> t;
        CHECK(!b.TextureVariables(kRt, t));
        CHECK(LogLines("string contract") == before + 1);
        const std::string line = LastLine("string contract");
        CHECK(line.find("texture name") != std::string::npos && line.find("call faulted") != std::string::npos);
    }
    { // each kind says its own name; a definition that is merely undefined is not a broken contract
        FakeRaw raw;
        raw.growBetween = true;
        raw.globalDefs["A"] = "1";
        LiveDepthBackend b(raw, Env());
        std::string v;
        const size_t before = LogLines("string contract");
        CHECK(b.Definition(kRt, nullptr, "Z", v) == DefinitionState::Undefined);
        CHECK(LogLines("string contract") == before);
        CHECK(b.Definition(kRt, nullptr, "A", v) == DefinitionState::Error);
        CHECK(LogLines("string contract") == before + 1 && LastLine("string contract").find("definition") != std::string::npos);
        FakeRaw r2;
        r2.growBetween = true;
        LiveDepthBackend b2(r2, Env());
        std::vector<UniformVar> u;
        CHECK(!b2.UniformVariables(kRt, u) && LastLine("string contract").find("uniform source") != std::string::npos);
        FakeRaw r3;
        r3.growBetween = true;
        LiveDepthBackend b3(r3, Env());
        std::vector<EffectTechnique> e;
        CHECK(!b3.Techniques(kRt, e) && LastLine("string contract").find("technique effect") != std::string::npos);
    }
}

// The first refusal of a runtime by a device that differs once a capture exists: both devices and the primary one, once
void SameDeviceRefusalLog()
{
    g_primary = kDev;
    FakeRaw raw;
    LiveDepthBackend b(raw, Env());
    b.NoteRuntime(kRt);
    b.NoteRuntime(kRt2); // the strict comparison needs a rival: the only runtime is taken whatever the pointers say
    const size_t before = LogLines("device mismatch");
    CHECK(b.SameDevice(kRt, kDev)); // equal: nothing to say
    CHECK(LogLines("device mismatch") == before);
    CHECK(!b.SameDevice(kRt, kDev2));
    CHECK(!b.SameDevice(kRt, kDev2));
    CHECK(LogLines("device mismatch") == before + 1);
    const std::string line = LastLine("device mismatch");
    CHECK(line.find("0xd3d0") != std::string::npos && line.find("0xd3d1") != std::string::npos); // the runtime's device, the capture's
    CHECK(line.find("primary 0xd3d0") != std::string::npos);
    // the fallback for the only runtime is not a refusal
    FakeRaw one;
    one.R(kRt).native = kDev2;
    LiveDepthBackend o(one, Env());
    o.NoteRuntime(kRt);
    const size_t after = LogLines("device mismatch");
    CHECK(o.SameDevice(kRt, 0) && o.SameDevice(kRt, kDev));
    CHECK(LogLines("device mismatch") == after);
}

// A runtime without a depth input candidate: the names that contain "depth" (any case), at most eight, and a few lines per process
void NoCandidateLog()
{
    FakeRaw raw;
    LiveDepthBackend b(raw, Env());
    std::vector<TextureVar> names;
    for (int i = 0; i < 12; ++i)
        names.push_back({ 0x900u + i, "ao_Depth_" + std::to_string(i) });
    names.push_back({ 0x950, "SomethingElse" });
    names.push_back({ 0x951, "MXAODEPTH" });
    const size_t before = LogLines("no known depth input");
    b.NoDepthCandidate(kRt, names);
    CHECK(LogLines("no known depth input") == before + 1);
    const std::string line = LastLine("no known depth input");
    CHECK(line.find("ao_Depth_0") != std::string::npos && line.find("ao_Depth_7") != std::string::npos); // the first eight
    CHECK(line.find("ao_Depth_8") == std::string::npos);                                                  // ... not more
    CHECK(line.find("SomethingElse") == std::string::npos);
    CHECK(line.find("14 texture variables") != std::string::npos && line.find("13 contain") != std::string::npos);
    // a case-insensitive match that comes after the cap is counted but not listed
    std::vector<TextureVar> mxao { { 1, "MXAODEPTH" }, { 2, "Other" } };
    b.NoDepthCandidate(kRt2, mxao);
    CHECK(LastLine("no known depth input").find("MXAODEPTH") != std::string::npos);
    // a limited number of lines per process
    for (int i = 0; i < 10; ++i)
        b.NoDepthCandidate(kRt + i, mxao);
    CHECK(LogLines("no known depth input") == before + 3);
}

// ---- the first bind --------------------------------------------------------------------------------------------------------------
void FirstBindReadback()
{
    // five candidate variables that do NOT read the DEPTH semantic (decoys: some other binding), and the real reader
    FakeRaw raw;
    for (uint64_t h = 0x502; h <= 0x506; ++h)
        raw.textures.push_back({ h, "DepthBufferTex", "", 0xA7, 0xA8 });
    LiveDepthBackend b(raw, Env());
    uint64_t s = 0, g = 0;
    const size_t before = LogLines("first bind");
    // reads before any bind are not logged
    CHECK(b.TextureBinding(kRt, kDepthVar, s, g));
    CHECK(LogLines("first bind") == before);
    // a bind of the copy arms it: the next readbacks of that runtime are logged
    CHECK(b.UpdateTextureBindings(kRt, "DEPTH", kCopy, kCopy));
    // the decoys come first (the core reads every candidate until one shows the copy): they must not use up the lines of the reader
    for (uint64_t v = 0x502; v <= 0x506; ++v)
        CHECK(b.TextureBinding(kRt, v, s, g) && s == 0xA7 && g == 0xA8);
    CHECK(LogLines("first bind") == before + 3); // at most three decoys
    CHECK(b.TextureBinding(kRt, kDepthVar, s, g) && s == kCopy && g == kCopy);
    CHECK(LogLines("first bind") == before + 4); // ... and the reader is always there
    bool reader = false;
    for (const std::string& l : depthtest_log::Lines())
        reader = reader || (l.find("first bind") != std::string::npos && l.find("0xc0 / srgb 0xc0") != std::string::npos);
    CHECK(reader);
    CHECK(b.TextureBinding(kRt, kDepthVar, s, g)); // the reader is said once
    CHECK(LogLines("first bind") == before + 4);
    { // a decoy that shows the bound srv but another srgb is no reader
        FakeRaw half;
        half.textures.push_back({ 0x508, "DepthBufferTex", "", kCopy, 0xA8 });
        LiveDepthBackend hb(half, Env());
        CHECK(hb.UpdateTextureBindings(kRt, "DEPTH", kCopy, kCopy));
        CHECK(hb.TextureBinding(kRt, 0x508, s, g));
        CHECK(LastLine("first bind").find("a decoy") != std::string::npos);
    }
    // the line says what ReShade answered, and what was bound
    bool both = false;
    for (const std::string& l : depthtest_log::Lines())
        both = both || (l.find("first bind") != std::string::npos && l.find("0xa7") != std::string::npos && l.find("0xa8") != std::string::npos &&
                        l.find("bound 0xc0") != std::string::npos);
    CHECK(both);
    { // a read of another runtime during the observation is not logged
        FakeRaw other;
        other.textures.push_back({ 0x507, "NoSemantic", "", 0xA7, 0xA8 });
        LiveDepthBackend ob(other, Env());
        CHECK(ob.UpdateTextureBindings(kRt, "DEPTH", kCopy, kCopy));
        const size_t quiet = LogLines("first bind");
        CHECK(ob.TextureBinding(kRt2, 0x507, s, g));
        CHECK(LogLines("first bind") == quiet);
        CHECK(ob.TextureBinding(kRt, 0x507, s, g));
        CHECK(LogLines("first bind") == quiet + 1);
    }
    const size_t afterBlock = LogLines("first bind"); // (that backend's line is in the shared log)
    // only the first bind of the process: a later one logs nothing
    CHECK(b.UpdateTextureBindings(kRt, "DEPTH", kOrig, kOrigSrgb)); // the restore
    CHECK(b.UpdateTextureBindings(kRt, "DEPTH", kCopy, kCopy));
    CHECK(b.TextureBinding(kRt, kDepthVar, s, g));
    CHECK(b.TextureBinding(kRt, 0x502, s, g));
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

    // a bind that is restored with the original views before anything was read (nothing logged) does not make the restore the next
    // observation: the restore binds views too, and what is read after it is not the first bind
    FakeRaw r4;
    LiveDepthBackend b4(r4, Env());
    const size_t l4 = LogLines("first bind");
    CHECK(b4.UpdateTextureBindings(kRt, "DEPTH", kCopy, kCopy));
    CHECK(b4.UpdateTextureBindings(kRt, "DEPTH", kOrig, kOrigSrgb)); // the restore, nothing read in between
    CHECK(b4.TextureBinding(kRt, kDepthVar, s, g));
    CHECK(LogLines("first bind") == l4);
    CHECK(b4.UpdateTextureBindings(kRt, "DEPTH", kCopy, kCopy)); // the next real bind is still the first one
    CHECK(b4.TextureBinding(kRt, kDepthVar, s, g));
    CHECK(LogLines("first bind") == l4 + 1);

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
    { // a real runtime for another device is not a consumer (with a second runtime around: a lone runtime is the game's, see SameDevice)
        Rig r;
        r.raw.R(kRt).native = kDev2;
        r.backend.NoteRuntime(kRt);
        r.backend.NoteRuntime(kRt2);
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
    for (const bool header : { false, true })
    {
        g_headerWording = header; // the observed behaviour first, then the vendored header's wording
        g_now = 5000; // (the tests leave these as they like)
        g_frame = 77;
        g_primary = kDev;
        std::printf("reshade_depth_backend: string wording %s\n", header ? "header" : "observed");
        StringContract();
        TextureNames();
        BadStrings();
        Definitions();
        UniformsAndTechniques();
        Views();
        RuntimeQueries();
        SameDeviceSemantics();
        SameDeviceByResources();
        Registry();
        StringContractLog();
        SameDeviceRefusalLog();
        NoCandidateLog();
        FirstBindReadback();
        BinderOverTheBackend();
        FaultAtEveryRawCall();
    }
    if (g_fail != 0)
    {
        std::printf("reshade_depth_backend: %d check(s) failed\n", g_fail);
        return 1;
    }
    std::puts("reshade_depth_backend: all checks passed");
    return 0;
}
