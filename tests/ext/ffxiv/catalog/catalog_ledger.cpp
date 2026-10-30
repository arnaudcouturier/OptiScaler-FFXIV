// Per-catalog hit accounting (CatalogLedger.h): which catalog CRCs have been created this session, when, and what the
// summary/first-seen/fingerprint/near-miss log lines say.
//
// What this pins:
//   * every catalog entry sets exactly its own bit (UI 7, scene 5, lighting 9 with the size check, chara 9, companion 4, nameplate 3
//     as vertex shaders), a CRC that belongs to two catalogs sets both bits, the pixel and the vertex namespaces stay apart;
//   * the first creation of an entry records time (ms since hook install), the Present count and the creation order ONCE, and its
//     fingerprint; repeats change nothing and compute nothing;
//   * the creation path performs ZERO heap allocations (it runs inside the CreatePixelShader hook), the uncatalogued sample is deduplicated
//     by CRC and hard-capped, and the fingerprint is computed at most once per catalog entry and once per sampled shader;
//   * UiHealthy() follows the UI mask per call (a late creation clears the fallback);
//   * the log: nothing before the session settled, one summary line (+ one first-seen and one fingerprint line per newly seen CRC) at
//     settle and again whenever a count changes, never twice for the same CRC; missing entries are listed; near-miss candidates are
//     named only on an equal non-zero reference fingerprint, each once; once any pixel catalog entry has a reference EVERY uncatalogued
//     shader is compared at creation (not just the first 256) and only matches are kept (16, distinct, fill once); the uncatalogued dump
//     appears only when the UI catalog (the only one with an established baseline) is below it and is capped;
//   * concurrent creation from several threads loses no bit and keeps the cap.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "check.h"
#include "dxbc_builder.h"

#include "ext/ffxiv/catalog/CatalogLedger.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <thread>
#include <vector>

using namespace ext::ffxiv::catalog;
using namespace testdxbc;

// ---- allocation counting ---------------------------------------------------------------------------------------------------------
namespace
{
bool g_count = false;
long g_allocs = 0;
} // namespace

void* operator new(size_t n)
{
    if (g_count)
        ++g_allocs;
    if (void* p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

namespace
{
const std::vector<uint8_t> g_zeros(70000, 0);

size_t Index(Catalog c) { return static_cast<size_t>(c); }

void NoteEntry(Ledger& l, Catalog c, size_t i, uint32_t ms)
{
    const Entry& e = Info(c).entries[i];
    if (Info(c).vertexShaders)
        l.NoteVertexShader(g_zeros.data(), 100, e.crc, ms);
    else
        l.NotePixelShader(g_zeros.data(), e.bytes ? e.bytes : 100, e.crc, ms);
}

void NoteAll(Ledger& l, Catalog c, uint32_t ms)
{
    for (size_t i = 0; i < Info(c).count; ++i)
        NoteEntry(l, c, i, ms);
}

void Presents(Ledger& l, uint64_t n)
{
    for (uint64_t i = 0; i < n; ++i)
        l.NotePresent();
}

size_t CountLines(const std::vector<std::string>& lines, const char* needle)
{
    size_t n = 0;
    for (const auto& s : lines)
        if (s.find(needle) != std::string::npos)
            ++n;
    return n;
}

std::string Joined(const std::vector<std::string>& lines)
{
    std::string s;
    for (const auto& x : lines)
        s += x + "\n";
    return s;
}

// A DXBC container of a given length class: distinct sizes give distinct fingerprints.
Bytes Blob(uint32_t bodyTokens) { return BuildDxbc(StrippedPs(bodyTokens)); }

void Fresh()
{
    Ledger l;
    for (size_t c = 0; c < kCatalogCount; ++c)
    {
        CHECK_EQ(l.Mask(static_cast<Catalog>(c)), 0u);
        CHECK_EQ(l.Count(static_cast<Catalog>(c)), 0u);
    }
    CHECK(!l.UiHealthy());
    CHECK_EQ(l.Present(), 0u);
    CHECK_EQ(l.PixelShadersSeen(), 0u);
    CHECK_EQ(l.UncatalogedRecorded(), 0u);
    CHECK_EQ(l.FingerprintsComputed(), 0u);
    CHECK(l.Poll(1000000).empty()); // nothing to say before anything happened, whatever the clock
}

// Every entry sets exactly its own bit; the catalog is full when all of them were seen.
void CountsPerCatalog()
{
    for (size_t c = 0; c < kCatalogCount; ++c)
    {
        const Catalog cat = static_cast<Catalog>(c);
        Ledger l;
        for (size_t i = 0; i < Info(cat).count; ++i)
        {
            NoteEntry(l, cat, i, 10 + uint32_t(i));
            CHECK(l.Mask(cat) & (1u << i));
            CHECK_EQ(l.Count(cat), i + 1);
            NoteEntry(l, cat, i, 999); // a repeat changes nothing
            CHECK_EQ(l.Count(cat), i + 1);
        }
        CHECK_EQ(l.Mask(cat), (1u << Info(cat).count) - 1);
    }
    // One catalog at a time: noting every UI entry does not move the scene, lighting, chara or nameplate masks (companion is inside UI).
    Ledger l;
    NoteAll(l, Catalog::Ui, 5);
    CHECK_EQ(l.Count(Catalog::Ui), 7u);
    CHECK_EQ(l.Count(Catalog::Scene), 0u);
    CHECK_EQ(l.Count(Catalog::Lighting), 0u);
    CHECK_EQ(l.Count(Catalog::Chara), 0u);
    CHECK_EQ(l.Count(Catalog::Nameplate), 0u);
    CHECK_EQ(l.Count(Catalog::Companion), 4u); // all four companion shaders are UI shaders
}

void Overlaps()
{
    // ToneMapping (lighting #0, 1512 bytes) is scene stage 1 too: one creation, two bits. ToneAdjust likewise (lighting #1, scene #4).
    Ledger l;
    l.NotePixelShader(g_zeros.data(), 1512, 0x85e777efu, 1);
    CHECK_EQ(l.Mask(Catalog::Scene), 1u);
    CHECK_EQ(l.Mask(Catalog::Lighting), 1u);
    l.NotePixelShader(g_zeros.data(), 1292, 0xf6e81a1bu, 2);
    CHECK_EQ(l.Mask(Catalog::Scene), 1u | (1u << 4));
    CHECK_EQ(l.Mask(Catalog::Lighting), 3u);

    // The lighting entries carry their DXBC size (FfxivLightingCapture::Identify): the same CRC at another size is a scene shader only.
    Ledger m;
    m.NotePixelShader(g_zeros.data(), 1511, 0x85e777efu, 1);
    CHECK_EQ(m.Mask(Catalog::Scene), 1u);
    CHECK_EQ(m.Mask(Catalog::Lighting), 0u);

    // Companion layouts are UI shaders: companion layout0 = UI #0, layout4 = UI #6, layout5 = UI #2, layout6 = UI #1.
    Ledger u;
    u.NotePixelShader(g_zeros.data(), 100, 1823062889u, 1); // layout4 = ui6
    CHECK_EQ(u.Mask(Catalog::Ui), 1u << 6);
    CHECK_EQ(u.Mask(Catalog::Companion), 1u << 1);
    u.NotePixelShader(g_zeros.data(), 100, 1898885941u, 2); // ui3: a UI shader that is not a companion shader
    CHECK_EQ(u.Mask(Catalog::Ui), (1u << 6) | (1u << 3));
    CHECK_EQ(u.Mask(Catalog::Companion), 1u << 1);
}

void Namespaces()
{
    Ledger l;
    // A nameplate CRC is a vertex shader CRC: seen as a pixel shader it counts nowhere.
    l.NotePixelShader(g_zeros.data(), 100, Info(Catalog::Nameplate).entries[0].crc, 1);
    CHECK_EQ(l.Count(Catalog::Nameplate), 0u);
    // A UI CRC seen as a vertex shader counts nowhere.
    l.NoteVertexShader(g_zeros.data(), 100, Info(Catalog::Ui).entries[0].crc, 2);
    CHECK_EQ(l.Count(Catalog::Ui), 0u);
    CHECK_EQ(l.Count(Catalog::Companion), 0u);
    // (As a pixel shader it is just a shader in no catalog: it is in the uncatalogued sample.) Vertex shaders that are in no catalog do not even
    // reach the sample.
    CHECK_EQ(l.UncatalogedRecorded(), 1u);
    l.NoteVertexShader(g_zeros.data(), 100, 0x12345678u, 3);
    CHECK_EQ(l.UncatalogedRecorded(), 1u);
    CHECK_EQ(l.PixelShadersSeen(), 1u); // the one pixel shader above
    l.NoteVertexShader(g_zeros.data(), 100, Info(Catalog::Nameplate).entries[0].crc, 4);
    CHECK_EQ(l.Count(Catalog::Nameplate), 1u);
}

// First creation: time, Present count, order and fingerprint are recorded once.
void FirstSeen()
{
    Ledger l;
    Presents(l, 5);
    const Bytes a = Blob(40);
    l.NotePixelShader(a.data(), a.size(), Info(Catalog::Ui).entries[2].crc, 1234);
    const Slot& s = l.SlotAt(Catalog::Ui, 2);
    CHECK_EQ(s.ready.load(), 1u);
    CHECK_EQ(s.ms, 1234u);
    CHECK_EQ(s.present, 5u);
    CHECK_EQ(s.order, 1u);
    CHECK(s.fp.valid);
    CHECK_EQ(s.fp.hash, Fingerprint(a.data(), a.size()).hash);
    CHECK_EQ(l.FingerprintsComputed(), 1u);

    Presents(l, 20);
    const Bytes b = Blob(90); // the same CRC created again later with other bytes (a re-creation): nothing is overwritten
    l.NotePixelShader(b.data(), b.size(), Info(Catalog::Ui).entries[2].crc, 99999);
    CHECK_EQ(s.ms, 1234u);
    CHECK_EQ(s.present, 5u);
    CHECK_EQ(s.order, 1u);
    CHECK_EQ(s.fp.hash, Fingerprint(a.data(), a.size()).hash);
    CHECK_EQ(l.FingerprintsComputed(), 1u);

    // the creation order counts every pixel shader the hook saw, catalog or not
    l.NotePixelShader(g_zeros.data(), 100, 0xABCDEF01u, 5);
    l.NotePixelShader(g_zeros.data(), 100, Info(Catalog::Ui).entries[4].crc, 6);
    CHECK_EQ(l.SlotAt(Catalog::Ui, 4).order, 4u);
    CHECK_EQ(l.SlotAt(Catalog::Ui, 4).present, 25u);
    CHECK_EQ(l.PixelShadersSeen(), 4u);

    // an entry seen through two catalogs (ToneMapping) is fingerprinted once for the creation, stored in both slots
    const Bytes tone(1512, 7); // size equals the lighting entry's; not a DXBC, so the fingerprint is "invalid" but recorded
    l.NotePixelShader(tone.data(), tone.size(), 0x85e777efu, 7);
    CHECK_EQ(l.SlotAt(Catalog::Scene, 0).order, l.SlotAt(Catalog::Lighting, 0).order);
    CHECK(!l.SlotAt(Catalog::Scene, 0).fp.valid);
    CHECK_EQ(l.FingerprintsComputed(), 4u); // ui2's blob, the uncatalogued shader, ui4's zeros, tone
}

// The partial-UI guard follows the mask on every call.
void UiHealth()
{
    Ledger l;
    for (size_t i = 0; i < 5; ++i)
    {
        NoteEntry(l, Catalog::Ui, i, 1);
        CHECK(!l.UiHealthy());
    }
    NoteEntry(l, Catalog::Ui, 6, 2); // the pinned sixth (ui6), not ui5
    CHECK(l.UiHealthy()); // a late creation clears the fallback, no latch
    NoteEntry(l, Catalog::Ui, 5, 3); // ui5 completes the set
    CHECK(l.UiHealthy());

    // Other catalogs do not count towards the UI set, uncatalogued shaders neither.
    Ledger m;
    for (size_t i = 0; i < 5; ++i)
        NoteEntry(m, Catalog::Ui, i, 1);
    NoteAll(m, Catalog::Scene, 2);
    NoteAll(m, Catalog::Chara, 3);
    for (uint32_t i = 0; i < 50; ++i)
        m.NotePixelShader(g_zeros.data(), 100, 0x70000000u + i, 4);
    CHECK(!m.UiHealthy());
    CHECK_EQ(m.Mask(Catalog::Ui), 0x1Fu);
    NoteEntry(m, Catalog::Ui, 5, 5); // ui5 without ui3: 6 of 7, still FALLBACK (not the pinned set)
    CHECK(!m.UiHealthy());
    CHECK_EQ(m.Mask(Catalog::Ui), 0x3Fu);
    NoteEntry(m, Catalog::Ui, 6, 6); // the pinned sixth arrives: a superset of the pinned set is healthy
    CHECK(m.UiHealthy());
}

// The uncatalogued sample: distinct CRCs only, hard cap, fingerprint at most once per recorded shader, no heap allocation.
void UncatalogedSample()
{
    static_assert(kUncatalogedCap == 256);
    Ledger l;
    const Bytes blob = Blob(60);
    for (int i = 0; i < 1000; ++i)
        l.NotePixelShader(blob.data(), blob.size(), 0x11111111u, 1); // one shader created a thousand times
    CHECK_EQ(l.UncatalogedRecorded(), 1u);
    CHECK_EQ(l.FingerprintsComputed(), 1u);
    CHECK_EQ(l.PixelShadersSeen(), 1000u);

    for (uint32_t i = 0; i < 600; ++i)
        l.NotePixelShader(blob.data(), blob.size(), 0x20000000u + i, 2 + i);
    CHECK_EQ(l.UncatalogedRecorded(), kUncatalogedCap);
    CHECK_EQ(l.FingerprintsComputed(), kUncatalogedCap); // never more than the cap, however many shaders come
    // creation order is kept: the first recorded one is the first shader, then the distinct ones in order
    CHECK_EQ(l.UncatalogedAt(0).crc, 0x11111111u);
    CHECK_EQ(l.UncatalogedAt(1).crc, 0x20000000u);
    CHECK_EQ(l.UncatalogedAt(1).order, 1001u);
    CHECK_EQ(l.UncatalogedAt(2).order, 1002u);
    CHECK(l.UncatalogedAt(255).ready.load() == 1u);
    CHECK_EQ(l.UncatalogedAt(0).fpHash, Fingerprint(blob.data(), blob.size()).hash);
    // a CRC beyond the cap is not recorded, and one inside it is still not recorded twice
    l.NotePixelShader(blob.data(), blob.size(), 0x20000001u, 9);
    CHECK_EQ(l.UncatalogedRecorded(), kUncatalogedCap);

    // Catalog entries are never part of the uncatalogued sample.
    Ledger m;
    NoteAll(m, Catalog::Ui, 1);
    NoteAll(m, Catalog::Scene, 1);
    NoteAll(m, Catalog::Chara, 1);
    CHECK_EQ(m.UncatalogedRecorded(), 0u);
}

void NoAllocations()
{
    Ledger l; // construction is outside the measured path
    const Bytes blob = Blob(200);
    const Bytes tiny = Blob(10);
    g_allocs = 0;
    g_count = true;
    for (uint32_t i = 0; i < 20000; ++i)
    {
        const Catalog c = static_cast<Catalog>(i % 5);
        const CatalogInfo& info = Info(c);
        const Entry& e = info.entries[i % info.count];
        if (i % 3 == 0)
            l.NotePixelShader(e.bytes ? static_cast<const void*>(g_zeros.data()) : blob.data(), e.bytes ? e.bytes : blob.size(), e.crc, i); // catalog hit (first: fingerprint + slot)
        else if (i % 3 == 1)
            l.NotePixelShader(tiny.data(), tiny.size(), 0x9e3779b9u * i, i); // uncatalogued (first 256 distinct: fingerprint + record)
        else
            l.NotePixelShader(g_zeros.data(), 100, 0xC0FFEE00u, i); // a repeat
        l.NoteVertexShader(blob.data(), blob.size(), Info(Catalog::Nameplate).entries[i % 3].crc, i);
        l.NoteLayoutSeen();
        l.NotePresent();
    }
    g_count = false;
    CHECK_EQ(g_allocs, 0);
    CHECK(l.Count(Catalog::Ui) > 0);
    CHECK_EQ(l.UncatalogedRecorded(), kUncatalogedCap);
}

// ---- log lines ---------------------------------------------------------------------------------------------------------------------
void Settling()
{
    Ledger l;
    const Bytes blob = Blob(40);
    l.NotePixelShader(blob.data(), blob.size(), Info(Catalog::Ui).entries[0].crc, 1000);
    Presents(l, 599);
    CHECK(l.Poll(kSettleMs + 5000).empty()); // enough time, not enough Presents
    l.NotePresent();
    CHECK(l.Poll(kSettleMs - 1).empty()); // enough Presents, not enough time
    const auto lines = l.Poll(kSettleMs);
    CHECK(!lines.empty());
    CHECK(l.Poll(kSettleMs + 1).empty()); // nothing changed since
    CHECK(l.Poll(kSettleMs + 1000000).empty());
}

void LogLines()
{
    Ledger l;
    // Pixel shaders created over time: the pinned six UI entries (0,1,2,3,4,6, missing only ui5: the healthy state), all scene stages, all chara; companion 4 = 3 + ...
    for (size_t i = 0; i < 6; ++i)
    {
        Presents(l, 10);
        const Bytes b = Blob(40 + uint32_t(i));
        l.NotePixelShader(b.data(), b.size(), Info(Catalog::Ui).entries[i == 5 ? 6 : i].crc, 1000 + uint32_t(i) * 100); // the pinned six (ui5 is scene-dependent)
    }
    NoteAll(l, Catalog::Scene, 5000);
    NoteAll(l, Catalog::Chara, 6000);
    NoteAll(l, Catalog::Lighting, 7000);
    l.NoteLayoutSeen();
    NoteAll(l, Catalog::Nameplate, 8000);
    Presents(l, 700);

    const auto first = l.Poll(40000);
    const std::string text = Joined(first);
    // one summary line with every catalog
    CHECK_HAS(text, "[ffxiv-catalog] UI 6/7 scene 5/5 lighting 9/9 chara 9/9 companion 4/4 nameplate 3/3");
    CHECK_HAS(text, "UI boundary ok");
    CHECK_EQ(CountLines(first, "[ffxiv-catalog] UI 6/7 "), 1u);
    // one first-seen line per entry that was seen (6 + 5 + 9 + 9 + 4 + 3 companion = 36; 6 CRCs are in two catalogs and still get a line per catalog)
    CHECK_EQ(CountLines(first, "first seen crc=0x"), 6u + 5u + 9u + 9u + 4u + 3u);
    CHECK_HAS(text, "first seen crc=0xe66dae4e name=UI:ui0 t=1000ms present=10 order=1");
    CHECK_HAS(text, "first seen crc=0xb0d428d9 name=UI:ui1 t=1100ms present=20 order=2");
    CHECK_HAS(text, "first seen crc=0x85e777ef name=scene:tonemap");
    CHECK_HAS(text, "first seen crc=0x85e777ef name=lighting:ToneMapping");
    CHECK_HAS(text, "first seen crc=0xb0af152e name=nameplate:vs-layout0");
    // one fingerprint line per distinct CRC: 6 UI + 5 scene + (9 lighting - 2 shared with scene) + 9 chara + 3 nameplate = 30 (companion shares with UI)
    CHECK_EQ(CountLines(first, "fingerprint crc=0x"), 6u + 5u + 7u + 9u + 3u);
    CHECK_HAS(text, "fingerprint crc=0xe66dae4e name=UI:ui0 fp=0x");
    CHECK_HAS(text, " size=");
    CHECK_HAS(text, " chunks=ISGN:");
    CHECK_HAS(text, "model=ps_5_0");
    // the one missing UI shader (ui5) is listed, as within the baseline; every other catalog has all its entries
    CHECK_HAS(text, "UI #5 (ui5) crc=0xe95deec4 missing (within the healthy baseline of 6)");
    CHECK(text.find("companion #") == std::string::npos); // companion is complete (all four are UI shaders of the pinned six)
    CHECK_EQ(CountLines(first, " missing"), 1u); // only the ui5 line
    // healthy: no uncatalogued dump
    CHECK_EQ(CountLines(first, "uncatalogued #"), 0u);

    // Not twice: the same state logs nothing...
    CHECK(l.Poll(41000).empty());

    // ...and a changed count logs the summary again plus ONLY the new CRC.
    const Bytes late = Blob(77);
    l.NotePixelShader(late.data(), late.size(), Info(Catalog::Ui).entries[5].crc, 45000); // the late ui5 completes the set
    const auto second = l.Poll(46000);
    const std::string text2 = Joined(second);
    CHECK_HAS(text2, "[ffxiv-catalog] UI 7/7 scene 5/5 lighting 9/9 chara 9/9 companion 4/4 nameplate 3/3");
    CHECK_EQ(CountLines(second, "first seen crc=0x"), 1u); // only ui5 (companion was already complete)
    CHECK_HAS(text2, "first seen crc=0xe95deec4 name=UI:ui5 t=45000ms");
    CHECK_EQ(CountLines(second, "name=companion:"), 0u); // no companion entry is new
    CHECK_EQ(CountLines(second, "fingerprint crc=0x"), 1u);
    CHECK_EQ(CountLines(second, " missing"), 0u);
    CHECK(l.Poll(47000).empty());
}

void NameplateNotes()
{
    Ledger l;
    NoteAll(l, Catalog::Ui, 100);
    Presents(l, 700);
    // No CreateInputLayout seen at all: the nameplate catalog is "not measured", not "0 of 3 missing".
    const std::string noLayouts = Joined(l.Poll(40000));
    CHECK_HAS(noLayouts, "nameplate -/3");
    CHECK(noLayouts.find("nameplate #") == std::string::npos);

    Ledger m;
    NoteAll(m, Catalog::Ui, 100);
    m.NoteLayoutSeen();
    Presents(m, 700);
    const std::string seen = Joined(m.Poll(40000));
    CHECK_HAS(seen, "nameplate 0/3");
    CHECK_HAS(seen, "nameplate #0 (vs-layout0) crc=0xb0af152e missing");
}

// A catalog below its established baseline triggers the near-miss output: the uncatalogued shaders of the session (capped), and the candidates
// named by an equal reference fingerprint (each once, never by anything else).
void NearMiss()
{
    // a table copy with a reference fingerprint on UI #3
    const Bytes recompiled = Blob(123); // the recompiled UI #3 after a "patch"
    const Bytes other = Blob(321);
    const uint64_t ref = Fingerprint(recompiled.data(), recompiled.size()).hash;
    CHECK(ref != 0);
    CatalogInfo tables[kCatalogCount];
    std::memcpy(tables, kInfo, sizeof tables);
    Entry ui[7];
    std::memcpy(ui, table::kUi, sizeof ui);
    ui[3].refFingerprint = ref;
    tables[Index(Catalog::Ui)].entries = ui;

    Ledger l(tables);
    for (size_t i = 0; i < 7; ++i)
        if (i != 3 && i != 5) // UI 5 of 7: below the baseline of 6
            NoteEntry(l, Catalog::Ui, i, 100 + uint32_t(i));
    l.NotePixelShader(other.data(), other.size(), 0xDEAD0002u, 200);
    l.NotePixelShader(recompiled.data(), recompiled.size(), 0xDEAD0001u, 300); // same structure as the reference, new CRC
    l.NotePixelShader(g_zeros.data(), 100, 0xDEAD0003u, 400);                  // not a DXBC at all
    Presents(l, 700);
    CHECK(!l.UiHealthy());

    const auto lines = l.Poll(40000);
    const std::string text = Joined(lines);
    CHECK_HAS(text, "UI boundary FALLBACK (UI 5/7, needs the pinned set: HUD-less reports NoBoundaryThisFrame until it is complete)");
    CHECK_HAS(text, "UI #3 (ui3) crc=0x712eb335 missing");
    CHECK_HAS(text, "UI #5 (ui5) crc=0xe95deec4 missing; no reference fingerprint");
    // the candidate: equal fingerprint only
    CHECK_EQ(CountLines(lines, "candidate crc=0x"), 1u);
    CHECK_HAS(text, "UI #3 (ui3) missing; candidate crc=0xdead0001 same fingerprint");
    CHECK(text.find("candidate crc=0xdead0002") == std::string::npos);
    CHECK(text.find("candidate crc=0xdead0003") == std::string::npos);
    // only UI has an established baseline: scene (0/5) and chara (0/9) are empty here and are NOT named as below a baseline
    CHECK_HAS(text, "near-miss: UI 5/7, required set of 6 incomplete; shaders created this session");
    CHECK_EQ(CountLines(lines, "required set of 6 incomplete"), 1u);
    // the dump of the session's uncatalogued shaders, with fingerprints and creation order
    CHECK_HAS(text, "uncatalogued #6 crc=0xdead0002 fp=0x");
    CHECK_HAS(text, "uncatalogued #7 crc=0xdead0001 fp=0x");
    CHECK_HAS(text, "uncatalogued #8 crc=0xdead0003 fp=invalid(not DXBC)");

    // A later change does not name the same candidate again; refFingerprint never changes CRC matching.
    NoteEntry(l, Catalog::Ui, 3, 500); // the original CRC shows up after all (and has a reference set): counted like any other
    CHECK_EQ(l.Count(Catalog::Ui), 6u);
    const auto again = l.Poll(41000);
    CHECK_EQ(CountLines(again, "candidate crc=0x"), 0u);
    CHECK_EQ(CountLines(again, "uncatalogued #"), 0u); // dumped once per session
    CHECK(l.UiHealthy());

    // A reference that nothing matches says so (and still names no candidate).
    CatalogInfo tables2[kCatalogCount];
    std::memcpy(tables2, kInfo, sizeof tables2);
    Entry ui2[7];
    std::memcpy(ui2, table::kUi, sizeof ui2);
    ui2[3].refFingerprint = 0x1122334455667788ull;
    tables2[Index(Catalog::Ui)].entries = ui2;
    Ledger m(tables2);
    for (size_t i = 0; i < 7; ++i)
        if (i != 3 && i != 5) // UI 5 of 7, ui3 (the one with the reference) missing
            NoteEntry(m, Catalog::Ui, i, 1);
    m.NotePixelShader(other.data(), other.size(), 0xDEAD0002u, 2);
    Presents(m, 700);
    const auto none = m.Poll(40000);
    CHECK_EQ(CountLines(none, "candidate crc=0x"), 0u);
    CHECK_HAS(Joined(none), "UI #3 (ui3) crc=0x712eb335 missing; reference fp=0x1122334455667788 matches no uncatalogued shader created so far");
}

void DumpIsCapped()
{
    static_assert(kUncatalogedLogCap == 32);
    Ledger l;
    for (size_t i = 0; i < 3; ++i)
        NoteEntry(l, Catalog::Ui, i, 1); // 3 of 7 UI: below the baseline
    const Bytes b = Blob(30);
    for (uint32_t i = 0; i < 300; ++i)
        l.NotePixelShader(b.data(), b.size(), 0x30000000u + i, 10 + i);
    Presents(l, 700);
    const auto first = l.Poll(40000);
    CHECK_EQ(CountLines(first, "uncatalogued #"), kUncatalogedLogCap);
    NoteEntry(l, Catalog::Ui, 3, 50000);
    const auto second = l.Poll(50001);
    CHECK_EQ(CountLines(second, "uncatalogued #"), 0u); // the cap is per session
    // the dump starts at the first uncatalogued shader created (creation order)
    CHECK_HAS(Joined(first), "uncatalogued #4 crc=0x30000000");

    // Healthy baseline met: never a dump, however many shaders are uncatalogued.
    Ledger h;
    for (size_t i = 0; i < 6; ++i)
        NoteEntry(h, Catalog::Ui, i == 5 ? 6 : i, 1); // the pinned six (ui5 is scene-dependent)
    NoteAll(h, Catalog::Scene, 1);
    NoteAll(h, Catalog::Chara, 1);
    for (uint32_t i = 0; i < 300; ++i)
        h.NotePixelShader(b.data(), b.size(), 0x40000000u + i, 10 + i);
    Presents(h, 700);
    CHECK_EQ(CountLines(h.Poll(40000), "uncatalogued #"), 0u);
}

// A HEALTHY launch must be quiet. Only the UI catalog has an established baseline (6 of 7). The scene stage 3 (0x27ebc404, lut3) is probably
// absent from this build, and the character finisher is created when the lobby starts, which can be after the settle point: neither may
// produce a "below the healthy baseline" block or the dump of the uncatalogued shaders. Their counts and missing entries are still logged.
void HealthyLaunchIsQuiet()
{
    const Bytes b = Blob(30);
    Ledger l;
    for (size_t i = 0; i < 6; ++i)
        NoteEntry(l, Catalog::Ui, i == 5 ? 6 : i, 1000 + uint32_t(i)); // the pinned six (ui5 is scene-dependent)
    for (size_t i = 0; i < 5; ++i)
        if (i != 2) // scene stage 3 (lut3) never appears
            NoteEntry(l, Catalog::Scene, i, 3000);
    for (size_t i = 0; i < 9; ++i)
        if (i != 7) // the chara finisher is not created yet
            NoteEntry(l, Catalog::Chara, i, 4000);
    for (uint32_t i = 0; i < 300; ++i)
        l.NotePixelShader(b.data(), b.size(), 0x60000000u + i, 10 + i);
    Presents(l, 700);
    CHECK(l.UiHealthy());

    const auto lines = l.Poll(40000);
    const std::string text = Joined(lines);
    CHECK_HAS(text, "scene 4/5");
    CHECK_HAS(text, "chara 8/9");
    CHECK_HAS(text, "UI boundary ok");
    CHECK_HAS(text, "scene #2 (lut3) crc=0x27ebc404 missing");
    CHECK_HAS(text, "chara #7 (finisher) crc=0xfdb2f4fc missing");
    CHECK_HAS(text, "UI #5 (ui5) crc=0xe95deec4 missing (within the healthy baseline of 6)"); // UI keeps its baseline, ui5 is the one missing
    for (const auto& s : lines)
        if (s.find("scene #2") != std::string::npos || s.find("chara #7") != std::string::npos)
            CHECK(s.find("healthy baseline") == std::string::npos); // no baseline is claimed for them
    CHECK_EQ(CountLines(lines, "below the healthy baseline"), 0u);
    CHECK_EQ(CountLines(lines, "near-miss:"), 0u);
    CHECK_EQ(CountLines(lines, "uncatalogued #"), 0u);
    CHECK(l.UncatalogedRecorded() > 0); // the sample was taken; it is just not dumped

    // The same with the scene and the chara catalogs still completely empty (an early session): quiet as long as UI is healthy.
    Ledger e;
    for (size_t i = 0; i < 6; ++i)
        NoteEntry(e, Catalog::Ui, i == 5 ? 6 : i, 1000); // the pinned six (ui5 is scene-dependent)
    for (uint32_t i = 0; i < 40; ++i)
        e.NotePixelShader(b.data(), b.size(), 0x61000000u + i, 10 + i);
    Presents(e, 700);
    const auto early = e.Poll(40000);
    CHECK_HAS(Joined(early), "scene 0/5");
    CHECK_EQ(CountLines(early, "below the healthy baseline"), 0u);
    CHECK_EQ(CountLines(early, "uncatalogued #"), 0u);

    // UI below 6 still dumps, and only UI is named, whatever scene and chara hold.
    Ledger u;
    for (size_t i = 0; i < 5; ++i)
        NoteEntry(u, Catalog::Ui, i, 1000);
    NoteAll(u, Catalog::Scene, 1);
    for (uint32_t i = 0; i < 40; ++i)
        u.NotePixelShader(b.data(), b.size(), 0x62000000u + i, 10 + i);
    Presents(u, 700);
    const auto bad = u.Poll(40000);
    CHECK_HAS(Joined(bad), "near-miss: UI 5/7, required set of 6 incomplete; shaders created");
    CHECK_EQ(CountLines(bad, "required set of 6 incomplete"), 1u);
    CHECK(CountLines(bad, "uncatalogued #") > 0);
}

// ---- slot capacity -----------------------------------------------------------------------------------------------------------------
// Synthetic tables for the capacity rule: `counts[c]` entries in catalog c, every entry with its own CRC (no two entries share one). The last\n// catalog is a vertex shader catalog, like the nameplate one.
struct SynthTables
{
    Entry entries[kCatalogCount][40];
    CatalogInfo info[kCatalogCount];
    SynthTables(const size_t (&counts)[kCatalogCount])
    {
        for (size_t c = 0; c < kCatalogCount; ++c)
        {
            for (size_t i = 0; i < 40; ++i)
                entries[c][i] = Entry { 0x70000000u + uint32_t(c) * 1000u + uint32_t(i), "e" };
            info[c] = CatalogInfo { "synthetic", entries[c], counts[c], 0, c == kCatalogCount - 1 };
        }
    }
};

void SlotCapacity()
{
    // The real tables fit (also asserted at compile time in CatalogLedger.h).
    static_assert(LedgerFits(kInfo));
    static_assert(kLedgerMaxSlots == 48 && kLedgerMaxEntriesPerCatalog == 32);
    Ledger real;
    CHECK(real.Fits());

    // The rule, from both sides.
    const size_t exact[kCatalogCount] { 8, 8, 8, 8, 8, 8 };       // 48 = the capacity
    const size_t over[kCatalogCount] { 9, 8, 8, 8, 8, 8 };        // 49
    const size_t wide[kCatalogCount] { 32, 16, 0, 0, 0, 0 };      // 32 in one catalog (a full mask word) and 48 in all
    const size_t tooWide[kCatalogCount] { 33, 1, 1, 1, 1, 1 };    // 38 in all, but a 33 entry catalog has no bit for its last entry
    const size_t tooWide2[kCatalogCount] { 1, 1, 1, 1, 1, 40 };
    const size_t empty[kCatalogCount] { 0, 0, 0, 0, 0, 0 };
    CHECK(LedgerFits(SynthTables(exact).info));
    CHECK(!LedgerFits(SynthTables(over).info));
    CHECK(LedgerFits(SynthTables(wide).info));
    CHECK(!LedgerFits(SynthTables(tooWide).info));
    CHECK(!LedgerFits(SynthTables(tooWide2).info));
    CHECK(LedgerFits(SynthTables(empty).info));

    // A table that fits exactly is fully usable: every entry of the last slot gets its record and its bit.
    {
        const SynthTables t(exact);
        Ledger l(t.info);
        CHECK(l.Fits());
        for (size_t c = 0; c < kCatalogCount; ++c)
            for (size_t i = 0; i < 8; ++i)
                if (c == kCatalogCount - 1)
                    l.NoteVertexShader(g_zeros.data(), 100, t.entries[c][i].crc, 7);
                else
                    l.NotePixelShader(g_zeros.data(), 100, t.entries[c][i].crc, 7);
        for (size_t c = 0; c < kCatalogCount; ++c)
        {
            CHECK_EQ(l.Mask(static_cast<Catalog>(c)), 0xFFu);
            for (size_t i = 0; i < 8; ++i)
                CHECK_EQ(l.SlotAt(static_cast<Catalog>(c), i).ready.load(), 1u);
        }
        CHECK(l.UiHealthy() == UiCatalogHealthy(0xFFu)); // not inert: the normal rule
    }
    {
        const SynthTables t(wide); // bit 31 of the first catalog
        Ledger l(t.info);
        CHECK(l.Fits());
        for (size_t i = 0; i < 32; ++i)
            l.NotePixelShader(g_zeros.data(), 100, t.entries[0][i].crc, 7);
        CHECK_EQ(l.Mask(static_cast<Catalog>(0)), 0xFFFFFFFFu);
        CHECK_EQ(l.SlotAt(static_cast<Catalog>(0), 31).ready.load(), 1u);
    }

    // A table that does not fit makes the ledger inert: nothing is counted, recorded or fingerprinted (so nothing can be written outside the slot
    // array), the Presents still count, the guard fails open, and the log says so once.
    for (const auto* counts : { &over, &tooWide, &tooWide2 })
    {
        const SynthTables t(*counts);
        Ledger l(t.info);
        CHECK(!l.Fits());
        for (size_t c = 0; c < kCatalogCount; ++c)
            for (size_t i = 0; i < (*counts)[c]; ++i)
            {
                l.NotePixelShader(g_zeros.data(), 100, t.entries[c][i].crc, 7);
                l.NoteVertexShader(g_zeros.data(), 100, t.entries[c][i].crc, 7);
            }
        l.NotePixelShader(g_zeros.data(), 100, 0x12345678u, 8); // a shader in no catalog
        l.NoteLayoutSeen();
        for (size_t c = 0; c < kCatalogCount; ++c)
            CHECK_EQ(l.Mask(static_cast<Catalog>(c)), 0u);
        CHECK_EQ(l.FingerprintsComputed(), 0u);
        CHECK_EQ(l.UncatalogedRecorded(), 0u);
        CHECK_EQ(l.PixelShadersSeen(), 0u);
        CHECK(l.UiHealthy()); // fail open, though no UI shader was counted
        Presents(l, 700);
        CHECK_EQ(l.Present(), 700u);
        const auto lines = l.Poll(40000);
        CHECK_EQ(lines.size(), 1u);
        CHECK_HAS(Joined(lines), "[ffxiv-catalog] disabled: the catalog tables do not fit the ledger");
        CHECK(l.Poll(41000).empty()); // said once
    }
}

// ---- reference fingerprints: the near-miss search over EVERY uncatalogued shader ------------------------------------------------------
// A copy of the real tables with reference fingerprints set on chosen entries (the real ones are all 0 until a game run has been logged).
struct RefTables
{
    CatalogInfo info[kCatalogCount];
    Entry entries[kCatalogCount][32];
    RefTables()
    {
        std::memcpy(info, kInfo, sizeof info);
        for (size_t c = 0; c < kCatalogCount; ++c)
        {
            for (size_t i = 0; i < kInfo[c].count; ++i)
                entries[c][i] = kInfo[c].entries[i];
            info[c].entries = entries[c];
        }
    }
    void Ref(Catalog c, size_t i, uint64_t fp) { entries[Index(c)][i].refFingerprint = fp; }
};

uint64_t HashOf(const Bytes& b) { return Fingerprint(b.data(), b.size()).hash; }

// The shader a patch recompiled is created LATE, after thousands of init-time shaders: far beyond the first 256 distinct CRCs that the
// below-baseline dump keeps. It is still found, because once a reference exists every uncatalogued shader is compared at creation.
void ReferenceLateMatch()
{
    const Bytes recompiled = Blob(123); // what UI #5 became
    const Bytes other = Blob(60);
    RefTables t;
    t.Ref(Catalog::Ui, 5, HashOf(recompiled));

    Ledger l(t.info);
    for (size_t i = 0; i < 7; ++i)
        if (i != 5) // the pinned six: healthy, so the below-baseline dump is NOT what finds the candidate
            NoteEntry(l, Catalog::Ui, i, 100 + uint32_t(i));
    for (uint32_t i = 0; i < 1000; ++i) // init-time shaders: 1000 distinct CRCs, none matches
        l.NotePixelShader(other.data(), other.size(), 0x80000000u + i, 200 + i);
    l.NotePixelShader(recompiled.data(), recompiled.size(), 0xDEAD0001u, 5000); // created late, the 1001st distinct CRC
    for (uint32_t i = 0; i < 50; ++i)                                         // and shaders after it
        l.NotePixelShader(other.data(), other.size(), 0x81000000u + i, 6000 + i);
    Presents(l, 700);
    CHECK(l.UiHealthy());

    const auto first = l.Poll(40000);
    const std::string text = Joined(first);
    CHECK_EQ(CountLines(first, "candidate crc=0x"), 1u);
    CHECK_HAS(text, "UI #5 (ui5) missing; candidate crc=0xdead0001 same fingerprint");
    CHECK_HAS(text, "reference matches kept 1/16 dropped 0"); // in the summary line
    CHECK_HAS(text, "created #1007"); // 6 UI entries, then 1000 init-time shaders, then this one
    CHECK_HAS(text, "UI #5 (ui5) crc=0xe95deec4 missing (within the healthy baseline of 6); reference fp=");
    CHECK_HAS(text, ", candidates follow");
    CHECK_EQ(CountLines(first, "uncatalogued #"), 0u); // healthy: no dump
    // it was found by the comparison at creation, not by the first-256 sample (which is full of init-time shaders)
    CHECK_EQ(l.UncatalogedRecorded(), kUncatalogedCap);
    for (size_t k = 0; k < kUncatalogedCap; ++k)
        CHECK(l.UncatalogedAt(k).crc != 0xDEAD0001u);

    // A match that shows up AFTER the settle point, with no catalog count changing (a zone load, the lobby): logged on the next Poll, once.
    CHECK(l.Poll(41000).empty());
    l.NotePixelShader(recompiled.data(), recompiled.size(), 0xDEAD0004u, 50000);
    const auto later = l.Poll(51000);
    CHECK_EQ(CountLines(later, "candidate crc=0x"), 1u);
    CHECK_HAS(Joined(later), "UI #5 (ui5) missing; candidate crc=0xdead0004 same fingerprint");
    CHECK(l.Poll(52000).empty());
    // the same shader created again, and again: not named again
    for (int i = 0; i < 3; ++i)
        l.NotePixelShader(recompiled.data(), recompiled.size(), 0xDEAD0001u, 60000 + i);
    CHECK(l.Poll(61000).empty());
}

// Zero references (today): nothing of the search runs, and the sample behaves exactly as before.
void ReferenceZeroUnchanged()
{
    static_assert(kCandidateCap == 16);
    Ledger l;
    CHECK_EQ(l.ReferenceCount(), 0u);
    const Bytes blob = Blob(60);
    for (uint32_t i = 0; i < 600; ++i)
        l.NotePixelShader(blob.data(), blob.size(), 0x20000000u + i, 2 + i);
    CHECK_EQ(l.UncatalogedRecorded(), kUncatalogedCap);
    CHECK_EQ(l.FingerprintsComputed(), kUncatalogedCap); // only the sample is fingerprinted, never more than the cap
    CHECK_EQ(l.CandidatesKept(), 0u);
    CHECK_EQ(l.CandidatesDropped(), 0u);

    // A reference in a VERTEX shader catalog (nameplate) cannot match a pixel shader: the search stays off.
    RefTables t;
    t.Ref(Catalog::Nameplate, 1, HashOf(blob));
    Ledger v(t.info);
    CHECK_EQ(v.ReferenceCount(), 0u);
    for (uint32_t i = 0; i < 600; ++i)
        v.NotePixelShader(blob.data(), blob.size(), 0x20000000u + i, 2 + i);
    CHECK_EQ(v.FingerprintsComputed(), kUncatalogedCap);
    CHECK_EQ(v.CandidatesKept(), 0u);
    // ... and the missing line says the vertex catalog is not searched instead of "matches no uncatalogued shader"
    v.NoteLayoutSeen();
    Presents(v, 700);
    const std::string text = Joined(v.Poll(40000));
    CHECK_HAS(text, "nameplate #1 (vs-layout4) crc=0x");
    CHECK_HAS(text, "(vertex shader catalog: not searched)");
    CHECK(text.find("reference matches") == std::string::npos); // the summary says nothing about a search that is off
}

// With a reference, EVERY uncatalogued shader is fingerprinted at creation (and counted), the sample still holds the first 256 for the dump.
void ReferenceFingerprintsEveryShader()
{
    const Bytes blob = Blob(60);
    const Bytes recompiled = Blob(123);
    RefTables t;
    t.Ref(Catalog::Ui, 3, HashOf(recompiled));
    Ledger l(t.info);
    CHECK_EQ(l.ReferenceCount(), 1u);
    for (uint32_t i = 0; i < 600; ++i)
        l.NotePixelShader(blob.data(), blob.size(), 0x20000000u + i, 2 + i);
    CHECK_EQ(l.FingerprintsComputed(), 600u);           // not capped at 256
    CHECK_EQ(l.UncatalogedRecorded(), kUncatalogedCap); // the sample is as before
    CHECK_EQ(l.UncatalogedAt(0).fpHash, HashOf(blob));  // and reuses the fingerprint (one computation per creation, not two)
    CHECK_EQ(l.CandidatesKept(), 0u);                   // nothing matched, nothing is kept
    // a repeated shader is fingerprinted at each creation (0.7 microseconds), still no allocation (see ReferenceNoAllocations)
    for (int i = 0; i < 100; ++i)
        l.NotePixelShader(blob.data(), blob.size(), 0x20000000u, 9);
    CHECK_EQ(l.FingerprintsComputed(), 700u);
    // a catalog hit costs one fingerprint, the first time only, as before
    NoteEntry(l, Catalog::Ui, 0, 5);
    NoteEntry(l, Catalog::Ui, 0, 6);
    CHECK_EQ(l.FingerprintsComputed(), 701u);
}

// Only a MATCH is kept: a planted matching shader is retained, others are not; a reference on any entry of any pixel catalog is compared.
void ReferenceMatchesOnly()
{
    const Bytes blob = Blob(60);
    const Bytes uiNew = Blob(123);
    const Bytes charaNew = Blob(77);
    RefTables t;
    t.Ref(Catalog::Ui, 5, HashOf(uiNew));
    t.Ref(Catalog::Chara, 7, HashOf(charaNew)); // a later entry of a later catalog: the whole list is searched, not its first element
    Ledger l(t.info);
    CHECK_EQ(l.ReferenceCount(), 2u);
    for (size_t i = 0; i < 7; ++i)
        if (i != 5) // all but ui5 (the reference sits on ui5 now)
            NoteEntry(l, Catalog::Ui, i, 10);
    for (size_t i = 0; i < 9; ++i)
        if (i != 7)
            NoteEntry(l, Catalog::Chara, i, 11);
    for (uint32_t i = 0; i < 300; ++i)
        l.NotePixelShader(blob.data(), blob.size(), 0x30000000u + i, 20);
    l.NotePixelShader(g_zeros.data(), 100, 0x31000000u, 21); // not a DXBC: never a candidate
    CHECK_EQ(l.CandidatesKept(), 0u);
    l.NotePixelShader(charaNew.data(), charaNew.size(), 0xCAFE0002u, 30);
    CHECK_EQ(l.CandidatesKept(), 1u);
    l.NotePixelShader(uiNew.data(), uiNew.size(), 0xCAFE0001u, 31);
    CHECK_EQ(l.CandidatesKept(), 2u);
    for (uint32_t i = 0; i < 300; ++i)
        l.NotePixelShader(blob.data(), blob.size(), 0x32000000u + i, 40);
    CHECK_EQ(l.CandidatesKept(), 2u);
    CHECK_EQ(l.CandidatesDropped(), 0u);
    // the record
    bool sawChara = false, sawUi = false;
    for (size_t k = 0; k < kCandidateCap; ++k)
        if (l.CandidateAt(k).ready.load() == 1)
        {
            const Candidate& c = l.CandidateAt(k);
            if (c.crc == 0xCAFE0002u)
                sawChara = c.fpHash == HashOf(charaNew) && c.ms == 30 && c.size == charaNew.size();
            if (c.crc == 0xCAFE0001u)
                sawUi = c.fpHash == HashOf(uiNew) && c.ms == 31;
        }
    CHECK(sawChara && sawUi);

    Presents(l, 700);
    const auto lines = l.Poll(40000);
    CHECK_EQ(CountLines(lines, "candidate crc=0x"), 2u);
    CHECK_HAS(Joined(lines), "UI #5 (ui5) missing; candidate crc=0xcafe0001 same fingerprint");
    CHECK_HAS(Joined(lines), "chara #7 (finisher) missing; candidate crc=0xcafe0002 same fingerprint");
    CHECK_EQ(CountLines(lines, "uncatalogued #"), 0u); // UI is healthy (the pinned six): no dump
}

// A match for an entry that EXISTS is not kept (it is not missing, and its siblings must not use up the candidate slots); it is kept when the
// entry does not exist yet at the time of creation.
void ReferencePresentEntryIsSkipped()
{
    const Bytes uiNew = Blob(123);
    RefTables t;
    t.Ref(Catalog::Ui, 3, HashOf(uiNew));
    Ledger l(t.info);
    NoteEntry(l, Catalog::Ui, 3, 5); // UI #3 exists
    for (uint32_t i = 0; i < 40; ++i)
        l.NotePixelShader(uiNew.data(), uiNew.size(), 0xAB000000u + i, 6); // 40 siblings of the same structure
    CHECK_EQ(l.CandidatesKept(), 0u);
    CHECK_EQ(l.CandidatesDropped(), 0u);

    Ledger m(t.info); // UI #3 missing at creation time: the siblings are candidates
    for (uint32_t i = 0; i < 3; ++i)
        m.NotePixelShader(uiNew.data(), uiNew.size(), 0xAB000000u + i, 6);
    CHECK_EQ(m.CandidatesKept(), 3u);
    NoteEntry(m, Catalog::Ui, 3, 7); // ... and then the entry shows up: nothing more is kept
    m.NotePixelShader(uiNew.data(), uiNew.size(), 0xAB000010u, 8);
    CHECK_EQ(m.CandidatesKept(), 3u);
    // a candidate kept before the entry appeared is not named once the entry exists
    Presents(m, 700);
    CHECK_EQ(CountLines(m.Poll(40000), "candidate crc=0x"), 0u);
}

// The candidate set holds 16 distinct CRCs, first come, nothing is overwritten; the rest is counted as dropped. Re-creation of a kept CRC
// does not use another slot.
void ReferenceCandidateCap()
{
    const Bytes uiNew = Blob(123);
    RefTables t;
    t.Ref(Catalog::Ui, 3, HashOf(uiNew));
    Ledger l(t.info);
    for (uint32_t i = 0; i < 40; ++i)
    {
        l.NotePixelShader(uiNew.data(), uiNew.size(), 0xDEAD1000u + i, 100 + i);
        l.NotePixelShader(uiNew.data(), uiNew.size(), 0xDEAD1000u + i, 200 + i); // created twice
    }
    CHECK_EQ(l.CandidatesKept(), 16u);
    CHECK_EQ(l.CandidatesDropped(), 2u * (40u - 16u)); // each of the 24 later CRCs found it full, twice
    for (size_t k = 0; k < kCandidateCap; ++k)
    {
        CHECK_EQ(l.CandidateAt(k).crc, 0xDEAD1000u + uint32_t(k)); // the first sixteen, in order, untouched
        CHECK_EQ(l.CandidateAt(k).ms, 100u + uint32_t(k));         // the record of the first creation, not of the second
    }
    Presents(l, 700);
    const auto lines = l.Poll(40000);
    CHECK_EQ(CountLines(lines, "candidate crc=0x"), 16u);
    CHECK_HAS(Joined(lines), "candidate crc=0xdead100f ");
    CHECK(Joined(lines).find("candidate crc=0xdead1010") == std::string::npos);
    CHECK(l.Poll(41000).empty());
}

// Same on a hot path: no heap allocation with references set (hits, misses, repeats, matches, a full candidate set).
void ReferenceNoAllocations()
{
    const Bytes blob = Blob(200);
    const Bytes uiNew = Blob(123);
    RefTables t;
    t.Ref(Catalog::Ui, 3, HashOf(uiNew));
    t.Ref(Catalog::Scene, 2, HashOf(blob)); // matches the plain uncatalogued shaders below too
    Ledger l(t.info);
    g_allocs = 0;
    g_count = true;
    for (uint32_t i = 0; i < 20000; ++i)
    {
        if (i % 4 == 0)
            l.NotePixelShader(blob.data(), blob.size(), 0x90000000u + i, i); // uncatalogued, matches the scene reference: candidates, then full
        else if (i % 4 == 1)
            l.NotePixelShader(uiNew.data(), uiNew.size(), 0xA0000000u + i, i);
        else if (i % 4 == 2)
            l.NotePixelShader(g_zeros.data(), 100, 0xB0000000u + i, i); // not a DXBC
        else
            NoteEntry(l, Catalog::Chara, i % 9, i);
        l.NotePresent();
    }
    g_count = false;
    CHECK_EQ(g_allocs, 0);
    CHECK_EQ(l.CandidatesKept(), 16u);
    CHECK(l.CandidatesDropped() > 0);
}

// Several threads create the same matching shaders: distinct CRCs, no duplicate slot, nothing lost below the cap.
void ReferenceThreads()
{
    const Bytes uiNew = Blob(123);
    RefTables t;
    t.Ref(Catalog::Ui, 3, HashOf(uiNew));
    Ledger l(t.info);
    std::atomic<bool> go { false };
    std::vector<std::thread> pool;
    for (int th = 0; th < 4; ++th)
        pool.emplace_back(
            [&]
            {
                while (!go.load())
                {
                }
                for (uint32_t round = 0; round < 50; ++round)
                    for (uint32_t i = 0; i < 12; ++i) // the same 12 CRCs from every thread, 50 times
                        l.NotePixelShader(uiNew.data(), uiNew.size(), 0xCC000000u + i, 100);
            });
    go = true;
    for (auto& th : pool)
        th.join();
    CHECK_EQ(l.CandidatesKept(), 12u);
    CHECK_EQ(l.CandidatesDropped(), 0u);
    for (size_t i = 0; i < kCandidateCap; ++i)
        for (size_t j = i + 1; j < kCandidateCap; ++j)
            if (l.CandidateAt(i).ready.load() == 1 && l.CandidateAt(j).ready.load() == 1 && l.CandidateAt(i).crc == l.CandidateAt(j).crc)
                CHECK(false); // a duplicated slot
}

// Several threads create shaders (the game creates them from loader threads): no bit is lost, the cap holds, no record is duplicated.
void Threads()
{
    Ledger l;
    std::atomic<bool> go { false };
    std::vector<std::thread> pool;
    for (int t = 0; t < 4; ++t)
        pool.emplace_back(
            [&, t]
            {
                while (!go.load())
                {
                }
                const Bytes blob = Blob(50 + uint32_t(t));
                for (uint32_t round = 0; round < 3; ++round)
                {
                    for (size_t c = 0; c < 5; ++c)
                        for (size_t i = 0; i < Info(static_cast<Catalog>(c)).count; ++i)
                        {
                            const Entry& e = Info(static_cast<Catalog>(c)).entries[i];
                            l.NotePixelShader(e.bytes ? static_cast<const void*>(g_zeros.data()) : blob.data(), e.bytes ? e.bytes : blob.size(), e.crc,
                                              100 + t);
                        }
                    for (size_t i = 0; i < 3; ++i)
                        l.NoteVertexShader(blob.data(), blob.size(), Info(Catalog::Nameplate).entries[i].crc, 100 + t);
                    for (uint32_t i = 0; i < 400; ++i)
                        l.NotePixelShader(blob.data(), blob.size(), 0x50000000u + (i * 7919u + uint32_t(t) * 13u) % 600u, 200);
                    l.NotePresent();
                }
            });
    go = true;
    for (auto& th : pool)
        th.join();
    CHECK_EQ(l.Count(Catalog::Ui), 7u);
    CHECK_EQ(l.Count(Catalog::Scene), 5u);
    CHECK_EQ(l.Count(Catalog::Lighting), 9u);
    CHECK_EQ(l.Count(Catalog::Chara), 9u);
    CHECK_EQ(l.Count(Catalog::Companion), 4u);
    CHECK_EQ(l.Count(Catalog::Nameplate), 3u);
    CHECK_EQ(l.UncatalogedRecorded(), kUncatalogedCap);
    CHECK_EQ(l.Present(), 12u);
    for (size_t i = 0; i < kUncatalogedCap; ++i)
        for (size_t j = i + 1; j < kUncatalogedCap; ++j)
            if (l.UncatalogedAt(i).crc == l.UncatalogedAt(j).crc)
                CHECK(false); // a duplicated record
    // every catalog slot is complete once its bit is visible
    for (size_t c = 0; c < kCatalogCount; ++c)
        for (size_t i = 0; i < Info(static_cast<Catalog>(c)).count; ++i)
            CHECK_EQ(l.SlotAt(static_cast<Catalog>(c), i).ready.load(), 1u);
}
} // namespace

// A session holding ui5 instead of ui3 (6 of 7, but not the pinned set) is FALLBACK at the log level: the boundary
// names the pinned set, the missing-ui3 line claims no healthy baseline, and the uncatalogued dump still runs (a 6/7
// session missing a required shader is not a healthy one). The count rule alone would call it healthy.
void Ui5WithoutUi3FallbackDump()
{
    Ledger l;
    for (size_t i = 0; i < 7; ++i)
        if (i != 3) // ui5 without ui3: 6 of 7, still FALLBACK (not the pinned set)
            NoteEntry(l, Catalog::Ui, i, 1000 + uint32_t(i));
    CHECK(!l.UiHealthy());
    const Bytes b = Blob(30);
    for (uint32_t i = 0; i < 40; ++i)
        l.NotePixelShader(b.data(), b.size(), 0x63000000u + i, 10 + i);
    Presents(l, 700);
    const auto lines = l.Poll(40000);
    const std::string text = Joined(lines);
    CHECK_HAS(text, "UI boundary FALLBACK (UI 6/7, needs the pinned set: HUD-less reports NoBoundaryThisFrame until it is complete)");
    CHECK_HAS(text, "UI #3 (ui3) crc=0x712eb335 missing");
    for (const auto& s : lines)
        if (s.find("UI #3 (ui3)") != std::string::npos)
            CHECK(s.find("within the healthy baseline") == std::string::npos); // the missing required shader claims no baseline
    CHECK_HAS(text, "near-miss: UI 6/7, required set of 6 incomplete; shaders created");
    CHECK(CountLines(lines, "uncatalogued #") > 0); // below the pinned set still dumps
}

int main()
{
    Fresh();
    CountsPerCatalog();
    Overlaps();
    Namespaces();
    FirstSeen();
    UiHealth();
    UncatalogedSample();
    NoAllocations();
    Settling();
    LogLines();
    NameplateNotes();
    NearMiss();
    DumpIsCapped();
    HealthyLaunchIsQuiet();
    Ui5WithoutUi3FallbackDump();
    SlotCapacity();
    ReferenceLateMatch();
    ReferenceZeroUnchanged();
    ReferenceFingerprintsEveryShader();
    ReferenceMatchesOnly();
    ReferencePresentEntryIsSkipped();
    ReferenceCandidateCap();
    ReferenceNoAllocations();
    ReferenceThreads();
    Threads();
    return Finish("catalog_ledger");
}
