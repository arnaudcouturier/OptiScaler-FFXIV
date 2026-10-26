#pragma once
// Shared fixtures of the gate tests (gate_rules, startup_flow): the product registry, a synthetic ResolvedBuild with every seam Found that a test
// breaks one seam at a time, and small helpers around Decide(). Header-only, one translation unit per test.
#include "resolve_test_util.h"

#include "ext/ffxiv/ActiveBuild.h"
#include "ext/ffxiv/resolve/Gate.h"
#include "ext/ffxiv/resolve/Registry.h"
#include "ext/ffxiv/resolve/Resolver.h"

#include <cstdio>
#include <string>

namespace gf
{
using namespace rt;
using namespace ext::ffxiv;

inline const Registry& Reg()
{
    static const Registry r = BuildRegistry();
    return r;
}

// Every seam of the registry Found with every anchor Unique, the build = the table (so every Rva / Guard / Offset field is the known value), the
// cross-group pairs agreeing. `known` sets rb.knownBuild (the resolver's verdict).
inline ResolvedBuild AllFound(bool known)
{
    ResolvedBuild rb;
    rb.build = kCurrentBuild;
    rb.timeDateStamp = known ? kCurrentBuild.timeDateStamp : 0x6bbbbbbbu;
    rb.sizeOfImage = kCurrentBuild.sizeOfImage;
    rb.knownBuild = known;
    std::uint32_t n = 0;
    for (const SeamSpec& s : Reg().seams)
    {
        SeamResult r;
        r.id = s.id;
        r.status = SeamStatus::Found;
        r.hasValue = true;
        r.rva = 0x1000 + 0x40 * (++n);
        for (const Binding& b : Reg().bindings)
        {
            const FieldRow* row = FindField(b.key);
            if (b.seam == s.id && row != nullptr && b.valueOffset == 0 && (row->kind == FieldKind::Rva || row->kind == FieldKind::Offset))
            {
                r.rva = *FieldU32(rb.build, *row);
                break;
            }
        }
        for (const AnchorSpec& a : s.anchors)
        {
            AnchorResult ar;
            ar.name = a.name;
            ar.outcome = AnchorOutcome::Unique;
            ar.candidates = 1;
            ar.hasValue = true;
            r.anchors.push_back(ar);
        }
        r.hits = static_cast<std::uint32_t>(s.anchors.size());
        rb.seams.push_back(std::move(r));
    }
    // jitterSource.cameraGlobal binds no field: it is the same global as camera.global (the cross-group pair)
    for (SeamResult& s : rb.seams)
    {
        if (s.id == "jitterSource.cameraGlobal")
            s.rva = kCurrentBuild.jitter.cameraGlobalRva;
    }
    return rb;
}

inline SeamResult& Mut(ResolvedBuild& rb, const std::string& id)
{
    for (SeamResult& s : rb.seams)
    {
        if (s.id == id)
            return s;
    }
    std::printf("FAIL: no seam %s in the registry\n", id.c_str());
    ++g_failures;
    return rb.seams.front();
}

inline void SetStatus(ResolvedBuild& rb, const std::string& id, SeamStatus st, bool fellBack = false)
{
    SeamResult& s = Mut(rb, id);
    s.status = st;
    s.hasValue = HasValue(st);
    s.fellBackToTable = fellBack;
    s.reason = st == SeamStatus::NotFound ? "no anchor matched" : "";
}

inline GateSettings Settings(bool legacy = false, bool unknownHooks = false, bool force = false)
{
    GateSettings s;
    s.legacyGameBuild = legacy;
    s.unknownBuildHooks = unknownHooks;
    s.forceUnknownBuild = force;
    return s;
}

inline GateReport Run(const ResolvedBuild* rb, const GateSettings& s, bool exe = true, bool exact = true)
{
    GateInput in;
    in.exeIsFfxiv = exe;
    in.exactKnownBuild = exact;
    in.rb = rb;
    in.registry = &Reg();
    return Decide(in, s);
}

inline constexpr Feature kGroup2[] = { Feature::QualityRead,  Feature::QualityHooks,    Feature::JitterProbe,
                                Feature::JitterSource, Feature::CompanionNative, Feature::CharaViewAccessor };

inline bool AllGroup1(const GateReport& g, bool on)
{
    for (std::size_t i = 0; i < static_cast<std::size_t>(kFirstGroup2Feature); ++i)
    {
        if (g.features[i].enabled != on || g.features[i].reason.empty())
            return false;
    }
    return true;
}
inline bool AllGroup2(const GateReport& g, bool on)
{
    for (const Feature f : kGroup2)
    {
        if (g.Enabled(f) != on || g.Reason(f).empty())
            return false;
    }
    return true;
}
inline bool Contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

} // namespace gf
