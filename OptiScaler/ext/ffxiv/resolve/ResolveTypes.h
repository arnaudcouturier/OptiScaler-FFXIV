#pragma once
// Pattern-search resolver: the frozen shared types (status, anchor/seam/predicate specs, results, the resolved build).
//
// FROZEN. The anchor writers add seams and bindings in their own
// Anchors*.h file only; they must not edit this file, Registry.h, FieldTable.h or any other resolver header.
//
// Header-only and free of namespace-scope objects with a constructor: nothing here runs at load time, and the product calls the
// resolver once, from the startup at the first device hook (ResolveStartup.cpp). No <windows.h>, no logging macros (see ResolveLog.h for the product sink).

#include "FieldTable.h"
#include "ext/ffxiv/GameBuild.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ext::ffxiv::resolve
{
// ---- Results ----------------------------------------------------------------------------------------------------------

// How one seam ended. Exactly these six values; the log line prints the name.
enum class SeamStatus : std::uint8_t
{
    Found,             // every anchor that could be evaluated agreed on one unique value (two or more anchors, or one anchor in a one-anchor seam)
    FoundSingleAnchor, // one unique anchor, the other anchors found nothing: usable, logged as a WARN
    NotFound,          // no anchor produced a candidate (also: a dependency of the seam did not resolve)
    Ambiguous,         // the pattern and the predicates left more than the expected number of candidates
    Conflict,          // two anchors disagree (or a unique anchor is not among another anchor's candidates): the seam is inert
    EntryPatched,      // the function entry already starts with E9 / EB / FF 25: a foreign hook (e.g. Dalamud). Distinct from NotFound.
};

inline const char* StatusName(SeamStatus s)
{
    switch (s)
    {
    case SeamStatus::Found:
        return "Found";
    case SeamStatus::FoundSingleAnchor:
        return "FoundSingleAnchor";
    case SeamStatus::NotFound:
        return "NotFound";
    case SeamStatus::Ambiguous:
        return "Ambiguous";
    case SeamStatus::Conflict:
        return "Conflict";
    case SeamStatus::EntryPatched:
        return "EntryPatched";
    }
    return "Unknown";
}

// A feature may use the seam's value only for these two statuses (EntryPatched has a value but the entry is not ours to hook).
inline bool IsUsable(SeamStatus s) { return s == SeamStatus::Found || s == SeamStatus::FoundSingleAnchor; }
// The seam carries a meaningful value (also true for EntryPatched, which dependents and the report may read).
inline bool HasValue(SeamStatus s) { return IsUsable(s) || s == SeamStatus::EntryPatched; }

enum class AnchorOutcome : std::uint8_t
{
    Unique,            // exactly `expectedHits` candidates survived: the value is set
    Zero,              // no candidate (no hit, or every hit was rejected by decode/predicates, or the image could not be read)
    Multiple,          // more (or fewer, when expectedHits > 1) candidates than expected
    DependencyMissing, // a seam this anchor depends on has no usable value
    Invalid,           // the anchor spec itself is wrong (caught by ValidateRegistry; never produces a value)
    Patched,           // the relaxed retry found the function with a jump written over its entry (only ever yields EntryPatched)
};

enum class Agreement : std::uint8_t
{
    NotChecked, // no known-build table was supplied, or the seam has no value or no table binding
    Agree,
    Disagree, // the table value wins (CrossCheck copies it into the resolved build) and a WARN is logged
};

struct AnchorResult
{
    std::string name;
    AnchorOutcome outcome = AnchorOutcome::Zero;
    std::uint32_t patternHits = 0; // raw matches of the pattern before decode and predicates (0 for a guard anchor that failed)
    std::uint32_t candidates = 0;  // candidates left after decode and predicates
    bool overflow = false;         // the raw hit count reached the scan cap (kMaxScanHits)
    bool hasValue = false;
    std::uint32_t value = 0;                    // the anchor's value (valid when hasValue)
    std::uint32_t match = 0;                    // RVA where the chosen pattern match starts
    std::vector<std::uint32_t> candidateValues; // up to 64 values of the surviving candidates (for the multi-anchor policy and reports)
    std::vector<std::uint32_t> candidateMatches; // the pattern match RVAs of those candidates (the offline margin check excludes them)
    std::string note;                           // why zero / invalid / dropped candidates (empty on a clean unique hit)
    bool weakDependency = false;                // a seam this anchor depends on (fromSeam, a predicate) is weak: EntryPatched or FoundSingleAnchor
};

struct SeamResult
{
    std::string id;
    SeamStatus status = SeamStatus::NotFound;
    bool hasValue = false;     // HasValue(status)
    std::uint32_t rva = 0;     // the resolved value: an RVA for Function/Global/Table seams, the raw number for Offset seams
    std::uint32_t hits = 0;    // sum of the anchors' surviving candidates (the number in the log line is per anchor: hits=1+0)
    std::vector<AnchorResult> anchors;
    std::uint64_t micros = 0;  // time spent evaluating this seam (decode, predicates, combine); the shared image scan is in ResolvedBuild::scanMicros
    std::string reason;        // empty for a clean Found
    bool hasTable = false;     // a known-build table value was compared
    std::uint32_t tableValue = 0;
    Agreement agreement = Agreement::NotChecked;
    bool fellBackToTable = false;   // known build only: the seam is not usable and every field bound to it was filled from the table (the status stays honest)
    bool viaWeakDependency = false; // the value rests on a weak seam (EntryPatched / FoundSingleAnchor) with no independent strong anchor: capped at FoundSingleAnchor
};

// ---- Specs ------------------------------------------------------------------------------------------------------------

enum class SeamKind : std::uint8_t
{
    Function, // the value is a function entry RVA: the entry bytes are checked for a foreign hook (EntryPatched)
    CodeSite, // the value is an RVA inside code that is NOT a function entry (a call site, a mid-function instruction): no jump check, never EntryPatched
    Global,   // the value is the RVA of a data global (usually from a RIP-relative load)
    Table,    // the value is the RVA of a data table
    Offset,   // the value is a raw number (a struct offset or an immediate), not an RVA
};

// Which sections the pattern is searched in.
enum class SearchIn : std::uint8_t
{
    Code,         // sections with the executable flag, [rva, rva + virtualSize)
    ReadOnlyData, // initialized, readable, not executable, not writable, not discardable (.rdata); .pdata and .rsrc excluded
    InitData,     // writable, not executable: the initialized part only, [rva, rva + min(rawSize, virtualSize))
};

// A region a decoded RIP target must lie in.
enum class RegionKind : std::uint8_t
{
    Image,        // anywhere inside a section
    Code,         // an executable section
    ReadOnlyData, // as SearchIn::ReadOnlyData
    InitData,     // the initialized part of a writable section
    Bss,          // the part of a writable section above its initialized data (or a section flagged uninitialized)
};

enum class DecodeKind : std::uint8_t
{
    None,       // no decode: the candidate has no target
    RipDisp32,  // target = match + at + length + int32 at (match + at + dispAt)   (mov rax,[rip+disp32], lea, cmp ...)
    Rel32,      // E8 call / E9 jmp rel32 at (match + at): target = match + at + 5 + int32 at (match + at + 1)
    Field,      // a little-endian unsigned value of `width` (1, 2 or 4) bytes at (match + at): a struct offset, an immediate
    RipDeref32, // like RipDisp32, then the uint32 stored at the target (a constant read from .rdata, e.g. a float's bits)
};

struct DecodeSpec
{
    DecodeKind kind = DecodeKind::None;
    std::int32_t at = 0;       // offset inside the match of the instruction (RipDisp32, Rel32, RipDeref32) or of the field (Field)
    std::uint8_t dispAt = 0;   // RipDisp32 / RipDeref32: offset of the disp32 inside the instruction
    std::uint8_t length = 0;   // RipDisp32 / RipDeref32: instruction length (the rip value is the instruction start + length)
    std::uint8_t width = 0;    // Field: 1, 2 or 4
};

enum class ValueFrom : std::uint8_t
{
    Match,             // value = match + valueOffset (the usual case: offset to the function entry or to the instruction)
    EnclosingFunction, // value = start of the function that contains (match + valueOffset), from the .pdata unwind table (chains followed).
                       // A leaf function without an unwind entry gives NoUnwindEntry: the candidate is dropped, never guessed.
    Target,            // value = the decoded target / field (DecodeSpec.kind must not be None)
};

enum class PredicateKind : std::uint8_t
{
    TargetIn,        // the decoded RIP target lies in `region`
    TargetIsSeam,    // the decoded target equals the value of seam `seam`
    CallsSeam,       // Rel32 only: the instruction is an E8 call and its target equals the value of seam `seam`
    Within,          // (match RVA - value of seam `seam`) lies in [lo, hi]: position / order relative to another resolved seam
    SameFunctionAs,  // the match lies in the same function (.pdata, chains followed) as the value of seam `seam`
    ValueIn,         // Field / RipDeref32 only: the decoded value (a struct offset, an immediate, a constant) lies in [valueLo, valueHi] (unsigned, inclusive)
};

struct Predicate
{
    PredicateKind kind = PredicateKind::TargetIn;
    RegionKind region = RegionKind::Image;
    std::string seam {};       // owned: a registry built from dynamic strings never dangles
    std::int32_t lo = 0;       // Within: signed distance bounds
    std::int32_t hi = 0;
    std::uint32_t valueLo = 0; // ValueIn: unsigned bounds (a float's bits do not fit an int32)
    std::uint32_t valueHi = 0;
};

namespace pred
{
inline Predicate TargetIn(RegionKind r) { return { PredicateKind::TargetIn, r, {}, 0, 0, 0, 0 }; }
inline Predicate TargetIs(std::string seam) { return { PredicateKind::TargetIsSeam, RegionKind::Image, std::move(seam), 0, 0, 0, 0 }; }
inline Predicate CallsSeam(std::string seam) { return { PredicateKind::CallsSeam, RegionKind::Image, std::move(seam), 0, 0, 0, 0 }; }
// match - seamValue in [lo, hi]
inline Predicate Within(std::string seam, std::int32_t lo, std::int32_t hi)
{
    return { PredicateKind::Within, RegionKind::Image, std::move(seam), lo, hi, 0, 0 };
}
// the match lies 1..maxGap bytes after the seam's value
inline Predicate After(std::string seam, std::int32_t maxGap) { return Within(std::move(seam), 1, maxGap); }
// the match lies 1..maxGap bytes before the seam's value
inline Predicate Before(std::string seam, std::int32_t maxGap) { return Within(std::move(seam), -maxGap, -1); }
inline Predicate SameFunctionAs(std::string seam) { return { PredicateKind::SameFunctionAs, RegionKind::Image, std::move(seam), 0, 0, 0, 0 }; }
// the decoded value of a Field or RipDeref32 anchor lies in [lo, hi] (unsigned, inclusive): the sanity check of a struct offset or a constant
inline Predicate ValueIn(std::uint32_t lo, std::uint32_t hi) { return { PredicateKind::ValueIn, RegionKind::Image, {}, 0, 0, lo, hi }; }
inline Predicate ValueEquals(std::uint32_t v) { return ValueIn(v, v); }
} // namespace pred

inline constexpr std::size_t kMaxScanHits = 4096; // raw matches kept per anchor; more than this = overflow (the anchor is Multiple)

// One independent way of finding a seam's value. Aggregate: use C++20 designated initializers IN THIS ORDER (fields you skip keep
// their defaults).
struct AnchorSpec
{
    std::string name {};      // short label for logs and reports ("accessorLoad")
    std::string pattern {};   // IDA style: "48 8B 05 ?? ?? ?? ?? 8B CA"; "??" wildcard byte, "4?" / "?5" nibble wildcards
    SearchIn searchIn = SearchIn::Code;
    std::uint32_t expectedHits = 1; // candidates (after decode and predicates) that must exist; > 1 for one pattern that marks several sites
    std::uint32_t pickIndex = 0;    // which of the expectedHits candidates (ascending address) is this seam's
    std::string fromSeam {};        // when non-empty the pattern is NOT scanned: it is verified (as a guard) at value(fromSeam) + fromOffset
    std::int32_t fromOffset = 0;
    ValueFrom valueFrom = ValueFrom::Match;
    std::int32_t valueOffset = 0;   // Match / EnclosingFunction: added to the match start (usually 0, or minus the offset of the function entry)
    DecodeSpec decode {};
    bool entryAnchor = false;       // the pattern starts at the function entry: on zero hits, one relaxed retry (first 14 bytes masked)
                                    // may report EntryPatched, never Found
    std::uint32_t minMargin = 3;    // offline gate (tests and the CLI only): required distance to the nearest non-resolved site; 0 = the
                                    // predicates are the disambiguator
    std::vector<Predicate> predicates {};
};

struct SeamSpec
{
    std::string id {};         // "chara.accessor": group.name, unique
    SeamKind kind = SeamKind::Function;
    std::vector<AnchorSpec> anchors {}; // one or more independent anchors (the multi-anchor policy in Resolver.h)
    // true: on a KNOWN build this seam is never filled from the table when it is not usable (its bound fields stay zero, fellBackToTable
    // stays false, UsableOrTable is false). For SHAPE seams whose Guard field is the only strict check of a hook site: the table's guard array
    // is a short prefix of the strict pattern, so a table fallback would weaken the check to that prefix. A zero Guard makes the hook's own
    // live-bytes-vs-guard compare refuse to install, on every build.
    bool failClosed = false;
};

// Which GameBuild field a seam fills. `key` names a row of FieldTable.h ("charaView.accessorRva"). For a Guard field the bytes are read
// from the image at value(seam) + guardOffset, with the field's own length.
struct Binding
{
    std::string key {};
    std::string seam {};
    std::int32_t guardOffset = 0; // Guard fields: the bytes are read at value(seam) + guardOffset
    std::int32_t valueOffset = 0; // Rva and Offset fields: the field gets value(seam) + valueOffset ("producerReturnRva = producerCall + 5")
};

// What the three Anchors*.h files fill (one Add...Anchors function each). Built inside BuildRegistry(), never at namespace scope.
struct Registry
{
    std::vector<SeamSpec> seams {};
    std::vector<Binding> bindings {};
    // Groups of seam ids (Function / CodeSite seams) that must resolve to DIFFERENT values (batch16 / batch32). The later seam of a group
    // (list order = resolution order: list a seam after the seams it depends on) that resolves to a value an earlier member already has
    // becomes Conflict, before anything depends on it. A group needs at least two ids.
    std::vector<std::vector<std::string>> mustDiffer {};
};

// ---- Logging ----------------------------------------------------------------------------------------------------------

enum class LogLevel : std::uint8_t
{
    Info,
    Warn,
    Error,
};
using LogSink = std::function<void(LogLevel, const std::string&)>;

// ---- The resolved build -----------------------------------------------------------------------------------------------

// What replaces the constexpr kCurrentBuild reads. `build` has the same field names and types as the table: build.nativeQuality,
// build.charaView, build.jitter, build.jitterSource. Every Rva and Guard field starts at 0 and is filled only by a
// seam that resolved with a usable status (and by the table when the cross-check disagrees); fixed offsets and constants keep the
// table defaults ("not self-healing") unless a bound Offset seam fills them. Check the seam before trusting a field.
struct ResolvedBuild
{
    GameBuild build = ZeroedBuild(); // Rva / Guard fields, the stamp and the size start at ZERO, never the C table values
    std::uint32_t timeDateStamp = 0; // of the image that was scanned
    std::uint32_t sizeOfImage = 0;
    bool knownBuild = false;         // a known-build table was supplied AND its stamp and size equal the image's (a table of another build is ignored)
    std::vector<SeamResult> seams {};
    std::vector<std::string> registryErrors {}; // non-empty = the registry itself is wrong; every seam is NotFound
    // Set when the shared image scan (or the relaxed entry scan) hit a window it could not read and was aborted. Every seam that
    // depends on a scanned anchor is then NotFound with the reason in its anchor note; the gate treats it as an INFRASTRUCTURE failure (not a verdict
    // about the executable) so that the exact known build can fall back to the legacy decision.
    bool scanFailed = false;
    std::string scanError {};
    std::uint32_t scanRequests = 0;  // distinct (pattern, section class) pairs the shared scan looked for
    std::uint64_t scanMicros = 0;    // the shared image scan
    std::uint64_t totalMicros = 0;   // everything, including the scan and the logging

    const SeamResult* Find(std::string_view id) const
    {
        for (const SeamResult& s : seams)
        {
            if (s.id == id)
                return &s;
        }
        return nullptr;
    }
    bool Usable(std::string_view id) const
    {
        const SeamResult* s = Find(id);
        return s != nullptr && IsUsable(s->status);
    }
    // The seam resolved, or (known build only) its bound fields were filled from the table, which is verified offline against this exact
    // executable. Never true on an unknown build for a seam that did not resolve.
    bool UsableOrTable(std::string_view id) const
    {
        const SeamResult* s = Find(id);
        return s != nullptr && (IsUsable(s->status) || s->fellBackToTable);
    }
    std::size_t Count(SeamStatus status) const
    {
        std::size_t n = 0;
        for (const SeamResult& s : seams)
            n += s.status == status ? 1 : 0;
        return n;
    }
};

struct ResolveOptions
{
    // A known build: used ONLY when its timeDateStamp and sizeOfImage equal the image's (otherwise ignored with one WARN, knownBuild = false).
    // Then every bound value of a usable seam is cross-checked (the table wins on a disagreement, WARN) and a seam that is not usable has
    // its bound fields filled from the table (fellBackToTable, WARN).
    const GameBuild* knownTable = nullptr;
    LogSink sink {};                       // one line per seam plus one total line; empty = silent
};
} // namespace ext::ffxiv::resolve
