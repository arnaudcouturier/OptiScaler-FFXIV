#pragma once
// The gate: a pure function from "what the resolver found" plus three persistent keys to a decision per feature (on/off and why).
//
//   input   a ResolvedBuild (or none), whether the exe is ffxiv_dx11.exe, whether its stamp and size are the known build's, GateSettings
//   output  GateReport: one Decision {enabled, reason} per Feature, the four cross-group consistency checks, the known/unknown verdict
//
// Header-only, no <windows.h>, no logging macros, no namespace-scope object with a constructor: the rules are unit-tested on synthetic
// ResolvedBuilds and on real ResolveImage results over dense images (tests/ext/ffxiv/resolve/gate_*.cpp). Nothing here reads the game.
//
// THE DESIGN:
//   Group 1 = features that identify by exe name or shader CRC and read no address of the executable (scene HDR, lighting, UI / HUD-less
//     boundary, geometry, markers, MipBind, the FFXIV quirks, the catalog health). They run on ANY ffxiv_dx11.exe.
//   Group 2 = features that read or hook addresses of the executable (native quality, jitter probe and source, the
//     CharaView accessor, the simulation-tick lease). KNOWN build: they run when the seams say so. UNKNOWN build: only with FfxivUnknownBuildHooks AND usable seams.
//   FfxivLegacyGameBuild (rollback): EXACTLY the legacy rules. Exact stamp and size, the compile-time table and its old byte guards, no resolve,
//     Group 2 only on the exact known build, Group 1 only on the exact known build too, nothing from the other two keys. The simulation-tick lease
//     is OFF under that key even on the exact known build: it is not part of the behaviour the key restores.
//
// THE ONE RULE for every hook site of all three groups (the group headers phrase it three ways):
//   usable  iff  rb.Usable(location) && rb.Usable(shape)   on EVERY build, never UsableOrTable for a shape.
//   A shape is failClosed: a table-filled location never makes its shape usable. EntryPatched is never usable.
// Every OTHER seam a feature reads (offsets, helper-reached seams, tables) is Usable on an UNKNOWN build; on a KNOWN build it is also accepted when
// its field was filled from the table (rb.UsableOrTable): the table is verified offline against that exact executable, and a third party that hooks
// a helper function the fork never touches must not turn a feature off while the verified value is at hand. The seams a contract
// names stay strict on every build (kStrictOnKnownBuild below): the hook sites, the jitter globals and constants, the quality write path.
// On the unpatched known build every seam is plain Found, so nothing changes there; the difference is a foreign patch inside a long strict shape,
// which the old short guards missed (a deliberate tightening, reported). The table still fills the build the consumers read (ActiveBuild) and the
// resolver's cross-check still makes the table win on a disagreement.
//
// Failure of the resolver ITSELF: on the exact known build an infrastructure failure (the image could not be opened, a cached exception,
// a scan that hit an unreadable window) falls back to the legacy decision, the same Group 2 decision as FfxivLegacyGameBuild (the compile-time table
// and the old byte guards decide). The gate block of the log carries one WARN for it; after a cached exception the not-self-healing line still prints, worded for the known build the gate runs on (the failed result itself is not marked as one); what is printed BEFORE that depends on the failure: nothing
// for an image that could not be opened, a WARN line per unresolved seam after a scan abort, an Error line from the resolver after a cached exception,
// an Error line from the startup after a startup exception. An unknown build stays off, and so does FfxivForceUnknownBuild (it asked for the resolver's
// values).
//
// The exact build is identified by its PE stamp and size, whatever the file is called (the legacy selector matched stamp and size only, and so does the
// rollback key); the name ffxiv_dx11.exe only decides on a build the table does not know (MakeGateInputT, StartupDecision.h).
// The frame pacing trace and the automatic ReShade depth still check the name, as before. The consumers still need
// ActiveStarted() (the first device hook) before any hook, DLAA read or CharaView works.

#include "AnchorsJitter.h"
#include "../BuildBounds.h"
#include "GateTypes.h"
#include "Registry.h"
#include "ResolveTypes.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace ext::ffxiv::resolve
{
// ---- the exe name ---------------------------------------------------------------------------------------------------------------------

// "ffxiv_dx11.exe", case-insensitive, on the last path component (a full path from GetModuleFileNameW is fine). `Char` is char or wchar_t.
template <class Char> bool IsFfxivExeNameT(std::basic_string_view<Char> name)
{
    for (std::size_t i = name.size(); i > 0; --i)
    {
        if (name[i - 1] == static_cast<Char>('\\') || name[i - 1] == static_cast<Char>('/'))
        {
            name = name.substr(i);
            break;
        }
    }
    constexpr std::string_view want = "ffxiv_dx11.exe";
    if (name.size() != want.size())
        return false;
    for (std::size_t i = 0; i < want.size(); ++i)
    {
        std::uint32_t c = static_cast<std::uint32_t>(name[i]);
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
        if (c != static_cast<std::uint32_t>(static_cast<unsigned char>(want[i])))
            return false;
    }
    return true;
}
inline bool IsFfxivExeName(std::string_view name) { return IsFfxivExeNameT<char>(name); }
inline bool IsFfxivExeName(std::wstring_view name) { return IsFfxivExeNameT<wchar_t>(name); }

// ---- inputs ---------------------------------------------------------------------------------------------------------------------------

struct GateSettings
{
    bool legacyGameBuild = false;   // [FFXIV] FfxivLegacyGameBuild   (rollback: exactly the legacy behaviour)
    bool unknownBuildHooks = false; // [FFXIV] FfxivUnknownBuildHooks (Group 2 on an unknown build)
    bool forceUnknownBuild = false; // [FFXIV] FfxivForceUnknownBuild (debug: the resolver ran WITHOUT the table; the build counts as unknown)
};

struct GateInput
{
    bool exeIsFfxiv = false;
    bool exactKnownBuild = false;   // the PE stamp and size equal the table's (MatchBuild != nullptr); the legacy path's identity
    const ResolvedBuild* rb = nullptr; // nullptr: no resolve ran (legacy, or the image could not be opened)
    const Registry* registry = nullptr; // nullptr: BuildRegistry()
    std::string resolverFailure {};     // non-empty: the startup itself failed (an exception outside the resolver cache); treated as an infrastructure failure
};

// ---- seam lists (data: the tests enumerate them) --------------------------------------------------------------------------------------

struct SitePair
{
    const char* location;
    const char* shape;
};

inline constexpr SitePair kQualitySites[] = {
    { "quality.querySize", "quality.querySizeShape" },
    { "quality.rendererUpdate", "quality.rendererUpdateShape" },
    { "quality.resizeCallbacks", "quality.resizeCallbacksShape" },
};
// Seams the DLAA gate (QualityRead) reads: the settings read path (its own strict block), the settings global, the four settings offsets.
inline constexpr const char* kQualityReadSeams[] = {
    "quality.settingsInstructions", "quality.settingsPointer",     "quality.settingsModeOffset",
    "quality.settingsQualityOffset", "quality.settingsDlaaOffset", "quality.settingsSelectorOffset",
};
// Seams the renderer-update request path reads on top of that (the chain manager -> context, the device callbacks, the display-size cache).
inline constexpr const char* kQualityHookSeams[] = {
    "quality.managerPointer",       "quality.devicePointer",        "quality.managerContextOffset",  "quality.deviceCallbacksOffset",
    "quality.rendererSceneSizeOffset", "quality.contextFlagsOffset", "quality.contextCacheOffset",
};
inline constexpr const char* kJitterProbeSeams[] = { "camera.global", "camera.cameraOffset", "camera.jitterPointerOffset" };
inline constexpr const char* kCharaSeams[] = { "chara.accessor", "chara.managerPointer", "chara.targetsOffset" };
// Seams the simulation-tick lease needs: the function (found from its interior, so a foreign inline hook over the entry does not hide it) and the
// global holding the Framework instance whose vtable slot is leased. The three counters the tick updates are NOT listed: they are telemetry only,
// the install's own layout check covers a pair of them, and a build that moved them must not turn the boundary off.
inline constexpr const char* kLatencySeams[] = { "latency.tick", "latency.frameworkInstance" };

struct CrossPair
{
    const char* left;
    const char* right;
    Feature affects[3]; // the features that read either side (Count = unused slot)
};
inline constexpr CrossPair kCrossPairs[] = {
    { "chara.managerPointer", "jitterSource.rendererPointer", { Feature::CharaViewAccessor, Feature::JitterSource, Feature::Count } },
    { "camera.global", "jitterSource.cameraGlobal", { Feature::JitterProbe, Feature::JitterSource, Feature::Count } },
    { "quality.settingsPointer", "jitterSource.settingsPointer", { Feature::QualityRead, Feature::QualityHooks, Feature::JitterSource } },
    { "quality.devicePointer", "jitterSource.devicePointer", { Feature::QualityHooks, Feature::JitterSource, Feature::Count } },
};

// Seams that stay STRICT (plain Usable, never filled from the table) on a KNOWN build too, besides every hook site (location AND shape) of the three
// groups. Taken from the hook-site rules of the Anchors headers: the quality display-size cache write (AnchorsQuality.h hook-site rule, managerPointer, managerContextOffset,
// contextFlagsOffset, settingsPointer, the settings mode offset, contextCacheOffset) and the settings read path's own shape, the three
// jitter globals, strengthFloor, ndcSignMask and the camera global of the world tie (AnchorsJitter.h), and the tick and the instance global of the
// simulation-tick lease (AnchorsLatency.h): an anchor that does not resolve on the known build means the build is not behaving as the table says, and
// the lease must not be installed from a table the executable just contradicted. Every other seam a feature reads is accepted on a
// known build when its field was filled from the verified table. On an UNKNOWN build everything is strict.
inline constexpr const char* kStrictOnKnownBuild[] = {
    "quality.settingsInstructions", "quality.settingsPointer",     "quality.settingsModeOffset",    "quality.managerPointer",
    "quality.managerContextOffset", "quality.contextFlagsOffset",  "quality.contextCacheOffset",    "jitterSource.settingsPointer",
    "jitterSource.rendererPointer", "jitterSource.devicePointer",  "jitterSource.strengthFloor",    "jitterSource.ndcSignMask",
    "jitterSource.cameraGlobal",    "latency.tick",                "latency.frameworkInstance",
};

namespace gate_detail
{
inline bool IsSiteSeam(std::string_view id)
{
    for (const SitePair& s : kQualitySites)
        if (id == s.location || id == s.shape)
            return true;
    for (const JitterHookSite& s : kJitterHookSites)
        if (id == s.location || id == s.shape)
            return true;
    return false;
}

inline bool StrictOnKnownBuild(std::string_view id)
{
    if (IsSiteSeam(id))
        return true;
    for (const char* s : kStrictOnKnownBuild)
        if (id == s)
            return true;
    return false;
}

inline void AddUnique(std::vector<std::string>& v, const std::string& id)
{
    if (std::find(v.begin(), v.end(), id) == v.end())
        v.push_back(id);
}

// Every seam a binding of `group` (a FieldTable group: "nativeQuality", "charaView", "jitter", "jitterSource") fills a field from:
// the Rva, the Guard and the Offset fields alike. Deduplicated, registry order. This is how "every bound seam of the group must be usable"
// follows the registry instead of a hand-copied list: a resolver change that binds one more field shows up here (and in a test that pins the count).
inline std::vector<std::string> BoundSeams(const Registry& reg, std::string_view group)
{
    std::vector<std::string> out;
    for (const Binding& b : reg.bindings)
    {
        const FieldRow* row = FindField(b.key);
        if (row != nullptr && group == row->group)
            AddUnique(out, b.seam);
    }
    return out;
}

// The seam ids of a SitePair list, both members.
template <std::size_t N> void AddSites(std::vector<std::string>& out, const SitePair (&sites)[N])
{
    for (const SitePair& s : sites)
    {
        AddUnique(out, s.location);
        AddUnique(out, s.shape);
    }
}
template <std::size_t N> void AddSeams(std::vector<std::string>& out, const char* const (&ids)[N])
{
    for (const char* id : ids)
        AddUnique(out, id);
}

inline std::string SeamFault(const ResolvedBuild& rb, const std::string& id)
{
    const SeamResult* s = rb.Find(id);
    if (s == nullptr)
        return id + " is not in the registry";
    if (s->status == SeamStatus::EntryPatched)
        return id + " EntryPatched (a foreign hook sits over the entry: never hooked)";
    std::string t = id + " " + StatusName(s->status);
    if (!s->reason.empty())
        t += " (" + s->reason + ")";
    return t;
}

// Collects why a feature is refused. The reason keeps the first three faults and a count.
class Checks
{
  public:
    explicit Checks(const ResolvedBuild& rb) : _rb(rb) {}

    void Usable(const std::string& id)
    {
        ++_checked;
        if (!_rb.Usable(id))
            _faults.push_back(SeamFault(_rb, id));
    }
    // A seam a feature reads that is not a hook site: usable; on a KNOWN build also when its field was filled from the verified table (never for
    // EntryPatched, never for a seam a hook-site rule names: kStrictOnKnownBuild). On an unknown build the table-fill flag is ignored.
    void Seam(const std::string& id)
    {
        ++_checked;
        const SeamResult* s = _rb.Find(id);
        const bool tableOk = _rb.knownBuild && !StrictOnKnownBuild(id) && s != nullptr && s->fellBackToTable && s->status != SeamStatus::EntryPatched;
        if (s == nullptr || !(IsUsable(s->status) || tableOk))
            _faults.push_back(SeamFault(_rb, id));
    }
    // quality.contextCacheOffset on a KNOWN build: plain Found, or a single anchor that only rests on a weak dependency (the +0x150/+0x154 pair is
    // verified offline for that exact exe). Never a table-filled value.
    void FoundOrWeakSingle(const std::string& id)
    {
        ++_checked;
        const SeamResult* s = _rb.Find(id);
        const bool ok = s != nullptr && (s->status == SeamStatus::Found || (s->status == SeamStatus::FoundSingleAnchor && s->viaWeakDependency));
        if (!ok)
            _faults.push_back(s == nullptr ? id + " is not in the registry"
                                           : id + " is " + StatusName(s->status) + ", plain Found (or, on the known build, a single anchor through a weak dependency) is required");
    }
    // a hook site: location AND shape, on every build
    void Site(const std::string& location, const std::string& shape)
    {
        Usable(location);
        if (shape != location)
            Usable(shape);
    }
    void PlainFound(const std::string& id)
    {
        ++_checked;
        const SeamResult* s = _rb.Find(id);
        if (s == nullptr || s->status != SeamStatus::Found)
            _faults.push_back(s == nullptr ? id + " is not in the registry"
                                           : id + " is " + StatusName(s->status) + ", plain Found is required (one anchor does not pin it)");
    }
    void AnchorUnique(const std::string& seam, const std::string& anchor)
    {
        ++_checked;
        const SeamResult* s = _rb.Find(seam);
        bool ok = false;
        if (s != nullptr)
        {
            for (const AnchorResult& a : s->anchors)
                ok = ok || (a.name == anchor && a.outcome == AnchorOutcome::Unique);
        }
        if (!ok)
            _faults.push_back(seam + " anchor '" + anchor + "' is not Unique");
    }
    void Require(bool ok, const std::string& why)
    {
        ++_checked;
        if (!ok)
            _faults.push_back(why);
    }
    void Merge(const Checks& other)
    {
        _checked += other._checked;
        _faults.insert(_faults.end(), other._faults.begin(), other._faults.end());
    }
    bool Ok() const { return _faults.empty(); }
    std::size_t Checked() const { return _checked; }
    std::string Reason() const
    {
        if (_faults.empty())
            return "all " + std::to_string(_checked) + " required checks pass";
        std::string r;
        for (std::size_t i = 0; i < _faults.size() && i < 3; ++i)
            r += (i ? "; " : "") + _faults[i];
        if (_faults.size() > 3)
            r += "; +" + std::to_string(_faults.size() - 3) + " more";
        return r;
    }

  private:
    const ResolvedBuild& _rb;
    std::size_t _checked = 0;
    std::vector<std::string> _faults;
};

inline void SetOn(Decision& d, std::string why)
{
    d.enabled = true;
    d.reason = std::move(why);
}
inline void SetOff(Decision& d, std::string why)
{
    d.enabled = false;
    d.reason = std::move(why);
}

inline Checks QualityReadChecks(const ResolvedBuild& rb)
{
    Checks c(rb);
    for (const char* id : kQualityReadSeams)
        c.Seam(id);
    c.Require(SettingsOffsetsInBounds(rb.build.nativeQuality), "a settings offset is outside the snapshot buffer");
    return c;
}

inline Checks QualityHookChecks(const ResolvedBuild& rb, const Registry& reg)
{
    Checks c = QualityReadChecks(rb);
    for (const SitePair& s : kQualitySites)
        c.Site(s.location, s.shape);
    // detour only when the anchor named `entry` is Unique (a prologue-only change leaves body / caller as the usable but weaker evidence)
    c.AnchorUnique("quality.querySize", "entry");
    c.AnchorUnique("quality.rendererUpdate", "entry");
    for (const char* id : kQualityHookSeams)
        c.Seam(id);
    // the 8-byte write at [[manager + managerContextOffset] + contextCacheOffset]: the seam itself plain Found (one anchor pins BOTH members of
    // the +0x150/+0x154 pair, verified and never healed). On the KNOWN build a single anchor through a weak dependency is accepted as well (a foreign
    // hook over the helper quality.cacheCompare; the pair is verified offline for that exact exe). Never a table-filled value.
    if (rb.knownBuild)
        c.FoundOrWeakSingle("quality.contextCacheOffset");
    else
        c.PlainFound("quality.contextCacheOffset");
    // every other bound seam of the group (a function seam can resolve while an offset it feeds is Conflict / NotFound)
    for (const std::string& id : BoundSeams(reg, "nativeQuality"))
        c.Seam(id);
    return c;
}

inline Checks JitterProbeChecks(const ResolvedBuild& rb, const Registry& reg)
{
    Checks c(rb);
    for (const char* id : kJitterProbeSeams)
        c.Seam(id);
    for (const std::string& id : BoundSeams(reg, "jitter"))
        c.Seam(id);
    return c;
}

inline Checks JitterSourceChecks(const ResolvedBuild& rb, const Registry& reg)
{
    Checks c(rb);
    // every hook site: location AND shape (data table kJitterHookSites, shared with AnchorsJitter.h)
    for (const JitterHookSite& s : kJitterHookSites)
        c.Site(s.location, s.shape);
    // the three globals the hooks read memory through: Usable on EVERY build (a Conflict global is table-filled on a known build)
    for (const JitterGlobal& g : kJitterGlobals)
        c.Usable(g.seam);
    // no field of their own: the live constants the core assumes (strengthFloorBits / ndcSignMaskBits, exactly 0.001f and 0x80000000) and the
    // camera global of the producer's world tie
    for (const char* id : kJitterGateSeams)
        c.Usable(id);
    // the minimum, kept as defence in depth: a zero global would make ReadInputs read the PE header as a pointer
    const JitterSourceBuild& b = rb.build.jitterSource;
    c.Require(b.settingsPointerRva != 0 && b.rendererPointerRva != 0 && b.devicePointerRva != 0, "a jitterSource global field is zero");
    for (const std::string& id : BoundSeams(reg, "jitterSource"))
        c.Seam(id);
    return c;
}

inline Checks CharaChecks(const ResolvedBuild& rb, const Registry& reg)
{
    Checks c(rb);
    for (const char* id : kCharaSeams)
        c.Seam(id);
    for (const std::string& id : BoundSeams(reg, "charaView"))
        c.Seam(id);
    return c;
}

inline Checks LatencyChecks(const ResolvedBuild& rb)
{
    Checks c(rb);
    // The two required seams. Deliberately not BoundSeams(reg, "latency"): the three counter offsets stay optional, and on a known build a table-filled
    // value is accepted here like for any other seam that is not a hook site - the live slot identity check at install is what protects the write.
    for (const char* id : kLatencySeams)
        c.Seam(id);
    return c;
}
} // namespace gate_detail

// Every seam id that a feature's gate requires to be usable (explicit lists plus the registry-derived bound seams): what the tests iterate to
// prove that making any ONE of them unusable turns the feature off. Group 1 features and QualityRead use their own short lists.
inline std::vector<std::string> RequiredSeams(Feature f, const Registry& reg)
{
    using namespace gate_detail;
    std::vector<std::string> out;
    switch (f)
    {
    case Feature::QualityRead:
        AddSeams(out, kQualityReadSeams);
        break;
    case Feature::QualityHooks:
        AddSeams(out, kQualityReadSeams);
        AddSites(out, kQualitySites);
        AddSeams(out, kQualityHookSeams);
        for (const std::string& id : BoundSeams(reg, "nativeQuality"))
            AddUnique(out, id);
        break;
    case Feature::JitterProbe:
        AddSeams(out, kJitterProbeSeams);
        for (const std::string& id : BoundSeams(reg, "jitter"))
            AddUnique(out, id);
        break;
    case Feature::JitterSource:
        for (const JitterHookSite& s : kJitterHookSites)
        {
            AddUnique(out, s.location);
            AddUnique(out, s.shape);
        }
        for (const JitterGlobal& g : kJitterGlobals)
            AddUnique(out, g.seam);
        AddSeams(out, kJitterGateSeams);
        for (const std::string& id : BoundSeams(reg, "jitterSource"))
            AddUnique(out, id);
        break;
    case Feature::CharaViewAccessor:
        AddSeams(out, kCharaSeams);
        for (const std::string& id : BoundSeams(reg, "charaView"))
            AddUnique(out, id);
        break;
    case Feature::LatencyTick:
        AddSeams(out, kLatencySeams);
        break;
    default:
        break;
    }
    return out;
}

// ---- the cross-group consistency checks -----------------------------------------------------------------------------------------------

inline std::vector<CrossCheckResult> CrossCheckGroups(const ResolvedBuild& rb)
{
    std::vector<CrossCheckResult> out;
    for (const CrossPair& p : kCrossPairs)
    {
        CrossCheckResult r;
        r.name = std::string(p.left) + " == " + p.right;
        const SeamResult* a = rb.Find(p.left);
        const SeamResult* b = rb.Find(p.right);
        if (a != nullptr && b != nullptr && IsUsable(a->status) && IsUsable(b->status))
        {
            r.compared = true;
            r.left = a->rva;
            r.right = b->rva;
            r.agree = a->rva == b->rva;
        }
        out.push_back(std::move(r));
    }
    return out;
}

// ---- the decision ---------------------------------------------------------------------------------------------------------------------

// Why a resolve result cannot be read as a verdict about the executable: "" when it can. INFRASTRUCTURE, not evidence: no result at all (the image
// could not be opened), a cached exception or a registry error, no seams, a scan that hit an unreadable window. A resolve that ran and found nothing
// (every seam NotFound) is a verdict and not a failure.
inline std::string ResolverInfrastructureFailure(const ResolvedBuild* rb)
{
    if (rb == nullptr)
        return "the main module image could not be read, no resolve result";
    if (!rb->registryErrors.empty())
        return rb->registryErrors.front();
    if (rb->seams.empty())
        return "no seams";
    if (rb->scanFailed)
        return "the image could not be scanned: " + rb->scanError;
    return std::string();
}

inline GateReport Decide(const GateInput& in, const GateSettings& s)
{
    using namespace gate_detail;
    GateReport r;
    r.started = true;
    r.exeIsFfxiv = in.exeIsFfxiv;
    r.legacy = s.legacyGameBuild;
    r.forcedUnknown = s.forceUnknownBuild && !s.legacyGameBuild;
    r.unknownBuildHooks = s.unknownBuildHooks;

    // ---- not the game: nothing at all
    if (!in.exeIsFfxiv)
    {
        for (Decision& d : r.features)
            SetOff(d, "the executable is not ffxiv_dx11.exe");
        return r;
    }

    // ---- the rollback key: exactly the legacy rules
    if (s.legacyGameBuild)
    {
        r.known = in.exactKnownBuild;
        for (std::size_t i = 0; i < kFeatureCount; ++i)
        {
            Decision& d = r.features[i];
            const Feature f = static_cast<Feature>(i);
            // The simulation-tick lease is not part of the behaviour this key restores: off, on the exact known build too.
            if (f == Feature::LatencyTick)
            {
                SetOff(d, "FfxivLegacyGameBuild: the simulation-tick lease stays off, it is not part of the behaviour this key restores");
                continue;
            }
            if (in.exactKnownBuild)
                SetOn(d, IsGroup2(f) ? "FfxivLegacyGameBuild: the exact known build; the old byte guards and the compile-time table decide"
                                     : "FfxivLegacyGameBuild: the exact known build (as before)");
            else
                SetOff(d, "FfxivLegacyGameBuild: this build is not the exact known one (stamp and size), nothing runs (as before)");
        }
        return r;
    }

    // ---- Group 1: the exe identity is the whole condition (the stamp and size on the known build, the name otherwise)
    for (std::size_t i = 0; i < static_cast<std::size_t>(kFirstGroup2Feature); ++i)
        SetOn(r.features[i], in.exactKnownBuild ? "exact known build (PE stamp and size): identified without the exe name, no executable address is read" : "ffxiv_dx11.exe: identified by name and shader CRC, no executable address is read");

    // ---- Group 2
    const ResolvedBuild* rb = in.rb;
    auto allGroup2 = [&](const std::string& why)
    {
        for (std::size_t i = static_cast<std::size_t>(kFirstGroup2Feature); i < kFeatureCount; ++i)
            SetOff(r.features[i], why);
    };
    const std::string failure = !in.resolverFailure.empty() ? in.resolverFailure : ResolverInfrastructureFailure(rb);
    if (!failure.empty())
    {
        if (in.exactKnownBuild && !s.forceUnknownBuild)
        {
            // The resolver itself failed on the exact known build. That says nothing about the executable, so the legacy decision runs for Group 2
            // (the compile-time table and the old byte guards decide, as under FfxivLegacyGameBuild). The forced-unknown debug key asked for the
            // resolver's own values and stays off; an unknown build stays off.
            r.known = true;
            r.legacyFallback = true;
            r.fallbackWhy = failure;
            for (std::size_t i = static_cast<std::size_t>(kFirstGroup2Feature); i < kFeatureCount; ++i)
                SetOn(r.features[i], "resolver infrastructure failure (" + failure +
                                         "): falling back to the legacy decision, the exact known build runs on the compile-time table and the old byte guards");
            return r;
        }
        r.known = rb != nullptr && rb->knownBuild;
        if (rb == nullptr && in.resolverFailure.empty())
            allGroup2("the resolver did not run (no image, or startup was not reached): fail open");
        else
            allGroup2("the resolver failed (" + failure + "): fail open");
        return r;
    }
    r.known = rb->knownBuild;
    if (s.forceUnknownBuild && rb->knownBuild)
    {
        allGroup2("FfxivForceUnknownBuild is on but the resolver treated the build as known: refused");
        return r;
    }

    static const Registry defaultRegistry = BuildRegistry();
    const Registry& reg = in.registry != nullptr ? *in.registry : defaultRegistry;

    const Checks quality = QualityReadChecks(*rb);
    const Checks qualityHooks = QualityHookChecks(*rb, reg);
    const Checks probe = JitterProbeChecks(*rb, reg);
    const Checks source = JitterSourceChecks(*rb, reg);
    const Checks chara = CharaChecks(*rb, reg);
    const Checks latency = LatencyChecks(*rb);
    auto put = [&](Feature f, const Checks& c) { c.Ok() ? SetOn(r.At(f), c.Reason()) : SetOff(r.At(f), c.Reason()); };
    put(Feature::QualityRead, quality);
    put(Feature::QualityHooks, qualityHooks);
    put(Feature::JitterProbe, probe);
    put(Feature::JitterSource, source);
    put(Feature::CharaViewAccessor, chara);
    put(Feature::LatencyTick, latency);
    // The lease's telemetry offsets: each bit says its own seam is usable (or table-filled on the known build), so the install may read it. A clear
    // bit is a value that describes no known layout: skipped, never read, and never a reason to refuse the lease (they are not required seams).
    r.latencyOffsetsResolved = 0;
    if (rb->UsableOrTable("latency.frameCounterOffset"))
        r.latencyOffsetsResolved |= kLatencyFrameCounterResolved;
    if (rb->UsableOrTable("latency.perfCounterValueOffset"))
        r.latencyOffsetsResolved |= kLatencyPerfValueResolved;
    if (rb->UsableOrTable("latency.perfCounterFreqOffset"))
        r.latencyOffsetsResolved |= kLatencyPerfFreqResolved;

    // ---- the cross-group consistency checks: a mismatch turns off the features of both sides
    //   KNOWN build: a mismatch of two usable sides (a side that is not usable is not compared, as before: the table is verified offline).
    //   UNKNOWN build: the pair must be compared AND agree. A side that is Conflict / NotFound leaves the other side's global with no
    //   second, independent derivation, and on an unknown build the two derivations are the only cross-confirmation these globals have.
    r.crossChecks = CrossCheckGroups(*rb);
    for (std::size_t i = 0; i < r.crossChecks.size(); ++i)
    {
        const CrossCheckResult& c = r.crossChecks[i];
        if (c.compared && c.agree)
            continue;
        if (!c.compared && rb->knownBuild)
            continue;
        char buf[96];
        if (c.compared)
            std::snprintf(buf, sizeof(buf), " (0x%x vs 0x%x)", c.left, c.right);
        else
            std::snprintf(buf, sizeof(buf), " (not compared: a side is not usable)");
        for (const Feature f : kCrossPairs[i].affects)
        {
            if (f == Feature::Count)
                continue;
            SetOff(r.At(f), c.compared ? "cross-check failed: " + c.name + buf + "; the two groups found different globals"
                                       : "cross-check not confirmed: " + c.name + buf + "; on an unknown build a global without a second derivation is not trusted");
        }
    }

    // ---- the key: an unknown build runs Group 2 only when the player asked for it. The resolver's verdict stays in the reason.
    if (!rb->knownBuild && !s.unknownBuildHooks)
    {
        for (std::size_t i = static_cast<std::size_t>(kFirstGroup2Feature); i < kFeatureCount; ++i)
        {
            Decision& d = r.features[i];
            const std::string verdict = d.enabled ? "seams allow it" : d.reason;
            SetOff(d, std::string("unknown build and FfxivUnknownBuildHooks is off (resolver verdict: ") + verdict + ")");
        }
    }
    return r;
}

// Group 1 and nothing else, for the callers that only need the exe-name rule (the game selector at the first device hook, before or without a
// resolve): the same decisions Decide() gives for these features.
inline Decision DecideGroup1(Feature f, bool exeIsFfxiv, bool exactKnownBuild, const GateSettings& s)
{
    GateInput in;
    in.exeIsFfxiv = exeIsFfxiv;
    in.exactKnownBuild = exactKnownBuild;
    return Decide(in, s).features[static_cast<std::size_t>(f)];
}

// ---- the "not self-healing" line --------------------------------------------------------------------------------------------------------

// Offset fields of the build that no usable seam fills, with their current value, as one line. On an unknown build these keep the table
// default: if the patch moved them the feature reads the wrong field (the strict shapes and the runtime range checks are what protect).
inline std::vector<std::string> UnhealedOffsets(const ResolvedBuild& rb, const Registry& reg)
{
    std::vector<std::string> out;
    for (const FieldRow& row : kFieldTable)
    {
        if (row.kind != FieldKind::Offset)
            continue;
        char key[96];
        if (FieldKey(row, key, sizeof(key)) == 0)
            continue;
        const Binding* bound = nullptr;
        for (const Binding& b : reg.bindings)
        {
            if (b.key == key)
                bound = &b;
        }
        bool healed = false;
        if (bound != nullptr)
            healed = rb.Usable(bound->seam);
        if (healed)
            continue;
        char text[160];
        const std::uint32_t* v = FieldU32(rb.build, row);
        std::snprintf(text, sizeof(text), "%s=0x%x%s", key, v != nullptr ? *v : 0u, bound == nullptr ? "" : " (seam unusable)");
        out.emplace_back(text);
    }
    return out;
}

inline std::string FormatNotSelfHealing(const ResolvedBuild& rb, const Registry& reg, bool knownBuild)
{
    const std::vector<std::string> list = UnhealedOffsets(rb, reg);
    std::string line = "[ffxiv-gate] not self-healing: " + std::to_string(list.size()) + " struct offsets keep their table values on this build (" +
                       std::string(knownBuild ? "known build, table verified offline" : "UNKNOWN build: a patch that moved one of them is not followed") + "): ";
    for (std::size_t i = 0; i < list.size(); ++i)
        line += (i ? ", " : "") + list[i];
    line += ". Decoded and range-checked but pinned, not healed: nativeQuality.contextCacheOffset, charaView.targetsOffset and every jitterSource offset "
            "(a moved one refuses the hook).";
    return line;
}

// The result's own mark, for the callers that log a resolve that ran (the tests and the unknown-build path):
// there the result is the verdict. The gate block passes the gate's verdict instead (FormatGateBlock below),
// so a failed resolve on the exact known build still reads as the known build it runs on.
inline std::string FormatNotSelfHealing(const ResolvedBuild& rb, const Registry& reg)
{
    return FormatNotSelfHealing(rb, reg, rb.knownBuild);
}
} // namespace ext::ffxiv::resolve
