#pragma once
// The resolver: turns a Registry (seams, each with one or more independent anchors) into a ResolvedBuild by scanning an ImageView.
//
//   ValidateRegistry   the registry itself is checked first (ids, patterns, decode parameters, dependencies, cycles, bindings). A broken
//                      registry resolves nothing: every seam is NotFound and the errors are in ResolvedBuild::registryErrors.
//   ResolveImage       one pass over the image for every pattern (ScanBatch), then the seams in dependency order. Per anchor: raw matches
//                      -> decode (RIP target, rel32, raw field, deref) -> value (match offset, enclosing function, decoded) -> predicates.
//                      An anchor is Unique only when EXACTLY `expectedHits` candidates survive. Seams combine their anchors by the
//                      multi-anchor policy below. Function seams are checked for a foreign hook at the entry (EntryPatched); CodeSite seams
//                      (a position inside code) are not. The known-build table (ResolveOptions::knownTable) counts only when its stamp and
//                      size equal the image's; then CrossCheck compares usable seams and fills the fields of unusable ones from it.
//
// Multi-anchor policy (the first three rows are the settled rule; the others extend them):
//   every unique anchor agrees, 2+ of them      Found
//   one anchor, one unique hit                  Found (a seam with a single anchor)
//   one unique anchor, the other anchors found nothing        FoundSingleAnchor (a WARN)
//   unique anchors disagree                     Conflict (inert, no value)
//   one unique anchor + another anchor with several candidates: FoundSingleAnchor when the unique value is among those candidates,
//                                                Conflict when it is not
//   no unique anchor, some anchor ambiguous     Ambiguous;   no unique anchor and none ambiguous: NotFound
//
// ResolveImage never throws for a bad image or registry; an exception from the image reader (bad_alloc, a throwing test reader) propagates
// to the caller (ResolveCache turns it into a cached failure).
//
// WHEN TO CALL (the product, P3): once, before the fork installs any Detours, and not under the loader lock: the scan reads a 35 MB code
// section and takes a few milliseconds to tens of milliseconds. ResolveCache.h wraps this in "resolve once, cache, never re-scan".

#include "FieldTable.h"
#include "ImageView.h"
#include "Pattern.h"
#include "ResolveLog.h"
#include "ResolveTypes.h"
#include "Scan.h"
#include "Unwind.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace ext::ffxiv::resolve
{
namespace detail
{
inline bool IsPredicateWithSeam(PredicateKind k)
{
    return k == PredicateKind::TargetIsSeam || k == PredicateKind::CallsSeam || k == PredicateKind::Within || k == PredicateKind::SameFunctionAs;
}

inline void AnchorDeps(const AnchorSpec& a, std::vector<std::string_view>& out)
{
    if (!a.fromSeam.empty())
        out.emplace_back(a.fromSeam);
    for (const Predicate& p : a.predicates)
    {
        if (IsPredicateWithSeam(p.kind) && !p.seam.empty())
            out.emplace_back(p.seam);
    }
}

// Index of the seam called `id`, or SIZE_MAX.
inline std::size_t SeamIndex(const std::vector<SeamSpec>& seams, std::string_view id)
{
    for (std::size_t i = 0; i < seams.size(); ++i)
    {
        if (!seams[i].id.empty() && id == seams[i].id)
            return i;
    }
    return static_cast<std::size_t>(-1);
}

// seam -> the seams it must wait for: those its anchors depend on (fromSeam, predicates) plus, inside each mustDiffer group, the member
// listed right before it (so that "the later member of a group" is well defined whatever the registry order).
inline std::vector<std::vector<std::size_t>> SeamDeps(const Registry& registry)
{
    const std::size_t n = registry.seams.size();
    std::vector<std::vector<std::size_t>> deps(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        std::vector<std::string_view> names;
        for (const AnchorSpec& a : registry.seams[i].anchors)
            AnchorDeps(a, names);
        for (std::string_view name : names)
        {
            const std::size_t j = SeamIndex(registry.seams, name);
            if (j != static_cast<std::size_t>(-1) && j != i)
                deps[i].push_back(j);
        }
    }
    for (const std::vector<std::string>& group : registry.mustDiffer)
    {
        for (std::size_t k = 1; k < group.size(); ++k)
        {
            const std::size_t prev = SeamIndex(registry.seams, group[k - 1]);
            const std::size_t cur = SeamIndex(registry.seams, group[k]);
            if (prev != static_cast<std::size_t>(-1) && cur != static_cast<std::size_t>(-1) && prev != cur)
                deps[cur].push_back(prev);
        }
    }
    return deps;
}

inline bool IsAddressDecode(DecodeKind k) { return k == DecodeKind::RipDisp32 || k == DecodeKind::Rel32 || k == DecodeKind::RipDeref32; }
} // namespace detail

// All problems of the registry, empty when it is sound.
inline std::vector<std::string> ValidateRegistry(const Registry& registry)
{
    using detail::AnchorDeps;
    std::vector<std::string> errors;
    auto err = [&errors](std::string text) { errors.push_back(std::move(text)); };

    std::vector<std::string_view> ids;
    for (std::size_t i = 0; i < registry.seams.size(); ++i)
    {
        const SeamSpec& s = registry.seams[i];
        const std::string& id = s.id;
        if (id.empty())
        {
            err("seam #" + std::to_string(i) + " has no id");
            continue;
        }
        if (std::find(ids.begin(), ids.end(), std::string_view(s.id)) != ids.end())
            err("duplicate seam id '" + id + "'");
        ids.emplace_back(s.id);
    }
    auto known = [&ids](std::string_view id) { return !id.empty() && std::find(ids.begin(), ids.end(), id) != ids.end(); };
    auto kindOf = [&registry](std::string_view id)
    {
        const std::size_t i = detail::SeamIndex(registry.seams, id);
        return i == static_cast<std::size_t>(-1) ? SeamKind::Function : registry.seams[i].kind;
    };
    auto isOffsetSeam = [&](std::string_view id) { return known(id) && kindOf(id) == SeamKind::Offset; };

    for (const SeamSpec& s : registry.seams)
    {
        const std::string& id = s.id;
        if (id.empty())
            continue;
        if (s.anchors.empty())
            err("seam '" + id + "' has no anchor");
        for (const AnchorSpec& a : s.anchors)
        {
            const std::string where = "seam '" + id + "' anchor '" + a.name + "': ";
            Pattern p;
            std::string perr;
            if (!ParsePattern(a.pattern, p, &perr))
                err(where + "bad pattern (" + perr + ")");
            if (a.minMargin == 0 && a.fromSeam.empty() &&
                std::none_of(a.predicates.begin(), a.predicates.end(),
                             [](const Predicate& pr) { return !(pr.kind == PredicateKind::TargetIn && pr.region == RegionKind::Image); }))
                err(where + "minMargin 0 means 'the predicates are the disambiguator': it needs at least one effective predicate (TargetIn(Image) is a no-op)");
            if (a.expectedHits == 0)
                err(where + "expectedHits must be at least 1");
            else if (a.pickIndex >= a.expectedHits)
                err(where + "pickIndex is not below expectedHits");
            if (a.valueFrom == ValueFrom::Target && a.decode.kind == DecodeKind::None)
                err(where + "valueFrom Target needs a decode");
            if (a.valueFrom == ValueFrom::Target && a.valueOffset != 0)
                err(where + "valueOffset is ignored for a decoded value: it must be 0");
            switch (a.decode.kind)
            {
            case DecodeKind::RipDisp32:
            case DecodeKind::RipDeref32:
                if (a.decode.length == 0 || a.decode.length > 15 || static_cast<unsigned>(a.decode.dispAt) + 4 > a.decode.length)
                    err(where + "RIP decode: the disp32 must lie inside the instruction (dispAt + 4 <= length <= 15)");
                else if (a.decode.dispAt < 2)
                    err(where + "RIP decode: dispAt must be at least 2 (an opcode and the RIP-relative ModRM byte precede the disp32)");
                break;
            case DecodeKind::Field:
                if (a.decode.width != 1 && a.decode.width != 2 && a.decode.width != 4)
                    err(where + "Field decode: width must be 1, 2 or 4");
                break;
            case DecodeKind::Rel32:
            case DecodeKind::None:
                break;
            }
            if (!a.fromSeam.empty())
            {
                if (!known(a.fromSeam))
                    err(where + "fromSeam '" + a.fromSeam + "' is not a seam");
                else if (id == a.fromSeam)
                    err(where + "fromSeam is the seam itself");
                else if (isOffsetSeam(a.fromSeam))
                    err(where + "fromSeam '" + a.fromSeam + "' is an Offset seam: its value is not an RVA");
                if (a.entryAnchor)
                    err(where + "a guard anchor (fromSeam) cannot be an entryAnchor");
            }
            if (a.entryAnchor)
            {
                if (s.kind != SeamKind::Function)
                    err(where + "entryAnchor is for Function seams (a CodeSite is not an entry)");
                if (a.valueFrom != ValueFrom::Match || a.valueOffset != 0 || a.expectedHits != 1)
                    err(where + "an entryAnchor's pattern starts at the entry: valueFrom Match, valueOffset 0, expectedHits 1");
            }
            for (const Predicate& pr : a.predicates)
            {
                if (detail::IsPredicateWithSeam(pr.kind))
                {
                    if (!known(pr.seam))
                        err(where + "a predicate names the unknown seam '" + pr.seam + "'");
                    else if (id == pr.seam)
                        err(where + "a predicate depends on the seam itself");
                    else if (isOffsetSeam(pr.seam))
                        err(where + "a predicate names the Offset seam '" + pr.seam + "': its value is not an RVA");
                }
                if ((pr.kind == PredicateKind::TargetIn || pr.kind == PredicateKind::TargetIsSeam) && !detail::IsAddressDecode(a.decode.kind))
                    err(where + "TargetIn / TargetIs need an address decode (RipDisp32, Rel32 or RipDeref32)");
                if (pr.kind == PredicateKind::CallsSeam && a.decode.kind != DecodeKind::Rel32)
                    err(where + "CallsSeam needs a Rel32 decode");
                if (pr.kind == PredicateKind::Within && pr.lo > pr.hi)
                    err(where + "Within: lo is above hi");
                if (pr.kind == PredicateKind::ValueIn)
                {
                    if (a.decode.kind != DecodeKind::Field && a.decode.kind != DecodeKind::RipDeref32)
                        err(where + "ValueIn needs a Field or RipDeref32 decode (a value, not an address)");
                    if (pr.valueLo > pr.valueHi)
                        err(where + "ValueIn: lo is above hi");
                }
            }
        }
    }

    // mustDiffer groups: at least two known Function / CodeSite seams, no repeats
    for (std::size_t g = 0; g < registry.mustDiffer.size(); ++g)
    {
        const std::vector<std::string>& group = registry.mustDiffer[g];
        const std::string where = "mustDiffer group #" + std::to_string(g) + ": ";
        if (group.size() < 2)
            err(where + "a group needs at least two seams");
        for (std::size_t k = 0; k < group.size(); ++k)
        {
            if (!known(group[k]))
            {
                err(where + "'" + group[k] + "' is not a seam");
                continue;
            }
            const SeamKind kind = kindOf(group[k]);
            if (kind != SeamKind::Function && kind != SeamKind::CodeSite)
                err(where + "'" + group[k] + "' is not a Function or CodeSite seam");
            if (std::find(group.begin(), group.begin() + static_cast<std::ptrdiff_t>(k), group[k]) != group.begin() + static_cast<std::ptrdiff_t>(k))
                err(where + "'" + group[k] + "' is listed twice");
        }
    }

    // cycles: depth-first search over "seam depends on seam" (anchor dependencies and the order inside mustDiffer groups)
    const std::vector<std::vector<std::size_t>> deps = detail::SeamDeps(registry);
    std::vector<int> mark(registry.seams.size(), 0); // 0 new, 1 on the stack, 2 done
    std::function<bool(std::size_t)> hasCycle = [&](std::size_t i) -> bool
    {
        mark[i] = 1;
        for (std::size_t j : deps[i])
        {
            if (mark[j] == 1 || (mark[j] == 0 && hasCycle(j)))
                return true;
        }
        mark[i] = 2;
        return false;
    };
    for (std::size_t i = 0; i < registry.seams.size(); ++i)
    {
        if (mark[i] == 0 && hasCycle(i))
        {
            err("dependency cycle through seam '" + registry.seams[i].id + "'");
            break;
        }
    }

    // bindings
    std::vector<std::string_view> keys;
    for (const Binding& b : registry.bindings)
    {
        const std::string& key = b.key;
        const FieldRow* row = FindField(key);
        if (row == nullptr)
        {
            err("binding '" + key + "': no such GameBuild field");
            continue;
        }
        if (std::find(keys.begin(), keys.end(), std::string_view(b.key)) != keys.end())
            err("binding '" + key + "' appears twice");
        keys.emplace_back(b.key);
        if (row->kind == FieldKind::Constant || row->kind == FieldKind::Pattern)
            err("binding '" + key + "': a constant or the pattern is never resolved");
        if (b.guardOffset != 0 && row->kind != FieldKind::Guard)
            err("binding '" + key + "': guardOffset is only for Guard fields");
        if (b.valueOffset != 0 && row->kind != FieldKind::Rva && row->kind != FieldKind::Offset)
            err("binding '" + key + "': valueOffset is only for Rva and Offset fields (a Guard field uses guardOffset)");
        const SeamSpec* seam = nullptr;
        for (const SeamSpec& s : registry.seams)
        {
            if (!s.id.empty() && b.seam == s.id)
                seam = &s;
        }
        if (seam == nullptr)
        {
            err("binding '" + key + "': no such seam '" + b.seam + "'");
            continue;
        }
        if (row->kind == FieldKind::Offset && seam->kind != SeamKind::Offset)
            err("binding '" + key + "': an Offset field needs an Offset seam");
        if ((row->kind == FieldKind::Rva || row->kind == FieldKind::Guard) && seam->kind == SeamKind::Offset)
            err("binding '" + key + "': an address field needs a Function, Global or Table seam");
    }
    return errors;
}

namespace detail
{
using Clock = std::chrono::steady_clock;
inline std::uint64_t MicrosSince(Clock::time_point t0)
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count());
}

struct Candidate
{
    std::uint32_t match = 0;
    bool hasTarget = false;
    std::uint32_t target = 0;
    std::uint32_t value = 0;
    std::uint8_t opcode = 0;   // Rel32: the byte at the instruction start
    bool hasDecoded = false;   // Field / RipDeref32: `decoded` is the value read (the ValueIn predicate tests it)
    std::uint32_t decoded = 0;
};

struct SeamState
{
    bool hasValue = false;
    std::uint32_t value = 0;
    bool weak = false; // EntryPatched or FoundSingleAnchor: whatever depends on it is capped at FoundSingleAnchor unless another anchor stands alone
};

inline std::string Hex(std::uint32_t v) { return HexValue(v); }

// What an entry that already starts with a jump looks like: E9 rel32, EB rel8, FF 25 (jmp [rip+x], the 14 byte absolute form). ONLY these
// three are detected: other hook forms (48 B8 imm64 / FF E0, 68 imm32 / C3, a leading CC) read as NotFound, not EntryPatched.
inline bool EntryIsJump(const std::uint8_t* b, std::size_t n)
{
    if (n >= 1 && (b[0] == 0xe9 || b[0] == 0xeb))
        return true;
    return n >= 2 && b[0] == 0xff && b[1] == 0x25;
}

struct EvalContext
{
    const ImageView& image;
    const std::vector<SeamSpec>& seams;
    const std::vector<SeamState>& states;

    const SeamState* StateOf(std::string_view id) const
    {
        const std::size_t i = SeamIndex(seams, id);
        return i == static_cast<std::size_t>(-1) ? nullptr : &states[i];
    }
};

// Decodes and evaluates one raw match. Returns false (with `why`) when the candidate cannot exist (unreadable bytes, a target outside the
// image, not a call, no unwind entry): such a candidate is dropped, never guessed.
inline bool BuildCandidate(const EvalContext& ctx, const AnchorSpec& spec, std::uint32_t match, Candidate& c, std::string& why)
{
    const PeInfo& info = ctx.image.Info();
    c = Candidate {};
    c.match = match;
    std::uint32_t decoded = 0; // the value a decode yields (target, deref or field)

    switch (spec.decode.kind)
    {
    case DecodeKind::None:
        break;
    case DecodeKind::RipDisp32:
    case DecodeKind::RipDeref32:
    {
        const std::int64_t insn = static_cast<std::int64_t>(match) + spec.decode.at;
        if (insn < 0 || insn > 0xffffffffll)
        {
            why = "the decoded instruction lies outside the image";
            return false;
        }
        // A RIP-relative operand is ModRM mod=00 r/m=101 (no SIB): the byte right before the disp32. A miscounted `length`, or masked
        // bytes that are really a [reg+disp32] operand, would otherwise give a garbage target that still lands in some section.
        std::uint8_t modrm = 0;
        if (spec.decode.dispAt < 1 || !ctx.image.Read(static_cast<std::uint32_t>(insn) + spec.decode.dispAt - 1, &modrm, 1))
        {
            why = "the ModRM byte could not be read";
            return false;
        }
        if ((modrm & 0xc7) != 0x05)
        {
            why = "the byte before the displacement is not a RIP-relative ModRM (mod=00 r/m=101)";
            return false;
        }
        std::int32_t disp = 0;
        if (!ctx.image.Read(static_cast<std::uint32_t>(insn) + spec.decode.dispAt, &disp, sizeof(disp)))
        {
            why = "the displacement bytes could not be read";
            return false;
        }
        const std::int64_t target = insn + spec.decode.length + disp;
        if (target < 0 || target >= static_cast<std::int64_t>(info.sizeOfImage))
        {
            why = "a RIP target at 0x" + Hex(static_cast<std::uint32_t>(target)).substr(2) + " lies outside the image";
            return false;
        }
        c.hasTarget = true;
        c.target = static_cast<std::uint32_t>(target);
        decoded = c.target;
        if (spec.decode.kind == DecodeKind::RipDeref32)
        {
            // Only read-only data: the live process and the offline file show the same bytes there. A deref of mutable memory (.data,
            // bss) reads a runtime value live and the file value offline, so the two answers would differ.
            if (!InRegion(info, RegionKind::ReadOnlyData, c.target))
            {
                why = "a RipDeref32 target must lie in read-only data (a deref of mutable memory differs between the live process and the file)";
                return false;
            }
            std::uint32_t v = 0;
            if (!ctx.image.Read(c.target, &v, sizeof(v)))
            {
                why = "the constant at the RIP target could not be read";
                return false;
            }
            decoded = v;
            c.hasDecoded = true;
            c.decoded = v;
        }
        break;
    }
    case DecodeKind::Rel32:
    {
        const std::int64_t insn = static_cast<std::int64_t>(match) + spec.decode.at;
        std::uint8_t b[5] = {};
        if (insn < 0 || insn > 0xffffffffll || !ctx.image.Read(static_cast<std::uint32_t>(insn), b, sizeof(b)))
        {
            why = "the call or jmp bytes could not be read";
            return false;
        }
        if (b[0] != 0xe8 && b[0] != 0xe9)
        {
            why = "the instruction is neither a call nor a jmp rel32";
            return false;
        }
        std::int32_t rel = 0;
        std::memcpy(&rel, b + 1, sizeof(rel));
        const std::int64_t target = insn + 5 + rel;
        if (target < 0 || target >= static_cast<std::int64_t>(info.sizeOfImage))
        {
            why = "a call target lies outside the image";
            return false;
        }
        c.hasTarget = true;
        c.target = static_cast<std::uint32_t>(target);
        c.opcode = b[0];
        decoded = c.target;
        break;
    }
    case DecodeKind::Field:
    {
        const std::int64_t at = static_cast<std::int64_t>(match) + spec.decode.at;
        std::uint8_t b[4] = {};
        if (at < 0 || at > 0xffffffffll || !ctx.image.Read(static_cast<std::uint32_t>(at), b, spec.decode.width))
        {
            why = "the field bytes could not be read";
            return false;
        }
        std::uint32_t v = 0;
        for (unsigned k = 0; k < spec.decode.width; ++k)
            v |= static_cast<std::uint32_t>(b[k]) << (8 * k);
        decoded = v;
        c.hasDecoded = true;
        c.decoded = v;
        break;
    }
    }

    switch (spec.valueFrom)
    {
    case ValueFrom::Match:
    {
        const std::int64_t v = static_cast<std::int64_t>(match) + spec.valueOffset;
        if (v < 0 || v >= static_cast<std::int64_t>(info.sizeOfImage))
        {
            why = "the value offset leaves the image";
            return false;
        }
        c.value = static_cast<std::uint32_t>(v);
        break;
    }
    case ValueFrom::EnclosingFunction:
    {
        const std::int64_t at = static_cast<std::int64_t>(match) + spec.valueOffset;
        if (at < 0 || at >= static_cast<std::int64_t>(info.sizeOfImage))
        {
            why = "the value offset leaves the image";
            return false;
        }
        const FunctionStartResult fs = FunctionStart(ctx.image, static_cast<std::uint32_t>(at));
        if (fs.status == FunctionStartStatus::NoUnwindEntry)
        {
            why = "no unwind entry covers the match (a leaf function): the function start is not guessed";
            return false;
        }
        if (fs.status != FunctionStartStatus::Found)
        {
            why = "the unwind data could not be read";
            return false;
        }
        c.value = fs.start;
        break;
    }
    case ValueFrom::Target:
        c.value = decoded;
        break;
    }
    return true;
}

inline bool PassesPredicates(const EvalContext& ctx, const AnchorSpec& spec, const Candidate& c, std::string& why)
{
    const PeInfo& info = ctx.image.Info();
    for (const Predicate& p : spec.predicates)
    {
        switch (p.kind)
        {
        case PredicateKind::TargetIn:
            if (!c.hasTarget || !InRegion(info, p.region, c.target))
            {
                why = "the decoded target is not in the required region";
                return false;
            }
            break;
        case PredicateKind::TargetIsSeam:
        {
            const SeamState* s = ctx.StateOf(p.seam);
            if (s == nullptr || !s->hasValue || !c.hasTarget || c.target != s->value)
            {
                why = "the decoded target is not the value of '" + p.seam + "'";
                return false;
            }
            break;
        }
        case PredicateKind::CallsSeam:
        {
            const SeamState* s = ctx.StateOf(p.seam);
            if (s == nullptr || !s->hasValue || !c.hasTarget || c.opcode != 0xe8 || c.target != s->value)
            {
                why = "the instruction is not a call of '" + p.seam + "'";
                return false;
            }
            break;
        }
        case PredicateKind::Within:
        {
            const SeamState* s = ctx.StateOf(p.seam);
            if (s == nullptr || !s->hasValue)
            {
                why = "'" + p.seam + "' has no value";
                return false;
            }
            const std::int64_t delta = static_cast<std::int64_t>(c.match) - static_cast<std::int64_t>(s->value);
            if (delta < p.lo || delta > p.hi)
            {
                why = "the match is not within the required distance of '" + p.seam + "'";
                return false;
            }
            break;
        }
        case PredicateKind::ValueIn:
            if (!c.hasDecoded || c.decoded < p.valueLo || c.decoded > p.valueHi)
            {
                why = "the decoded value is outside the required range";
                return false;
            }
            break;
        case PredicateKind::SameFunctionAs:
        {
            const SeamState* s = ctx.StateOf(p.seam);
            if (s == nullptr || !s->hasValue)
            {
                why = "'" + p.seam + "' has no value";
                return false;
            }
            const FunctionStartResult mine = FunctionStart(ctx.image, c.match);
            const FunctionStartResult other = FunctionStart(ctx.image, s->value);
            if (mine.status != FunctionStartStatus::Found || other.status != FunctionStartStatus::Found)
            {
                why = "no unwind entry: the function of the match or of the seam is unknown";
                return false;
            }
            if (mine.start != other.start)
            {
                why = "the match is in another function than '" + p.seam + "'";
                return false;
            }
            break;
        }
        }
    }
    return true;
}

// Decode + predicates over a list of raw matches. `dropped` counts the rejected ones, `firstWhy` keeps the first reason.
inline std::vector<Candidate> Filter(const EvalContext& ctx, const AnchorSpec& spec, const std::vector<std::uint32_t>& matches, std::size_t& dropped,
                                     std::string& firstWhy)
{
    std::vector<Candidate> out;
    for (std::uint32_t m : matches)
    {
        Candidate c;
        std::string why;
        if (!BuildCandidate(ctx, spec, m, c, why) || !PassesPredicates(ctx, spec, c, why))
        {
            ++dropped;
            if (firstWhy.empty())
                firstWhy = why;
            continue;
        }
        out.push_back(c);
    }
    return out;
}

inline void FillCandidates(AnchorResult& ar, const std::vector<Candidate>& cands)
{
    ar.candidates = static_cast<std::uint32_t>(cands.size());
    for (std::size_t i = 0; i < cands.size() && i < 64; ++i)
    {
        ar.candidateValues.push_back(cands[i].value);
        ar.candidateMatches.push_back(cands[i].match);
    }
}

// One anchor: its raw matches (scanned or verified at a known site) to an outcome. `allValues` receives the value of EVERY surviving
// candidate (the multi-anchor policy needs the whole list; AnchorResult::candidateValues keeps only the first 64 for reports).
// `relaxedUnreadable`: the relaxed entry scan failed part-way, its hit list is partial and is never used.
inline AnchorResult EvaluateAnchorCore(const EvalContext& ctx, const AnchorSpec& spec, const Pattern& pattern, const ScanHits* scanned,
                                       const std::string& scanError, const ScanHits* relaxed, bool relaxedUnreadable,
                                       std::vector<std::uint32_t>& allValues)
{
    AnchorResult ar;
    ar.name = spec.name;

    std::vector<std::string_view> deps;
    AnchorDeps(spec, deps);
    for (std::string_view d : deps)
    {
        const SeamState* s = ctx.StateOf(d);
        if (s == nullptr || !s->hasValue)
        {
            ar.outcome = AnchorOutcome::DependencyMissing;
            ar.note = "dependency '" + std::string(d) + "' has no value";
            return ar;
        }
    }

    std::vector<std::uint32_t> matches;
    if (!spec.fromSeam.empty())
    {
        const SeamState* from = ctx.StateOf(spec.fromSeam);
        const std::int64_t at = static_cast<std::int64_t>(from->value) + spec.fromOffset;
        std::vector<std::uint8_t> bytes(pattern.size());
        if (at < 0 || at > 0xffffffffll || !ctx.image.Read(static_cast<std::uint32_t>(at), bytes.data(), bytes.size()))
        {
            ar.note = "the guard bytes could not be read";
            return ar;
        }
        if (MatchAt(bytes.data(), pattern))
            matches.push_back(static_cast<std::uint32_t>(at));
        else
            ar.note = "the guard bytes at " + Hex(static_cast<std::uint32_t>(at)) + " do not match";
    }
    else if (scanned != nullptr)
    {
        matches = scanned->rvas;
        ar.overflow = scanned->overflow;
    }
    else if (!scanError.empty())
    {
        ar.note = scanError;
        return ar;
    }
    ar.patternHits = static_cast<std::uint32_t>(matches.size());

    std::size_t dropped = 0;
    std::string firstWhy;
    const std::vector<Candidate> cands = Filter(ctx, spec, matches, dropped, firstWhy);
    FillCandidates(ar, cands);
    for (const Candidate& c : cands)
        allValues.push_back(c.value);
    if (dropped != 0)
        ar.note = std::to_string(dropped) + " of " + std::to_string(matches.size()) + " matches dropped: " + firstWhy;

    if (ar.overflow)
    {
        ar.outcome = AnchorOutcome::Multiple;
        ar.note += (ar.note.empty() ? "" : "; ") + std::string("the scan stopped at ") + std::to_string(kMaxScanHits) + " matches";
        return ar;
    }
    if (cands.size() == spec.expectedHits)
    {
        ar.outcome = AnchorOutcome::Unique;
        ar.hasValue = true;
        ar.value = cands[spec.pickIndex].value;
        ar.match = cands[spec.pickIndex].match;
        return ar;
    }
    if (!cands.empty())
    {
        ar.outcome = AnchorOutcome::Multiple;
        return ar;
    }

    ar.outcome = AnchorOutcome::Zero;
    if (ar.note.empty() && spec.fromSeam.empty())
        ar.note = "the pattern was not found";
    if (spec.entryAnchor && ar.patternHits == 0 && relaxedUnreadable)
    {
        // an unreadable relaxed scan is not "no relaxed hit": its partial list could read as exactly one (a false EntryPatched)
        ar.note += "; the relaxed entry scan could not read the image";
        return ar;
    }

    // The relaxed retry: the pattern starts at the function entry and a foreign hook may have written a jump over it. Only when the
    // full pattern had no raw match at all, only with exactly one relaxed match that passes decode and predicates, and only when the
    // entry really starts with a jump. The result is never Found: the best this can say is Patched.
    if (spec.entryAnchor && ar.patternHits == 0 && relaxed != nullptr && !relaxed->rvas.empty())
    {
        std::size_t rDropped = 0;
        std::string rWhy;
        const std::vector<Candidate> rc = Filter(ctx, spec, relaxed->rvas, rDropped, rWhy);
        if (!relaxed->overflow && rc.size() == 1)
        {
            std::uint8_t entry[6] = {};
            std::size_t got = 0;
            for (std::size_t n : { std::size_t(6), std::size_t(2), std::size_t(1) })
            {
                if (ctx.image.Read(rc[0].value, entry, n))
                {
                    got = n;
                    break;
                }
            }
            if (got != 0 && EntryIsJump(entry, got))
            {
                ar.outcome = AnchorOutcome::Patched;
                ar.hasValue = true;
                ar.value = rc[0].value;
                ar.match = rc[0].match;
                ar.candidates = 1;
                ar.candidateValues = { rc[0].value };
                ar.candidateMatches = { rc[0].match };
                ar.note = "relaxed match: the entry is overwritten by a jump";
            }
            else
                ar.note += "; a relaxed match exists but its entry is not a jump";
        }
        else if (rc.size() > 1 || relaxed->overflow)
            ar.note += "; the relaxed match is not unique";
    }
    return ar;
}

// EvaluateAnchorCore plus the weak-dependency mark: an anchor that depends (fromSeam, a predicate) on an EntryPatched or single-anchor seam
// rests on a value nobody verified twice, whatever it finds.
inline AnchorResult EvaluateAnchor(const EvalContext& ctx, const AnchorSpec& spec, const Pattern& pattern, const ScanHits* scanned,
                                   const std::string& scanError, const ScanHits* relaxed, bool relaxedUnreadable, std::vector<std::uint32_t>& allValues)
{
    AnchorResult ar = EvaluateAnchorCore(ctx, spec, pattern, scanned, scanError, relaxed, relaxedUnreadable, allValues);
    std::vector<std::string_view> deps;
    AnchorDeps(spec, deps);
    std::string weak;
    for (std::string_view d : deps)
    {
        const SeamState* s = ctx.StateOf(d);
        if (s != nullptr && s->hasValue && s->weak)
            weak += (weak.empty() ? "" : ", ") + std::string("'") + std::string(d) + "'";
    }
    if (!weak.empty())
    {
        ar.weakDependency = true;
        ar.note += (ar.note.empty() ? "" : "; ") + std::string("depends on the weak seam ") + weak + " (EntryPatched or single-anchor)";
    }
    return ar;
}

// The multi-anchor policy. Fills status, value and reason of `seam` from its anchor results. `allValues[a]` = every candidate value of
// anchor a (EvaluateAnchor). An anchor whose scan overflowed (kMaxScanHits raw matches) has an INCOMPLETE list: it says nothing, it
// neither confirms nor contradicts another anchor's value.
inline void Combine(SeamResult& seam, SeamKind kind, const ImageView& image, const std::vector<std::vector<std::uint32_t>>& allValues)
{
    std::vector<const AnchorResult*> unique, multiple;
    for (const AnchorResult& a : seam.anchors)
    {
        if ((a.outcome == AnchorOutcome::Unique || a.outcome == AnchorOutcome::Patched) && a.hasValue)
            unique.push_back(&a);
        else if (a.outcome == AnchorOutcome::Multiple)
            multiple.push_back(&a);
    }
    seam.hits = 0;
    for (const AnchorResult& a : seam.anchors)
        seam.hits += a.candidates;

    auto notes = [&seam]()
    {
        std::string text;
        for (const AnchorResult& a : seam.anchors)
        {
            if (a.outcome == AnchorOutcome::Unique || a.note.empty())
                continue;
            text += (text.empty() ? "" : "; ") + a.name + ": " + a.note;
        }
        return text;
    };

    if (unique.empty())
    {
        seam.status = multiple.empty() ? SeamStatus::NotFound : SeamStatus::Ambiguous;
        if (!multiple.empty())
        {
            std::size_t most = 0;
            for (const AnchorResult* m : multiple)
                most = std::max<std::size_t>(most, m->candidates);
            seam.reason = "anchor '" + multiple.front()->name + "' left " + std::to_string(most) + " candidates (exactly one is needed)";
            const std::string extra = notes();
            if (!extra.empty())
                seam.reason += "; " + extra;
        }
        else
        {
            seam.reason = "no anchor found it";
            const std::string extra = notes();
            if (!extra.empty())
                seam.reason += ": " + extra;
        }
        return;
    }

    const std::uint32_t value = unique.front()->value;
    for (const AnchorResult* u : unique)
    {
        if (u->value != value)
        {
            seam.status = SeamStatus::Conflict;
            seam.reason = "anchors disagree: '" + unique.front()->name + "' " + Hex(value) + ", '" + u->name + "' " + Hex(u->value);
            return;
        }
    }
    std::string noInfo;
    for (const AnchorResult* m : multiple)
    {
        if (m->overflow)
        {
            noInfo += (noInfo.empty() ? "" : "; ") + std::string("anchor '") + m->name + "' matched too often to say anything";
            continue;
        }
        const std::vector<std::uint32_t>& values = allValues[static_cast<std::size_t>(m - seam.anchors.data())];
        if (std::find(values.begin(), values.end(), value) == values.end())
        {
            seam.status = SeamStatus::Conflict;
            seam.reason = "'" + unique.front()->name + "' found " + Hex(value) + " but the candidates of '" + m->name + "' do not include it";
            return;
        }
    }

    seam.hasValue = true;
    seam.rva = value;
    const bool patched = std::any_of(unique.begin(), unique.end(), [](const AnchorResult* a) { return a->outcome == AnchorOutcome::Patched; });
    if (unique.size() >= 2 || seam.anchors.size() == 1)
        seam.status = SeamStatus::Found;
    else
    {
        seam.status = SeamStatus::FoundSingleAnchor;
        seam.reason = "only anchor '" + unique.front()->name + "' resolved";
        const std::string extra = notes();
        if (!extra.empty())
            seam.reason += "; " + extra;
    }
    if (!noInfo.empty() && seam.status == SeamStatus::FoundSingleAnchor)
        seam.reason += (seam.reason.empty() ? "" : "; ") + noInfo;

    // A value that rests on a weak seam (EntryPatched, or itself single-anchor) is never plain Found: the weakness propagates. Any unique
    // anchor that depends on one is enough (conservative: the cost of the cap is a WARN, the usable status stays).
    std::string weakNotes;
    for (const AnchorResult* u : unique)
    {
        if (u->weakDependency)
            weakNotes += (weakNotes.empty() ? "" : "; ") + std::string("anchor '") + u->name + "' " + u->note;
    }
    if (!weakNotes.empty())
    {
        seam.status = SeamStatus::FoundSingleAnchor;
        seam.viaWeakDependency = true;
        seam.reason += (seam.reason.empty() ? "" : "; ") + std::string("the value rests on a weak seam: ") + weakNotes;
    }

    if (kind == SeamKind::Function)
    {
        std::uint8_t entry[6] = {};
        std::size_t got = 0;
        for (std::size_t n : { std::size_t(6), std::size_t(2), std::size_t(1) })
        {
            if (image.Read(value, entry, n))
            {
                got = n;
                break;
            }
        }
        if (patched || (got != 0 && EntryIsJump(entry, got)))
        {
            seam.status = SeamStatus::EntryPatched;
            seam.reason = "the function entry at " + Hex(value) + " already starts with a jump (a foreign hook): not hooked";
        }
    }
}

// The known-build table is only ever used for the build it describes: the stamp AND the size of the image must equal the table's.
inline bool TableIsThisBuild(const ResolvedBuild& rb, const GameBuild& table)
{
    return table.timeDateStamp == rb.timeDateStamp && table.sizeOfImage == rb.sizeOfImage;
}

// The value a binding writes into its field: the seam value plus the binding's valueOffset. False when that leaves the image (an Rva
// field) or the 32 bit range.
inline bool BoundValue(const SeamResult& seam, const Binding& b, const FieldRow& row, std::uint32_t sizeOfImage, std::uint32_t& out)
{
    const std::int64_t v = static_cast<std::int64_t>(seam.rva) + b.valueOffset;
    if (v < 0 || v > 0xffffffffll || (row.kind == FieldKind::Rva && v >= static_cast<std::int64_t>(sizeOfImage)))
        return false;
    out = static_cast<std::uint32_t>(v);
    return true;
}
} // namespace detail

// The known-build cross-check. Only when the table is THIS build (stamp and size equal rb's; otherwise nothing happens and false is
// returned, so a table of another build never writes into a resolved build):
//   * a usable seam: every bound value is compared with the table; on a disagreement the TABLE value replaces the resolved one in
//     `rb.build` ("table wins"), the seam is marked Disagree and its reason says which field;
//   * a seam that is NOT usable (NotFound, Ambiguous, Conflict): every field bound to it is filled from the table, because
//     the table was verified offline against this exact executable. The seam keeps its honest status, `fellBackToTable` is set and its
//     reason says so. This runs after every seam resolved: a table value never feeds the resolution of another seam.
//   * a seam with SeamSpec::failClosed (a strict SHAPE seam whose Guard field is the hook's only strict check) is NOT filled either when it is
//     not usable: its fields stay zero and `UsableOrTable` is false.
//   * an EntryPatched seam (a foreign hook, e.g. Dalamud, over the entry) is NOT filled: the byte guards refused to hook it before, so
//     the resolver fails closed the same way. Its fields stay zero and `UsableOrTable` is false.
// ResolveImage runs it itself when ResolveOptions::knownTable is set. Returns whether the table was applied (rb.knownBuild).
inline bool CrossCheck(ResolvedBuild& rb, const GameBuild& table, const Registry& registry)
{
    if (!detail::TableIsThisBuild(rb, table))
        return false;
    rb.knownBuild = true;
    for (SeamResult& seam : rb.seams)
    {
        bool failClosed = false; // SeamSpec::failClosed of this seam
        for (const SeamSpec& spec : registry.seams)
        {
            if (spec.id == seam.id)
            {
                failClosed = spec.failClosed;
                break;
            }
        }
        bool anyBinding = false, disagree = false, filled = false;
        for (const Binding& b : registry.bindings)
        {
            if (seam.id != b.seam)
                continue;
            const FieldRow* row = FindField(b.key);
            if (row == nullptr)
                continue;
            anyBinding = true;
            if (row->kind != FieldKind::Guard && !seam.hasTable)
            {
                // the table's view of the SEAM value (the field minus the binding's offset), so that "rva=X table=X" reads as agreement
                seam.hasTable = true;
                seam.tableValue = *FieldU32(table, *row) - static_cast<std::uint32_t>(b.valueOffset);
            }
            if (seam.status == SeamStatus::EntryPatched)
                continue; // a foreign hook sits on the entry: nothing is filled, a feature never hooks it (fail closed, as the byte guards did)
            if (failClosed && !IsUsable(seam.status))
                continue; // SeamSpec::failClosed: a strict shape that did not match is never replaced by the table's (shorter) guard
            if (!IsUsable(seam.status))
            {
                // known build, seam not usable: the table value stands in (never for an unknown build)
                if (row->kind == FieldKind::Guard)
                    std::memcpy(FieldBytes(rb.build, *row), FieldBytes(table, *row), row->size);
                else
                    *FieldU32(rb.build, *row) = *FieldU32(table, *row);
                filled = true;
                continue;
            }
            if (row->kind == FieldKind::Guard)
            {
                if (std::memcmp(FieldBytes(rb.build, *row), FieldBytes(table, *row), row->size) != 0)
                {
                    disagree = true;
                    seam.reason += (seam.reason.empty() ? "" : "; ") + b.key + ": the resolved bytes differ from the table (table wins)";
                    std::memcpy(FieldBytes(rb.build, *row), FieldBytes(table, *row), row->size);
                }
            }
            else
            {
                const std::uint32_t tv = *FieldU32(table, *row);
                std::uint32_t* field = FieldU32(rb.build, *row);
                if (*field != tv)
                {
                    disagree = true;
                    seam.reason += (seam.reason.empty() ? "" : "; ") + b.key + ": resolved " + detail::Hex(*field) + ", table " + detail::Hex(tv) +
                                   " (table wins)";
                    *field = tv;
                }
            }
        }
        if (anyBinding && IsUsable(seam.status))
            seam.agreement = disagree ? Agreement::Disagree : Agreement::Agree;
        if (filled)
        {
            seam.fellBackToTable = true;
            seam.reason += (seam.reason.empty() ? "" : "; ") + std::string("known build: the bound fields are filled from the table (the seam did not resolve)");
        }
    }
    return true;
}

inline ResolvedBuild ResolveImage(const ImageView& image, const Registry& registry, const ResolveOptions& options = {})
{
    using namespace detail;
    const auto t0 = Clock::now();

    ResolvedBuild rb; // build starts zeroed (ResolvedBuild::build = ZeroedBuild())
    rb.timeDateStamp = image.TimeDateStamp();
    rb.sizeOfImage = image.SizeOfImage();
    rb.build.timeDateStamp = rb.timeDateStamp;
    rb.build.sizeOfImage = rb.sizeOfImage;
    // A known-build table counts only for the build it describes: stamp AND size must equal the image's. Otherwise it is ignored (one
    // WARN below): no cross-check, no table value anywhere, knownBuild = false.
    rb.knownBuild = options.knownTable != nullptr && TableIsThisBuild(rb, *options.knownTable);
    const bool tableIgnored = options.knownTable != nullptr && !rb.knownBuild;
    rb.registryErrors = ValidateRegistry(registry);

    const std::size_t n = registry.seams.size();
    rb.seams.resize(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        rb.seams[i].id = registry.seams[i].id;
        rb.seams[i].anchors.resize(registry.seams[i].anchors.size());
        for (std::size_t a = 0; a < registry.seams[i].anchors.size(); ++a)
            rb.seams[i].anchors[a].name = registry.seams[i].anchors[a].name;
    }

    auto emit = [&options](LogLevel level, const std::string& line)
    {
        if (!options.sink)
            return;
        try
        {
            options.sink(level, line);
        }
        catch (...)
        {
        }
    };

    if (!rb.registryErrors.empty() || !image.Valid())
    {
        for (SeamResult& s : rb.seams)
            s.reason = rb.registryErrors.empty() ? "the image headers are not valid" : "the registry is invalid: nothing was resolved";
        if (rb.registryErrors.empty())
            rb.registryErrors.push_back("the image headers are not valid");
    }
    else
    {
        // 1. every pattern, one pass over the image
        std::vector<std::vector<Pattern>> patterns(n);
        std::vector<ScanRequest> requests;
        std::vector<std::vector<int>> requestOf(n); // -1: not scanned
        for (std::size_t i = 0; i < n; ++i)
        {
            patterns[i].reserve(registry.seams[i].anchors.size()); // the requests point into these vectors: they must not move
            for (const AnchorSpec& a : registry.seams[i].anchors)
            {
                Pattern p;
                ParsePattern(a.pattern, p);
                patterns[i].push_back(std::move(p));
                if (a.fromSeam.empty())
                {
                    // the same pattern over the same sections (one pattern that marks two sites) is scanned once
                    int shared = -1;
                    for (std::size_t r = 0; r < requests.size() && shared < 0; ++r)
                    {
                        if (requests[r].searchIn == a.searchIn && requests[r].pattern->bytes == patterns[i].back().bytes &&
                            requests[r].pattern->mask == patterns[i].back().mask)
                            shared = static_cast<int>(r);
                    }
                    if (shared < 0)
                    {
                        shared = static_cast<int>(requests.size());
                        requests.push_back({ &patterns[i].back(), a.searchIn });
                    }
                    requestOf[i].push_back(shared);
                }
                else
                    requestOf[i].push_back(-1);
            }
        }
        rb.scanRequests = static_cast<std::uint32_t>(requests.size());
        const auto scanStart = Clock::now();
        const ScanOutcome scan = ScanBatch(image, requests);
        std::string scanError = scan.unreadable ? "the image could not be scanned: " + scan.error : std::string();

        // 2. the relaxed entry patterns, only for entry anchors whose full pattern has no raw match
        std::vector<Pattern> relaxedPatterns;
        std::vector<ScanRequest> relaxedRequests;
        std::vector<std::vector<int>> relaxedOf(n);
        if (!scan.unreadable)
        {
            for (std::size_t i = 0; i < n; ++i)
            {
                for (std::size_t a = 0; a < registry.seams[i].anchors.size(); ++a)
                {
                    relaxedOf[i].push_back(-1);
                    const AnchorSpec& spec = registry.seams[i].anchors[a];
                    if (!spec.entryAnchor || requestOf[i][a] < 0 || !scan.perRequest[requestOf[i][a]].rvas.empty())
                        continue;
                    Pattern rp = RelaxEntry(patterns[i][a]);
                    if (rp.fixedBytes() < kMinRelaxedFixedBytes)
                        continue;
                    relaxedOf[i][a] = static_cast<int>(relaxedPatterns.size());
                    relaxedPatterns.push_back(std::move(rp));
                }
            }
            for (std::size_t i = 0; i < n; ++i)
            {
                for (std::size_t a = 0; a < registry.seams[i].anchors.size(); ++a)
                {
                    if (relaxedOf[i][a] >= 0)
                        relaxedRequests.push_back({ nullptr, registry.seams[i].anchors[a].searchIn });
                }
            }
            for (std::size_t k = 0; k < relaxedRequests.size(); ++k)
                relaxedRequests[k].pattern = &relaxedPatterns[k];
        }
        const ScanOutcome relaxedScan = relaxedRequests.empty() ? ScanOutcome {} : ScanBatch(image, relaxedRequests);
        rb.scanMicros = MicrosSince(scanStart);
        rb.scanFailed = scan.unreadable || relaxedScan.unreadable;
        if (scan.unreadable)
            rb.scanError = scan.error;
        else if (relaxedScan.unreadable)
            rb.scanError = relaxedScan.error;

        // 3. seams in dependency order (the registry is acyclic: ValidateRegistry checked)
        const std::vector<std::vector<std::size_t>> deps = SeamDeps(registry);
        std::vector<SeamState> states(n);
        std::vector<bool> done(n, false);
        std::vector<std::vector<std::uint8_t>> guards(registry.bindings.size());
        const EvalContext ctx { image, registry.seams, states };

        for (std::size_t round = 0; round < n; ++round)
        {
            std::size_t pick = n;
            for (std::size_t i = 0; i < n && pick == n; ++i)
            {
                if (done[i])
                    continue;
                bool ready = true;
                for (std::size_t d : deps[i])
                    ready = ready && done[d];
                if (ready)
                    pick = i;
            }
            if (pick == n)
                break; // unreachable for an acyclic registry
            const std::size_t i = pick;
            done[i] = true;

            const auto seamStart = Clock::now();
            SeamResult& seam = rb.seams[i];
            const SeamSpec& spec = registry.seams[i];
            std::vector<std::vector<std::uint32_t>> allValues(spec.anchors.size());
            for (std::size_t a = 0; a < spec.anchors.size(); ++a)
            {
                const ScanHits* hits = requestOf[i][a] >= 0 && !scan.unreadable ? &scan.perRequest[requestOf[i][a]] : nullptr;
                const bool hasRelaxed = !relaxedOf.empty() && !relaxedOf[i].empty() && relaxedOf[i][a] >= 0;
                const ScanHits* relaxed = hasRelaxed && !relaxedScan.unreadable ? &relaxedScan.perRequest[relaxedOf[i][a]] : nullptr;
                seam.anchors[a] = EvaluateAnchor(ctx, spec.anchors[a], patterns[i][a], hits, requestOf[i][a] >= 0 ? scanError : std::string(), relaxed,
                                                 hasRelaxed && relaxedScan.unreadable, allValues[a]);
            }
            Combine(seam, spec.kind, image, allValues);

            // a seam that loses its value after Combine (a binding that cannot be satisfied, a clash inside a mustDiffer group) is
            // downgraded here, before anything depends on it
            auto drop = [&seam](SeamStatus status, std::string reason)
            {
                seam.status = status;
                seam.hasValue = false;
                seam.rva = 0;
                seam.reason = std::move(reason);
            };

            // guard bytes of this seam's bindings are read now (and the value + valueOffset of the others checked), so that a failure
            // downgrades the seam before anything depends on it
            if (IsUsable(seam.status))
            {
                for (std::size_t b = 0; b < registry.bindings.size(); ++b)
                {
                    const Binding& bind = registry.bindings[b];
                    const FieldRow* row = FindField(bind.key);
                    if (row == nullptr || seam.id != bind.seam)
                        continue;
                    if (row->kind != FieldKind::Guard)
                    {
                        std::uint32_t bound = 0;
                        if (!BoundValue(seam, bind, *row, rb.sizeOfImage, bound))
                        {
                            drop(SeamStatus::NotFound, "the value of '" + bind.key + "' (seam value " + Hex(seam.rva) + " + valueOffset) leaves the image");
                            break;
                        }
                        continue;
                    }
                    guards[b].assign(row->size, 0);
                    const std::int64_t at = static_cast<std::int64_t>(seam.rva) + bind.guardOffset;
                    if (at < 0 || at > 0xffffffffll || !image.Read(static_cast<std::uint32_t>(at), guards[b].data(), guards[b].size()))
                    {
                        drop(SeamStatus::NotFound, "the guard bytes of '" + bind.key + "' could not be read at " + Hex(static_cast<std::uint32_t>(at)));
                        break;
                    }
                }
            }

            // mustDiffer: the later member of a group that landed on the value of an earlier member is the one in doubt
            if (seam.hasValue)
            {
                for (const std::vector<std::string>& group : registry.mustDiffer)
                {
                    const auto self = std::find(group.begin(), group.end(), seam.id);
                    if (self == group.end())
                        continue;
                    bool clash = false;
                    for (auto other = group.begin(); other != self && !clash; ++other)
                    {
                        const std::size_t j = SeamIndex(registry.seams, *other);
                        if (j != static_cast<std::size_t>(-1) && rb.seams[j].hasValue && rb.seams[j].rva == seam.rva)
                        {
                            drop(SeamStatus::Conflict, "resolved to " + Hex(seam.rva) + ", the same value as '" + *other + "' (these seams must differ)");
                            clash = true;
                        }
                    }
                    if (clash)
                        break;
                }
            }
            states[i].hasValue = seam.hasValue;
            states[i].value = seam.rva;
            states[i].weak = seam.status == SeamStatus::EntryPatched || seam.status == SeamStatus::FoundSingleAnchor;
            seam.micros = MicrosSince(seamStart);
        }

        // 4. fill the build from the usable seams
        for (std::size_t b = 0; b < registry.bindings.size(); ++b)
        {
            const Binding& bind = registry.bindings[b];
            const FieldRow* row = FindField(bind.key);
            const SeamResult* seam = rb.Find(bind.seam);
            if (row == nullptr || seam == nullptr || !IsUsable(seam->status))
                continue;
            if (row->kind == FieldKind::Guard)
            {
                if (guards[b].size() == row->size)
                    std::memcpy(FieldBytes(rb.build, *row), guards[b].data(), row->size);
            }
            else
            {
                std::uint32_t bound = 0;
                if (BoundValue(*seam, bind, *row, rb.sizeOfImage, bound))
                    *FieldU32(rb.build, *row) = bound;
            }
        }
        // the known-build table, after every seam resolved: a table value never feeds the resolution of another seam
        if (rb.knownBuild)
            CrossCheck(rb, *options.knownTable, registry);
    }

    // 5. one line per seam (registry order), then the total
    for (const std::string& e : rb.registryErrors)
        emit(LogLevel::Error, "[ffxiv-resolve] registry error: " + e);
    if (tableIgnored)
        emit(LogLevel::Warn, "[ffxiv-resolve] known table ignored: it describes another build (table stamp " + HexValue(options.knownTable->timeDateStamp) +
                                 " size " + HexValue(options.knownTable->sizeOfImage) + ", image stamp " + HexValue(rb.timeDateStamp) + " size " +
                                 HexValue(rb.sizeOfImage) + ")");
    for (const SeamResult& s : rb.seams)
        emit(SeamLogLevel(s), FormatSeamLine(s));
    rb.totalMicros = MicrosSince(t0);
    emit(rb.registryErrors.empty() ? LogLevel::Info : LogLevel::Warn, FormatTotalLine(rb));
    return rb;
}
} // namespace ext::ffxiv::resolve
