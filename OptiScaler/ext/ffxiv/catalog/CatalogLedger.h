#pragma once
// Shader catalog health accounting.
//
// The FFXIV fork identifies shaders by the CRC-32 of their bytecode (CatalogTable.h). A game patch that recompiles a shader changes its CRC
// and the feature that depends on it silently goes off. The ledger makes that visible: for every catalog it keeps a bitmask of the
// entries that were created this session and, per entry, when (milliseconds since the hooks were installed), at which Present and as which
// creation it was first seen, plus its structural fingerprint (ShaderFingerprint.h). A summary line says how many of each catalog exist
// ("UI 6/7 scene 5/5 ..."), a per-CRC line says when each one appeared, and for catalogs that are short the log names the shaders of the
// session that belong to no catalog (near-miss candidates). Nothing here changes a CRC, tags a shader or decides anything by fingerprint:
// logging only. The one consumer that acts on it is the partial-UI-catalog guard: UiHealthy() is read on every frame by
// UiBoundaryProvider::Take() (FfxivUiBoundary.cpp).
//
// Where it is fed (FfxivCatalog.h): the creation hook (every DXBC pixel shader, armed or not: the same always-run place as the boundary
// stamp, NOT the D1 registry that only runs while armed), and the nameplate vertex shader tagging in CreateInputLayout. Creation runs on
// loader threads inside the game's CreatePixelShader call, so the Note* functions are lock free, allocation free and log free: atomics, fixed
// arrays, one fingerprint per catalog entry the first time it is seen and at most kUncatalogedCap fingerprints for shaders outside every
// catalog (the fingerprint is O(chunks + signature elements), well below the CRC the hook has already computed for the same bytes).
// Everything that builds text (Poll) runs on the Present thread, and only when something is due.
//
// Pure and dependency free (no pch, no D3D, no logging) so tests/ext/ffxiv/catalog builds it standalone.

#include "CatalogTable.h"
#include "ShaderFingerprint.h"

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace ext::ffxiv::catalog
{
// First summary after the session has settled: this many Presents AND this many milliseconds since the hooks were installed (P0-A: the UI
// set exists by the first Present, +9.5 s after the hook; the character-preview finisher shader appears when the lobby starts, +16..20 s).
inline constexpr uint64_t kSettlePresents = 600;
inline constexpr uint32_t kSettleMs = 30000;

// Shaders outside every catalog that get a fingerprint and a record in the SAMPLE (distinct CRCs, in creation order). HARD cap: the 257th
// distinct shader of a session is neither fingerprinted nor recorded by the sample. A session creates tens of thousands of shader objects
// (P0-A: about 32,600), so this is a sample of the first 256 distinct ones, which are probably init-time shaders. It only feeds the
// below-baseline dump; the near-miss CANDIDATES do not depend on it (see kCandidateCap).
inline constexpr size_t kUncatalogedCap = 256;
// Lines of that sample written to the log per session (only when a catalog is below its established baseline).
inline constexpr size_t kUncatalogedLogCap = 32;
// Near-miss candidates kept per session, and therefore candidate lines logged (each at most once). Once ANY pixel-shader catalog entry has a
// reference fingerprint (refFingerprint != 0), EVERY uncatalogued shader is fingerprinted at creation and compared with the references
// (at most kLedgerMaxSlots of them); only the matches are kept, here: first come, fill once, never overwritten (a reader never sees a torn
// record), distinct CRCs only. While every reference is 0 nothing of this runs and the sample behaves exactly as before.
inline constexpr size_t kCandidateCap = 16;

// Capacity of the per-entry record array: one Slot per catalog entry. A catalog mask is one 32-bit word, so a catalog holds at most 32 entries.
inline constexpr size_t kLedgerMaxSlots = 48;
inline constexpr size_t kLedgerMaxEntriesPerCatalog = 32;

// Do these tables fit the ledger: no catalog above 32 entries, all of them together within kLedgerMaxSlots. A table that does not fit would make
// the creation hook index past the slot array (and shift a 32-bit mask by 32 or more), on the game's own thread.
constexpr bool LedgerFits(const CatalogInfo* tables) noexcept
{
    size_t total = 0;
    for (size_t c = 0; c < kCatalogCount; ++c)
    {
        if (tables[c].count > kLedgerMaxEntriesPerCatalog)
            return false;
        total += tables[c].count;
    }
    return total <= kLedgerMaxSlots;
}
// The real tables (37 entries today) fit. Adding catalog entries past the capacity fails the build here, not at run time in the game.
static_assert(LedgerFits(kInfo), "the catalog tables no longer fit the ledger's slot array: raise kLedgerMaxSlots");

// One record per catalog entry, written once by the thread that first sees the entry. Readers look at it only after `ready` (acquire).
struct Slot
{
    std::atomic<uint32_t> ready { 0 };
    uint32_t ms = 0;      // milliseconds since the hooks were installed
    uint64_t present = 0; // Presents so far
    uint32_t order = 0;   // 1-based creation index among all pixel shaders the hook saw
    ShaderFingerprint fp; // of the bytes of the first creation
};

// A shader outside every catalog, kept compactly (the full fingerprint is 2 KB, 256 of them would be 0.5 MB).
struct Uncatalogued
{
    std::atomic<uint32_t> ready { 0 };
    uint32_t crc = 0, size = 0, order = 0, ms = 0;
    uint64_t present = 0;
    uint64_t fpHash = 0; // 0 when the bytes were not a fingerprintable DXBC
    FpError error = FpError::None;
    uint32_t tokens = 0;
    uint16_t chunks = 0;
    uint8_t programType = 0, major = 0, minor = 0;
    uint8_t inputs = 0, outputs = 0;
    bool valid = false;
    bool hasProgram = false;
};

// An uncatalogued shader whose fingerprint equals the reference fingerprint of a catalog entry (a near-miss candidate for that entry).
struct Candidate
{
    std::atomic<uint32_t> ready { 0 };
    uint32_t crc = 0, size = 0, order = 0, ms = 0;
    uint64_t present = 0;
    uint64_t fpHash = 0; // non-zero: equal to a reference
};

class Ledger
{
  public:
    // Tables that do not fit (LedgerFits: only possible with a table built by a test, the real ones are checked at compile time) make the
    // ledger inert: nothing is counted, recorded or fingerprinted, so nothing is ever written outside the slot array. UiHealthy() then answers
    // true (fail open: the guard only ever removes a feature, a broken ledger must not).
    explicit Ledger(const CatalogInfo* tables = kInfo) : _tables(tables), _fits(LedgerFits(tables))
    {
        size_t base = 0;
        for (size_t c = 0; c < kCatalogCount; ++c)
        {
            _missingFor[c] = 0xFFFFFFFFu;
            _slotBase[c] = base;
            if (_fits)
                base += tables[c].count;
        }
        _slotBase[kCatalogCount] = base;
        // The references the near-miss search compares uncatalogued PIXEL shaders with (a vertex shader catalog, the nameplate one, cannot match a
        // pixel shader). At most kLedgerMaxSlots (the tables fit).
        for (size_t c = 0; _fits && c < kCatalogCount; ++c)
            for (size_t i = 0; !tables[c].vertexShaders && i < tables[c].count; ++i)
                if (tables[c].entries[i].refFingerprint != 0)
                {
                    _refHash[_refN] = tables[c].entries[i].refFingerprint;
                    _refCatalog[_refN] = static_cast<uint8_t>(c);
                    _refIndex[_refN] = static_cast<uint8_t>(i);
                    ++_refN;
                }
    }
    bool Fits() const noexcept { return _fits; }

    // ---- feeding (any thread; lock free, allocation free) --------------------------------------------------------------------------
    // A DXBC pixel shader was created: `crc` is the CRC-32 of the `size` bytes at `code` (computed once by the hook), `nowMs` the time since
    // the hooks were installed. Sets the bit of every catalog the shader belongs to (a CRC can be in two: ToneMapping is a lighting entry and
    // scene stage 1; companion is inside UI). A shader in no catalog goes to the uncatalogued sample.
    void NotePixelShader(const void* code, size_t size, uint32_t crc, uint32_t nowMs) noexcept
    {
        if (!_fits)
            return;
        const uint32_t order = _created.fetch_add(1, std::memory_order_relaxed) + 1;
        bool hit = false;
        const ShaderFingerprint* computed = nullptr; // the fingerprint of these bytes, once a slot holds it (a CRC in two catalogs: computed once)
        for (size_t c = 0; c < kCatalogCount; ++c)
        {
            const CatalogInfo& info = _tables[c];
            if (info.vertexShaders)
                continue;
            for (size_t i = 0; i < info.count; ++i)
            {
                const Entry& e = info.entries[i];
                if (e.crc != crc || (e.bytes != 0 && e.bytes != size))
                    continue;
                hit = true;
                First(c, i, code, size, order, nowMs, computed);
            }
        }
        if (hit)
            return;
        if (_refN == 0)
        {
            Sample(code, size, crc, order, nowMs, nullptr); // no reference exists: only the first kUncatalogedCap distinct shaders are fingerprinted
            return;
        }
        // A reference exists: EVERY uncatalogued shader is fingerprinted here (about 0.7 microseconds, no allocation, no lock, no log) and compared
        // with the references. Only matches are kept. The sample (for the below-baseline dump) reuses this fingerprint.
        const ShaderFingerprint fp = Fingerprint(code, size);
        _fingerprints.fetch_add(1, std::memory_order_relaxed);
        NoteReferenceMatch(fp, crc, size, order, nowMs);
        Sample(code, size, crc, order, nowMs, &fp);
    }

    // A vertex shader whose bytecode was handed to CreateInputLayout: only the nameplate catalog is made of those.
    void NoteVertexShader(const void* code, size_t size, uint32_t crc, uint32_t nowMs) noexcept
    {
        if (!_fits)
            return;
        const ShaderFingerprint* computed = nullptr;
        for (size_t c = 0; c < kCatalogCount; ++c)
        {
            const CatalogInfo& info = _tables[c];
            if (!info.vertexShaders)
                continue;
            for (size_t i = 0; i < info.count; ++i)
                if (info.entries[i].crc == crc)
                    First(c, i, code, size, _vertex.fetch_add(1, std::memory_order_relaxed) + 1, nowMs, computed);
        }
    }

    // A CreateInputLayout call was seen at all: without it the nameplate catalog was not measured (0/3 would be a false alarm).
    void NoteLayoutSeen() noexcept { _layouts.fetch_add(1, std::memory_order_relaxed); }

    // One Present (bridge Present entry). Returns the new count.
    uint64_t NotePresent() noexcept { return _present.fetch_add(1, std::memory_order_relaxed) + 1; }

    // ---- reading -------------------------------------------------------------------------------------------------------------------
    uint32_t Mask(Catalog c) const noexcept { return _mask[static_cast<size_t>(c)].load(std::memory_order_acquire); }
    unsigned Count(Catalog c) const noexcept { return static_cast<unsigned>(std::popcount(Mask(c))); }
    uint32_t UiMask() const noexcept { return Mask(Catalog::Ui); }
    // The partial-UI-catalog guard, evaluated on every call (never latched).
    bool UiHealthy() const noexcept { return !_fits || UiCatalogHealthy(UiMask()); }
    uint64_t Present() const noexcept { return _present.load(std::memory_order_relaxed); }
    uint32_t PixelShadersSeen() const noexcept { return _created.load(std::memory_order_relaxed); }
    uint32_t LayoutsSeen() const noexcept { return _layouts.load(std::memory_order_relaxed); }
    uint32_t FingerprintsComputed() const noexcept { return _fingerprints.load(std::memory_order_relaxed); }
    uint32_t UncatalogedRecorded() const noexcept
    {
        const uint32_t n = _uncatClaimed.load(std::memory_order_relaxed);
        return n < kUncatalogedCap ? n : static_cast<uint32_t>(kUncatalogedCap);
    }
    // References compared at creation (0: no catalog entry has a reference fingerprint, the near-miss search is off).
    size_t ReferenceCount() const noexcept { return _refN; }
    // Near-miss candidates kept (at most kCandidateCap), and matches dropped because that was full.
    uint32_t CandidatesKept() const noexcept
    {
        uint32_t n = 0;
        for (size_t k = 0; k < kCandidateCap; ++k)
            n += _cand[k].ready.load(std::memory_order_acquire) == 1 ? 1u : 0u;
        return n;
    }
    uint32_t CandidatesDropped() const noexcept { return _candDropped.load(std::memory_order_relaxed); }
    const Candidate& CandidateAt(size_t k) const noexcept { return _cand[k]; }
    // Valid once the catalog's Mask() has the entry's bit.
    const Slot& SlotAt(Catalog c, size_t i) const noexcept { return _slots[_slotBase[static_cast<size_t>(c)] + i]; }
    const Uncatalogued& UncatalogedAt(size_t i) const noexcept { return _uncat[i]; }

    // ---- log (Present thread) ------------------------------------------------------------------------------------------------------
    // The lines that are due now (empty almost always). Nothing before the session has settled (kSettlePresents Presents and kSettleMs);
    // then one summary line plus one first-seen and one fingerprint line per newly seen CRC, the missing entries and the near-miss output;
    // after that the same again whenever any catalog's count changed, and never for a CRC that was already logged. Not re-entrant: a
    // concurrent call returns nothing.
    std::vector<std::string> Poll(uint32_t nowMs)
    {
        std::vector<std::string> lines;
        if (_polling.test_and_set(std::memory_order_acquire))
            return lines;
        struct Release
        {
            std::atomic_flag& f;
            ~Release() { f.clear(std::memory_order_release); }
        } release { _polling };

        if (!_fits)
        {
            if (!_inertReported)
            {
                _inertReported = true;
                lines.push_back("[ffxiv-catalog] disabled: the catalog tables do not fit the ledger (more than " + std::to_string(kLedgerMaxEntriesPerCatalog) + " entries in a catalog or more than " +
                                std::to_string(kLedgerMaxSlots) + " in all); nothing is counted");
            }
            return lines;
        }
        const uint64_t present = _present.load(std::memory_order_relaxed);
        uint32_t mask[kCatalogCount];
        bool changed = false;
        for (size_t c = 0; c < kCatalogCount; ++c)
        {
            mask[c] = _mask[c].load(std::memory_order_acquire);
            changed |= mask[c] != _loggedMask[c];
        }
        const uint32_t candidates = _candReady.load(std::memory_order_acquire);
        changed |= candidates != _candSeen; // a near-miss candidate showed up (possibly with no catalog count changing)
        if (!_settled)
        {
            if (present < kSettlePresents || nowMs < kSettleMs)
                return lines;
            _settled = true;
        }
        else if (!changed)
            return lines;

        lines.push_back(SummaryLine(mask, nowMs, present));

        // first seen + fingerprint, for the entries that are new since the last time
        for (size_t c = 0; c < kCatalogCount; ++c)
        {
            const CatalogInfo& info = _tables[c];
            for (size_t i = 0; i < info.count; ++i)
            {
                const uint32_t bit = 1u << i;
                if (!(mask[c] & bit) || (_loggedMask[c] & bit))
                    continue;
                const Slot& s = _slots[_slotBase[c] + i];
                lines.push_back(Fmt("[ffxiv-catalog] first seen crc=0x%08x name=%s:%s t=%ums present=%llu order=%u", info.entries[i].crc, info.name,
                                    info.entries[i].name, s.ms, static_cast<unsigned long long>(s.present), s.order));
            }
        }
        for (size_t c = 0; c < kCatalogCount; ++c)
        {
            const CatalogInfo& info = _tables[c];
            for (size_t i = 0; i < info.count; ++i)
            {
                const uint32_t bit = 1u << i;
                if (!(mask[c] & bit) || (_loggedMask[c] & bit))
                    continue;
                const uint32_t crc = info.entries[i].crc;
                if (FingerprintLogged(crc))
                    continue;
                if (_fpLoggedN < kFpLoggedCap)
                    _fpLogged[_fpLoggedN++] = crc;
                lines.push_back(Fmt("[ffxiv-catalog] fingerprint crc=0x%08x name=%s:%s ", crc, info.name, info.entries[i].name) +
                                FormatFingerprint(_slots[_slotBase[c] + i].fp));
            }
        }

        // missing entries (catalogs whose set changed since their missing list was last written, or that never had one written)
        for (size_t c = 0; c < kCatalogCount; ++c)
        {
            const CatalogInfo& info = _tables[c];
            if (info.vertexShaders && _layouts.load(std::memory_order_relaxed) == 0)
                continue; // not measured: no CreateInputLayout was ever seen
            if (_missingFor[c] == mask[c])
                continue;
            _missingFor[c] = mask[c];
            const unsigned have = static_cast<unsigned>(std::popcount(mask[c]));
            for (size_t i = 0; i < info.count; ++i)
            {
                if (mask[c] & (1u << i))
                    continue;
                const Entry& e = info.entries[i];
                std::string line = Fmt("[ffxiv-catalog] %s #%u (%s) crc=0x%08x missing", info.name, static_cast<unsigned>(i), e.name, e.crc);
                // Health is the pinned required set, not the count: a session that created ui5 but not ui3 holds 6 of 7
                // yet is in FALLBACK, so its missing-ui3 line must not claim the healthy baseline.
                const bool catalogHealthy = c == static_cast<size_t>(Catalog::Ui) && kUiRequiredMask != 0
                    ? UiCatalogHealthy(mask[c])
                    : (info.healthyMin != 0 && have >= info.healthyMin);
                if (catalogHealthy)
                    line += Fmt(" (within the healthy baseline of %u)", info.healthyMin);
                if (e.refFingerprint == 0)
                    line += "; no reference fingerprint, no candidate can be named";
                else if (info.vertexShaders)
                    line += Fmt("; reference fp=0x%016llx (vertex shader catalog: not searched)", static_cast<unsigned long long>(e.refFingerprint));
                else if (CandidateCount(e.refFingerprint) == 0)
                    line += Fmt("; reference fp=0x%016llx matches no uncatalogued shader created so far", static_cast<unsigned long long>(e.refFingerprint));
                else
                    line += Fmt("; reference fp=0x%016llx, candidates follow", static_cast<unsigned long long>(e.refFingerprint));
                lines.push_back(std::move(line));
            }
        }

        // near-miss candidates: an uncatalogued shader whose fingerprint EQUALS the reference of a missing entry (kept at creation, whenever it
        // was created). Named once each.
        for (size_t c = 0; c < kCatalogCount; ++c)
        {
            const CatalogInfo& info = _tables[c];
            if (info.vertexShaders)
                continue; // only pixel shaders are compared
            for (size_t i = 0; i < info.count; ++i)
            {
                if ((mask[c] & (1u << i)) || info.entries[i].refFingerprint == 0)
                    continue;
                for (uint32_t k = 0; k < kCandidateCap; ++k)
                {
                    const Candidate& u = _cand[k];
                    if (u.ready.load(std::memory_order_acquire) != 1 || u.fpHash != info.entries[i].refFingerprint || CandidateLogged(k))
                        continue;
                    MarkCandidate(k);
                    lines.push_back(Fmt("[ffxiv-catalog] %s #%u (%s) missing; candidate crc=0x%08x same fingerprint fp=0x%016llx (created #%u, t=%ums, "
                                        "present=%llu)",
                                        info.name, static_cast<unsigned>(i), info.entries[i].name, u.crc, static_cast<unsigned long long>(u.fpHash), u.order,
                                        u.ms, static_cast<unsigned long long>(u.present)));
                }
            }
        }

        // the sample of everything that is in no catalog: only when a catalog is below the baseline a healthy build reaches
        std::string below;
        for (size_t c = 0; c < kCatalogCount; ++c)
        {
            const CatalogInfo& info = _tables[c];
            const unsigned have = static_cast<unsigned>(std::popcount(mask[c]));
            // The dump runs while a catalog is below its baseline; for the UI catalog that is the pinned required set,
            // not the count, so a session holding ui5 instead of ui3 still dumps.
            const bool belowBaseline = c == static_cast<size_t>(Catalog::Ui) && kUiRequiredMask != 0
                ? !UiCatalogHealthy(mask[c])
                : (info.healthyMin != 0 && have < info.healthyMin);
            if (!info.vertexShaders && belowBaseline)
            {
                // The UI catalog is judged by its pinned set, not a count: say so, so a 6/7 session missing a
                // required shader does not read as "below 6".
                if (c == static_cast<size_t>(Catalog::Ui) && kUiRequiredMask != 0)
                    below += Fmt("%s%s %u/%u, required set of %u incomplete", below.empty() ? "" : ", ", info.name, have,
                                 static_cast<unsigned>(info.count), static_cast<unsigned>(std::popcount(kUiRequiredMask)));
                else
                    below += Fmt("%s%s %u/%u below the healthy baseline of %u", below.empty() ? "" : ", ", info.name, have,
                                 static_cast<unsigned>(info.count), info.healthyMin);
            }
        }
        if (!below.empty())
        {
            const uint32_t n = UncatalogedRecorded();
            bool header = false;
            for (uint32_t k = static_cast<uint32_t>(_dumpedUpTo); k < n && _dumped < kUncatalogedLogCap; ++k)
            {
                const Uncatalogued& u = _uncat[k];
                if (u.ready.load(std::memory_order_acquire) != 1)
                    break; // still being written: next time
                if (!header)
                {
                    lines.push_back("[ffxiv-catalog] near-miss: " + below + "; shaders created this session that belong to no catalog, in creation order "
                                    "(sample: the first " + std::to_string(kUncatalogedCap) + " distinct CRCs, log: at most " +
                                    std::to_string(kUncatalogedLogCap) + " lines per session)");
                    header = true;
                }
                lines.push_back(Fmt("[ffxiv-catalog] uncatalogued #%u crc=0x%08x ", u.order, u.crc) + UncatText(u) +
                                Fmt(" t=%ums present=%llu", u.ms, static_cast<unsigned long long>(u.present)));
                ++_dumped;
                _dumpedUpTo = k + 1;
            }
        }

        for (size_t c = 0; c < kCatalogCount; ++c)
            _loggedMask[c] = mask[c];
        _candSeen = candidates;
        return lines;
    }

  private:
    template <class... A> static std::string Fmt(const char* format, A... args)
    {
        char buf[512];
        const int n = std::snprintf(buf, sizeof buf, format, args...);
        return n < 0 ? std::string() : std::string(buf, static_cast<size_t>(n < static_cast<int>(sizeof buf) ? n : static_cast<int>(sizeof buf) - 1));
    }

    // The first creation of entry (c, i): the thread that wins the claim records it, then publishes the slot, then the mask bit.
    void First(size_t c, size_t i, const void* code, size_t size, uint32_t order, uint32_t nowMs, const ShaderFingerprint*& computed) noexcept
    {
        const uint32_t bit = 1u << i;
        if (_claimed[c].fetch_or(bit, std::memory_order_acq_rel) & bit)
            return; // already seen: nothing is overwritten, nothing is computed
        Slot& s = _slots[_slotBase[c] + i];
        if (computed)
            s.fp = *computed;
        else
        {
            s.fp = Fingerprint(code, size);
            computed = &s.fp;
            _fingerprints.fetch_add(1, std::memory_order_relaxed);
        }
        s.ms = nowMs;
        s.present = _present.load(std::memory_order_relaxed);
        s.order = order;
        s.ready.store(1, std::memory_order_release);
        _mask[c].fetch_or(bit, std::memory_order_release); // the bit is only ever visible after the slot is complete
    }

    // A shader outside every catalog: distinct CRCs only, first kUncatalogedCap of them.
    void Sample(const void* code, size_t size, uint32_t crc, uint32_t order, uint32_t nowMs, const ShaderFingerprint* have) noexcept
    {
        if (_uncatClaimed.load(std::memory_order_relaxed) >= kUncatalogedCap)
            return; // hard cap: nothing more is fingerprinted or recorded
        if (!Insert(crc))
            return; // this CRC is already in the sample (or was seen when the sample filled up)
        const uint32_t index = _uncatClaimed.fetch_add(1, std::memory_order_relaxed);
        if (index >= kUncatalogedCap)
            return;
        ShaderFingerprint own; // only filled when the caller has no fingerprint of these bytes yet
        if (!have)
        {
            own = Fingerprint(code, size);
            _fingerprints.fetch_add(1, std::memory_order_relaxed);
        }
        const ShaderFingerprint& fp = have ? *have : own;
        Uncatalogued& u = _uncat[index];
        u.crc = crc;
        u.size = static_cast<uint32_t>(size);
        u.order = order;
        u.ms = nowMs;
        u.present = _present.load(std::memory_order_relaxed);
        u.valid = fp.valid;
        u.error = fp.error;
        u.fpHash = fp.hash;
        u.hasProgram = fp.hasProgram;
        u.tokens = fp.tokenCount;
        u.chunks = static_cast<uint16_t>(fp.chunkCount);
        u.programType = static_cast<uint8_t>(fp.programType);
        u.major = fp.versionMajor;
        u.minor = fp.versionMinor;
        u.inputs = static_cast<uint8_t>(fp.input.count > 255 ? 255 : fp.input.count);
        u.outputs = static_cast<uint8_t>(fp.output.count > 255 ? 255 : fp.output.count);
        u.ready.store(1, std::memory_order_release);
    }

    // Does this uncatalogued shader have the structure of a reference of a catalog entry that does not exist (yet)? Then keep it as a candidate.
    // Entries already created are skipped: they are not missing, and their same-structure siblings must not use up the few candidate slots.
    void NoteReferenceMatch(const ShaderFingerprint& fp, uint32_t crc, size_t size, uint32_t order, uint32_t nowMs) noexcept
    {
        for (size_t k = 0; k < _refN; ++k) // an invalid shader has hash 0, a reference is never 0: it cannot match
        {
            if (fp.hash != _refHash[k])
                continue;
            if ((_claimed[_refCatalog[k]].load(std::memory_order_relaxed) >> _refIndex[k]) & 1u)
                continue; // that entry exists
            KeepCandidate(fp.hash, crc, size, order, nowMs);
            return;
        }
    }

    // Fill-once set of distinct candidate CRCs: claim a slot with a CAS on its key, fill it, publish it. A CRC already kept is not kept twice.
    void KeepCandidate(uint64_t hash, uint32_t crc, size_t size, uint32_t order, uint32_t nowMs) noexcept
    {
        const uint64_t key = uint64_t(crc) | (1ull << 32); // 0 means free
        for (size_t k = 0; k < kCandidateCap; ++k)
        {
            uint64_t expected = 0;
            if (_candKey[k].compare_exchange_strong(expected, key, std::memory_order_acq_rel))
            {
                Candidate& u = _cand[k];
                u.crc = crc;
                u.size = static_cast<uint32_t>(size);
                u.order = order;
                u.ms = nowMs;
                u.present = _present.load(std::memory_order_relaxed);
                u.fpHash = hash;
                u.ready.store(1, std::memory_order_release);
                _candReady.fetch_add(1, std::memory_order_release);
                return;
            }
            if (expected == key)
                return; // already kept
        }
        _candDropped.fetch_add(1, std::memory_order_relaxed); // full
    }

    // Open addressing set of CRCs (never full: at most kUncatalogedCap + the number of racing threads are inserted). True = newly inserted.
    bool Insert(uint32_t crc) noexcept
    {
        const uint64_t key = uint64_t(crc) | (1ull << 32); // 0 means empty
        size_t at = (uint32_t(crc * 2654435761u) >> 22) & (kKeys - 1);
        for (size_t probe = 0; probe < kKeys; ++probe, at = (at + 1) & (kKeys - 1))
        {
            uint64_t expected = 0;
            if (_keys[at].compare_exchange_strong(expected, key, std::memory_order_acq_rel))
                return true;
            if (expected == key)
                return false;
        }
        return false; // full: cannot happen below the cap; treat as seen
    }

    static std::string UncatText(const Uncatalogued& u)
    {
        if (!u.valid)
            return std::string("fp=invalid(") + FpErrorName(u.error) + ")";
        std::string s = Fmt("fp=0x%016llx size=%u chunks=%u", static_cast<unsigned long long>(u.fpHash), u.size, static_cast<unsigned>(u.chunks));
        if (u.hasProgram)
            s += Fmt(" model=%s_%u_%u tokens=%u", ProgramTypeName(u.programType), static_cast<unsigned>(u.major), static_cast<unsigned>(u.minor), u.tokens);
        else
            s += " model=none";
        s += Fmt(" io=%u->%u", static_cast<unsigned>(u.inputs), static_cast<unsigned>(u.outputs));
        return s;
    }

    std::string SummaryLine(const uint32_t* mask, uint32_t nowMs, uint64_t present) const
    {
        std::string s = "[ffxiv-catalog]";
        for (size_t c = 0; c < kCatalogCount; ++c)
        {
            const CatalogInfo& info = _tables[c];
            if (info.vertexShaders && _layouts.load(std::memory_order_relaxed) == 0)
                s += Fmt(" %s -/%u", info.name, static_cast<unsigned>(info.count));
            else
                s += Fmt(" %s %u/%u", info.name, static_cast<unsigned>(std::popcount(mask[c])), static_cast<unsigned>(info.count));
        }
        s += Fmt(" | pixel shaders created %u, uncatalogued sampled %u/%u", _created.load(std::memory_order_relaxed), UncatalogedRecorded(),
                 static_cast<unsigned>(kUncatalogedCap));
        if (_refN != 0) // the near-miss search is on: every uncatalogued shader was compared with the references
            s += Fmt(", reference matches kept %u/%u dropped %u", CandidatesKept(), static_cast<unsigned>(kCandidateCap), CandidatesDropped());
        s += Fmt(" | t=%ums present=%llu | ", nowMs, static_cast<unsigned long long>(present));
        const unsigned ui = static_cast<unsigned>(std::popcount(mask[static_cast<size_t>(Catalog::Ui)] & kUiCatalogMask));
        if (UiCatalogHealthy(mask[static_cast<size_t>(Catalog::Ui)]))
            s += "UI boundary ok";
        else
            s += Fmt("UI boundary FALLBACK (UI %u/7, needs %s: HUD-less reports NoBoundaryThisFrame until it is complete)", ui,
                     kUiRequiredMask != 0 ? "the pinned set" : std::to_string(kUiCatalogHealthyMin).c_str());
        return s;
    }

    bool FingerprintLogged(uint32_t crc) const
    {
        for (size_t k = 0; k < _fpLoggedN; ++k)
            if (_fpLogged[k] == crc)
                return true;
        return false;
    }
    uint32_t CandidateCount(uint64_t ref) const
    {
        uint32_t n = 0;
        for (size_t k = 0; k < kCandidateCap; ++k)
            if (_cand[k].ready.load(std::memory_order_acquire) == 1 && _cand[k].fpHash == ref)
                ++n;
        return n;
    }
    bool CandidateLogged(uint32_t k) const { return (_candLogged >> k) & 1u; }
    void MarkCandidate(uint32_t k) { _candLogged |= 1u << k; }

    static constexpr size_t kKeys = 1024;
    static constexpr size_t kFpLoggedCap = 64;

    const CatalogInfo* _tables;
    const bool _fits; // LedgerFits(_tables)

    // feeding side (any thread)
    std::atomic<uint32_t> _claimed[kCatalogCount] {}; // an entry's first creation is being or has been recorded
    std::atomic<uint32_t> _mask[kCatalogCount] {};    // the entry's slot is complete
    size_t _slotBase[kCatalogCount + 1] {};
    Slot _slots[kLedgerMaxSlots] {};
    std::atomic<uint64_t> _present { 0 };
    std::atomic<uint32_t> _created { 0 }, _vertex { 0 }, _layouts { 0 }, _fingerprints { 0 };
    std::atomic<uint32_t> _uncatClaimed { 0 };
    std::atomic<uint64_t> _keys[kKeys] {};
    Uncatalogued _uncat[kUncatalogedCap] {};
    // near-miss search (see kCandidateCap)
    uint64_t _refHash[kLedgerMaxSlots] {}; // the non-zero reference fingerprints of the pixel shader catalogs, and where they belong
    uint8_t _refCatalog[kLedgerMaxSlots] {}, _refIndex[kLedgerMaxSlots] {};
    size_t _refN = 0;
    std::atomic<uint64_t> _candKey[kCandidateCap] {};
    Candidate _cand[kCandidateCap] {};
    std::atomic<uint32_t> _candReady { 0 };   // candidates published so far
    std::atomic<uint32_t> _candDropped { 0 }; // matches that found the kept set full

    // log side (Present thread, under _polling)
    std::atomic_flag _polling;
    bool _settled = false;
    bool _inertReported = false;
    uint32_t _loggedMask[kCatalogCount] {};
    uint32_t _missingFor[kCatalogCount] {};
    uint32_t _fpLogged[kFpLoggedCap] {};
    size_t _fpLoggedN = 0;
    size_t _dumped = 0;
    size_t _dumpedUpTo = 0;
    static_assert(kCandidateCap <= 32, "_candLogged is one 32-bit word");
    uint32_t _candLogged = 0; // candidates already named in the log
    uint32_t _candSeen = 0;   // _candReady at the last Poll
};
} // namespace ext::ffxiv::catalog
