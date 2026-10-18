#include "check.h"
#include "ext/ffxiv/chara/CharaProbeRules.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace ext::ffxiv::chara::probe;

namespace
{
constexpr TextureInfo kInfo { 3840, 2160, 87 };

// ---------------------------------------------------------------------------------------------------------------------
// Derived textures
// ---------------------------------------------------------------------------------------------------------------------

void TestChainAndHopCap()
{
    DerivedSet set;
    CHECK(set.Empty(1000));
    CHECK(set.FromPreview(0, 0x100, 0xA, kInfo, false, DerivedKind::Copy, 1000) == DeriveResult::Added);
    const Derived* a = set.Find(0xA, 1000);
    CHECK(a && a->hops == 1 && a->sourceEntry == 0 && a->kind == DerivedKind::Copy && a->firstSeen == 1000);
    CHECK(a && a->info.width == 3840 && a->info.height == 2160 && a->info.format == 87);

    CHECK(set.FromDerived(0xA, 0xB, kInfo, false, DerivedKind::Copy, 1010) == DeriveResult::Added);
    CHECK(set.Find(0xB, 1010) && set.Find(0xB, 1010)->hops == 2 && set.Find(0xB, 1010)->sourceEntry == 0);
    CHECK(set.FromDerived(0xB, 0xC, kInfo, false, DerivedKind::Draw, 1020) == DeriveResult::Added);
    CHECK(set.Find(0xC, 1020) && set.Find(0xC, 1020)->hops == 3 && set.Find(0xC, 1020)->kind == DerivedKind::Draw);

    // the fourth hop is refused and nothing is recorded
    CHECK(set.FromDerived(0xC, 0xD, kInfo, false, DerivedKind::Copy, 1030) == DeriveResult::TooManyHops);
    CHECK(set.Find(0xD, 1030) == nullptr);
    CHECK(set.Count(1030) == 3);

    // the source entry is inherited along the chain
    DerivedSet other;
    CHECK(other.FromPreview(5, 0x500, 0xE, kInfo, false, DerivedKind::Copy, 0) == DeriveResult::Added);
    CHECK(other.FromDerived(0xE, 0xF, kInfo, false, DerivedKind::Copy, 1) == DeriveResult::Added);
    CHECK(other.Find(0xF, 1) && other.Find(0xF, 1)->sourceEntry == 5);
}

// ---------------------------------------------------------------------------------------------------------------------
// Which texture is it: references, recycled addresses (REVIEW-CHARA-2 M3)
// ---------------------------------------------------------------------------------------------------------------------

void TestHeldReferences()
{
    // The set keeps the texture alive while the entry lives, so its address cannot be handed to an unrelated texture.
    auto texture = std::make_shared<int>(1);
    std::weak_ptr<int> weak = texture;
    DerivedSet set;
    CHECK(set.FromPreview(0, 0x100, 0xA, kInfo, false, DerivedKind::Copy, 1000, texture) == DeriveResult::Added);
    texture.reset(); // the game let go of its reference
    CHECK(!weak.expired());

    // an entry that holds its texture is the texture: a different size at the same address is not a question (it cannot happen)
    const TextureInfo other { 64, 64, 28 };
    CHECK(set.Find(0xA, 1500, &other) != nullptr);

    // a sampler draw keeps it alive, so the reference stays
    CHECK(set.Touch(0xA, 3500));
    set.Prune(3500 + kExpiryMs - 1);
    CHECK(!weak.expired() && set.Find(0xA, 3500 + kExpiryMs - 1) != nullptr);

    // 3 s after the last sighting the entry is dead and Prune releases the texture
    set.Prune(3500 + kExpiryMs);
    CHECK(weak.expired());
    CHECK(set.Find(0xA, 3500 + kExpiryMs) == nullptr && set.Empty(3500 + kExpiryMs));

    // Clear releases every reference
    auto a = std::make_shared<int>(1), b = std::make_shared<int>(2);
    std::weak_ptr<int> wa = a, wb = b;
    set.FromPreview(0, 0x100, 0xA, kInfo, false, DerivedKind::Copy, 0, a);
    set.FromDerived(0xA, 0xB, kInfo, false, DerivedKind::Copy, 1, b);
    a.reset();
    b.reset();
    CHECK(!wa.expired() && !wb.expired());
    set.Clear();
    CHECK(wa.expired() && wb.expired());

    // a replaced entry (full set) releases its texture; only 16 are ever held
    DerivedSet full;
    std::vector<std::weak_ptr<int>> weaks;
    for (uintptr_t i = 0; i < DerivedSet::kCapacity; ++i)
    {
        auto t = std::make_shared<int>(static_cast<int>(i));
        weaks.push_back(t);
        full.FromPreview(0, 0x100, 0x1000 + i, kInfo, false, DerivedKind::Copy, 100 + i, t);
    }
    size_t alive = 0;
    for (auto& w : weaks)
        alive += w.expired() ? 0 : 1;
    CHECK(alive == DerivedSet::kCapacity);
    auto extra = std::make_shared<int>(99);
    CHECK(full.FromPreview(0, 0x100, 0x2000, kInfo, false, DerivedKind::Copy, 200, extra) == DeriveResult::Replaced);
    CHECK(weaks[0].expired()); // the oldest entry went, with its reference
    alive = 0;
    for (auto& w : weaks)
        alive += w.expired() ? 0 : 1;
    CHECK(alive == DerivedSet::kCapacity - 1);

    // deriving the same texture again with a new reference replaces the old one; without one the entry keeps its own
    DerivedSet again;
    auto first = std::make_shared<int>(1), second = std::make_shared<int>(2);
    std::weak_ptr<int> w1 = first, w2 = second;
    again.FromPreview(0, 0x100, 0xA, kInfo, false, DerivedKind::Copy, 0, first);
    first.reset();
    CHECK(again.FromPreview(0, 0x100, 0xA, kInfo, false, DerivedKind::Copy, 1) == DeriveResult::Refreshed);
    CHECK(!w1.expired());
    CHECK(again.FromPreview(0, 0x100, 0xA, kInfo, false, DerivedKind::Copy, 2, second) == DeriveResult::Refreshed);
    second.reset();
    CHECK(w1.expired() && !w2.expired());
}

void TestRecycledAddressWithoutReference()
{
    // A swapchain buffer is never held (it would block ResizeBuffers). Its entry is checked against the texture's actual size and
    // format family: a new texture at the same address with other dimensions is not the derived one.
    const TextureInfo screen { 1920, 1080, 87 };
    DerivedSet set;
    set.FromPreview(0, 0x100, 0xB, screen, false, DerivedKind::Copy, 1000); // no reference
    CHECK(set.Find(0xB, 1100) != nullptr);                                  // no size given: as before
    CHECK(set.Find(0xB, 1100, &screen) != nullptr);
    const TextureInfo typed { 1920, 1080, 91 };                              // sRGB flavour of the same family: the same texture
    CHECK(set.Find(0xB, 1100, &typed) != nullptr);
    const TextureInfo smaller { 960, 540, 87 };                              // a recycled address: another size
    CHECK(set.Find(0xB, 1100, &smaller) == nullptr);
    const TextureInfo taller { 1920, 1081, 87 };
    CHECK(set.Find(0xB, 1100, &taller) == nullptr);
    const TextureInfo otherFormat { 1920, 1080, 10 };
    CHECK(set.Find(0xB, 1100, &otherFormat) == nullptr);
    // a sampler draw on the recycled texture does not keep the stale entry alive either
    CHECK(!set.Touch(0xB, 3900, &smaller));
    CHECK(set.Find(0xB, 4000) == nullptr); // so it expired on time
    set.FromPreview(0, 0x100, 0xB, screen, false, DerivedKind::Copy, 5000);
    CHECK(set.Touch(0xB, 5200, &screen) == true);
}

void TestFormatFamilyAndWholeCopies()
{
    CHECK(SameFormatFamily(27, 28) && SameFormatFamily(28, 29) && SameFormatFamily(87, 90) && SameFormatFamily(87, 91));
    CHECK(SameFormatFamily(9, 10) && SameFormatFamily(23, 24));
    CHECK(!SameFormatFamily(28, 87) && !SameFormatFamily(10, 28) && !SameFormatFamily(87, 10));
    CHECK(SameFormatFamily(41, 41) && !SameFormatFamily(41, 40)); // other formats: equal only to themselves

    const TextureInfo image { 576, 960, 87 };
    CopyFacts whole;
    whole.source = whole.dest = image;
    CHECK(WholeImageCopy(whole));                                      // CopyResource of the whole image
    whole.via = CopyVia::CopyRegion;
    CHECK(WholeImageCopy(whole));                                      // CopySubresourceRegion without box and offsets
    whole.via = CopyVia::Resolve;
    CHECK(WholeImageCopy(whole));

    // a partial CopySubresourceRegion never derives the destination (an atlas, a mip, an array slice)
    CopyFacts region = whole;
    region.via = CopyVia::CopyRegion;
    for (int what = 0; what < 6; ++what)
    {
        CopyFacts f = region;
        switch (what)
        {
        case 0: f.destSub = 1; break;
        case 1: f.sourceSub = 1; break;
        case 2: f.x = 8; break;
        case 3: f.y = 8; break;
        case 4: f.z = 1; break;
        case 5: f.hasBox = true; break;
        }
        CHECK(!WholeImageCopy(f));
    }
    // ... nor does a copy into a texture of another size or format
    CopyFacts bigger = whole;
    bigger.via = CopyVia::CopyResource;
    bigger.dest = { 1024, 1024, 87 };
    CHECK(!WholeImageCopy(bigger));
    bigger.dest = { 576, 961, 87 };
    CHECK(!WholeImageCopy(bigger));
    bigger.dest = { 576, 960, 28 };
    CHECK(!WholeImageCopy(bigger));
    bigger.dest = { 576, 960, 90 }; // typeless flavour of the same family: a whole copy
    CHECK(WholeImageCopy(bigger));
    CopyFacts empty;
    CHECK(!WholeImageCopy(empty)); // unknown sizes
    // a resolve of another subresource, or into another size, is partial; the format argument can make the families agree
    CopyFacts resolve = whole;
    resolve.via = CopyVia::Resolve;
    resolve.destSub = 2;
    CHECK(!WholeImageCopy(resolve));
    resolve.destSub = 0;
    resolve.source.format = 91;
    resolve.dest.format = 28; // other family, but the resolve format names the destination's family
    resolve.resolveFormat = 29;
    CHECK(WholeImageCopy(resolve));
    resolve.resolveFormat = 87;
    CHECK(!WholeImageCopy(resolve));
}

PromotionFacts Promotable()
{
    PromotionFacts f;
    f.rendererRecent = true;
    f.haveTarget = true;
    f.pixelShaderBound = true;
    f.target = { 576, 960, 87 };
    f.backbufferKnown = true;
    f.backbuffer = { 1920, 1080, 87 };
    return f;
}

void TestPromotion()
{
    CHECK(DecidePromotion(Promotable()) == Promotion::Promote);

    PromotionFacts f = Promotable();
    f.rendererRecent = false;
    CHECK(DecidePromotion(f) == Promotion::RendererIdle);
    f = Promotable();
    f.haveTarget = false;
    CHECK(DecidePromotion(f) == Promotion::NoTarget);
    f = Promotable();
    f.targetIsBackbuffer = true;
    CHECK(DecidePromotion(f) == Promotion::IsBackbuffer);
    f = Promotable();
    f.targetIsTableEntry = true;
    CHECK(DecidePromotion(f) == Promotion::IsTableEntry);

    // a stale binding: the draw has no pixel shader, so nothing it did read the bound preview (M5)
    f = Promotable();
    f.pixelShaderBound = false;
    CHECK(DecidePromotion(f) == Promotion::NoPixelShader);

    // a screen-size intermediate is judged against the backbuffer's own size, not a 4K constant (M7)
    f = Promotable();
    f.target = { 1920, 1080, 87 };
    CHECK(DecidePromotion(f) == Promotion::ScreenSizeIntermediate); // the whole screen
    f.target = { 1920, 540, 87 };
    CHECK(DecidePromotion(f) == Promotion::ScreenSizeIntermediate); // exactly half the area
    f.target = { 1920, 539, 87 };
    CHECK(DecidePromotion(f) == Promotion::Promote);                // just under half
    f.backbuffer = { 2560, 1440, 87 };                              // 2.07 M of 3.69 M pixels: more than half
    f.target = { 1920, 1080, 87 };
    CHECK(DecidePromotion(f) == Promotion::ScreenSizeIntermediate);
    f.target = { 1280, 720, 87 };
    CHECK(DecidePromotion(f) == Promotion::Promote);
    // a 1080p game with a 1920x1080 intermediate was invisible to the old 3000x1700 rule
    f = Promotable();
    f.target = { 1920, 1080, 87 };
    CHECK(DecidePromotion(f) != Promotion::Promote);

    // backbuffer unknown: the draw's viewport is the reference; no viewport at all: nothing can be said, nothing is promoted
    f = Promotable();
    f.backbufferKnown = false;
    f.viewportWidth = 1920;
    f.viewportHeight = 1080;
    CHECK(DecidePromotion(f) == Promotion::Promote);                // 576x960 is under half of the viewport
    f.target = { 1920, 1080, 87 };
    CHECK(DecidePromotion(f) == Promotion::ScreenSizeIntermediate);
    f.viewportWidth = f.viewportHeight = 0;
    CHECK(DecidePromotion(f) == Promotion::NoReference);

    // the same rule annotates the copy line
    CHECK(IsScreenSizeIntermediate({ 1920, 1080, 87 }, { 1920, 1080, 87 }));
    CHECK(!IsScreenSizeIntermediate({ 576, 960, 87 }, { 1920, 1080, 87 }));
    CHECK(!IsScreenSizeIntermediate({ 1920, 1080, 87 }, { 0, 0, 0 })); // backbuffer unknown: no annotation
}

void TestRefusals()
{
    DerivedSet set;
    // destination == source
    CHECK(set.FromPreview(0, 0x100, 0x100, kInfo, false, DerivedKind::Copy, 0) == DeriveResult::SameTexture);
    CHECK(set.Empty(0));
    CHECK(set.FromPreview(0, 0x100, 0xA, kInfo, false, DerivedKind::Copy, 0) == DeriveResult::Added);
    CHECK(set.FromDerived(0xA, 0xA, kInfo, false, DerivedKind::Copy, 1) == DeriveResult::SameTexture);
    CHECK(set.Find(0xA, 1)->hops == 1); // unchanged

    // a preview entry never becomes derived
    CHECK(set.FromPreview(0, 0x100, 0x200, kInfo, true, DerivedKind::Copy, 2) == DeriveResult::DestIsPreview);
    CHECK(set.FromDerived(0xA, 0x200, kInfo, true, DerivedKind::Copy, 2) == DeriveResult::DestIsPreview);
    CHECK(set.Find(0x200, 2) == nullptr);

    // null pointers
    CHECK(set.FromPreview(0, 0, 0xB, kInfo, false, DerivedKind::Copy, 3) == DeriveResult::NullPointer);
    CHECK(set.FromPreview(0, 0x100, 0, kInfo, false, DerivedKind::Copy, 3) == DeriveResult::NullPointer);
    CHECK(set.FromDerived(0, 0xB, kInfo, false, DerivedKind::Copy, 3) == DeriveResult::NullPointer);
    CHECK(set.Find(0, 3) == nullptr);

    // entries outside 0..7 are not preview outputs; a source that is not derived is unknown
    CHECK(set.FromPreview(8, 0x100, 0xB, kInfo, false, DerivedKind::Copy, 4) == DeriveResult::UnknownSource);
    CHECK(set.FromPreview(-1, 0x100, 0xB, kInfo, false, DerivedKind::Copy, 4) == DeriveResult::UnknownSource);
    CHECK(set.FromPreview(7, 0x100, 0xB, kInfo, false, DerivedKind::Copy, 4) == DeriveResult::Added);
    CHECK(set.FromDerived(0x999, 0xC, kInfo, false, DerivedKind::Copy, 4) == DeriveResult::UnknownSource);
    CHECK(set.Find(0xC, 4) == nullptr);

    CHECK(Accepted(DeriveResult::Added) && Accepted(DeriveResult::Refreshed) && Accepted(DeriveResult::Replaced));
    CHECK(!Accepted(DeriveResult::TooManyHops) && !Accepted(DeriveResult::CopiesOff) && !Accepted(DeriveResult::SameTexture));
}

void TestExpiryAndRefresh()
{
    DerivedSet set;
    CHECK(set.FromPreview(0, 0x100, 0xA, kInfo, false, DerivedKind::Copy, 1000) == DeriveResult::Added);
    CHECK(set.Find(0xA, 3999) != nullptr); // 2999 ms after lastSeen: alive
    CHECK(!set.Empty(3999));
    CHECK(set.Find(0xA, 4000) == nullptr); // 3000 ms: expired
    CHECK(set.Empty(4000));
    CHECK(set.Count(4000) == 0);

    // deriving again refreshes the time, and it counts from the refresh
    CHECK(set.FromPreview(0, 0x100, 0xA, kInfo, false, DerivedKind::Copy, 3500) == DeriveResult::Refreshed);
    CHECK(set.Find(0xA, 6499) != nullptr);
    CHECK(set.Find(0xA, 6500) == nullptr);
    CHECK(set.Find(0xA, 6499)->firstSeen == 1000); // the first sighting is kept

    // a sampler draw can keep a texture alive
    CHECK(set.Touch(0xA, 6000));
    CHECK(set.Find(0xA, 8999) != nullptr);
    CHECK(!set.Touch(0xA, 9000)); // expired: not resurrected
    CHECK(!set.Touch(0x777, 0));

    // an expired source cannot feed a new copy
    DerivedSet chain;
    chain.FromPreview(0, 0x100, 0xA, kInfo, false, DerivedKind::Copy, 0);
    CHECK(chain.FromDerived(0xA, 0xB, kInfo, false, DerivedKind::Copy, 3000) == DeriveResult::UnknownSource);
    CHECK(chain.FromDerived(0xA, 0xB, kInfo, false, DerivedKind::Copy, 2999) == DeriveResult::Added);

    // a stale record of the same pointer is replaced, not duplicated
    DerivedSet stale;
    stale.FromPreview(0, 0x100, 0xA, kInfo, false, DerivedKind::Copy, 0);
    CHECK(stale.FromPreview(2, 0x100, 0xA, { 8, 8, 28 }, false, DerivedKind::Copy, 5000) == DeriveResult::Added);
    CHECK(stale.Live(5000).size() == 1);
    CHECK(stale.Find(0xA, 5000)->firstSeen == 5000 && stale.Find(0xA, 5000)->sourceEntry == 2 && stale.Find(0xA, 5000)->info.width == 8);

    // the shortest chain wins when the same texture is reached again
    DerivedSet shortest;
    shortest.FromPreview(0, 0x100, 0xA, kInfo, false, DerivedKind::Copy, 0);
    shortest.FromDerived(0xA, 0xB, kInfo, false, DerivedKind::Copy, 1);
    CHECK(shortest.Find(0xB, 1)->hops == 2);
    CHECK(shortest.FromPreview(0, 0x100, 0xB, kInfo, false, DerivedKind::Copy, 2) == DeriveResult::Refreshed);
    CHECK(shortest.Find(0xB, 2)->hops == 1);
    CHECK(shortest.FromDerived(0xA, 0xB, kInfo, false, DerivedKind::Copy, 3) == DeriveResult::Refreshed);
    CHECK(shortest.Find(0xB, 3)->hops == 1); // a longer route does not make it longer
    CHECK(shortest.Count(3) == 2);
}

void TestFullSet()
{
    DerivedSet set;
    for (uintptr_t i = 0; i < DerivedSet::kCapacity; ++i)
        CHECK(set.FromPreview(0, 0x100, 0x1000 + i, kInfo, false, DerivedKind::Copy, 100 + i) == DeriveResult::Added);
    CHECK(set.Count(120) == 16);
    // full: the oldest (smallest lastSeen) is replaced
    CHECK(set.FromPreview(0, 0x100, 0x2000, kInfo, false, DerivedKind::Copy, 200) == DeriveResult::Replaced);
    CHECK(set.Count(200) == 16);
    CHECK(set.Find(0x1000, 200) == nullptr);
    CHECK(set.Find(0x1001, 200) != nullptr);
    CHECK(set.Find(0x2000, 200) != nullptr);
    // refreshing the oldest protects it from the next replacement
    CHECK(set.Touch(0x1001, 210));
    CHECK(set.FromPreview(0, 0x100, 0x2001, kInfo, false, DerivedKind::Copy, 220) == DeriveResult::Replaced);
    CHECK(set.Find(0x1001, 220) != nullptr);
    CHECK(set.Find(0x1002, 220) == nullptr);
    // expired entries are reused before a live one is replaced
    CHECK(set.FromPreview(0, 0x100, 0x2002, kInfo, false, DerivedKind::Copy, 100 + 3000 + 50) == DeriveResult::Added);
    CHECK(set.Find(0x2000, 3150) != nullptr && set.Find(0x2001, 3150) != nullptr && set.Find(0x1001, 3150) != nullptr);
    CHECK(set.Find(0x2002, 3150) != nullptr);
    // sixteen slots, never more: flooding the set with distinct textures keeps exactly the most recent sixteen
    DerivedSet flood;
    for (uintptr_t i = 0; i < 40; ++i)
        flood.FromPreview(0, 0x100, 0x5000 + i, kInfo, false, DerivedKind::Copy, 100 + i);
    CHECK(flood.Count(140) == DerivedSet::kCapacity && flood.Live(140).size() == DerivedSet::kCapacity);
    for (uintptr_t i = 0; i < 40; ++i)
        CHECK((flood.Find(0x5000 + i, 140) != nullptr) == (i >= 40 - DerivedSet::kCapacity));
}

void TestCopiesSwitchAndClear()
{
    DerivedSet set;
    CHECK(set.CopiesEnabled());
    set.FromPreview(0, 0x100, 0xA, kInfo, false, DerivedKind::Copy, 0);
    set.SetCopiesEnabled(false);
    CHECK(!set.CopiesEnabled());
    CHECK(set.FromPreview(0, 0x100, 0xB, kInfo, false, DerivedKind::Copy, 1) == DeriveResult::CopiesOff);
    CHECK(set.FromDerived(0xA, 0xB, kInfo, false, DerivedKind::Copy, 1) == DeriveResult::CopiesOff);
    CHECK(set.Find(0xB, 1) == nullptr);
    CHECK(set.Find(0xA, 1) != nullptr); // what was found before stays visible
    set.SetCopiesEnabled(true);
    CHECK(set.FromPreview(0, 0x100, 0xB, kInfo, false, DerivedKind::Copy, 2) == DeriveResult::Added);
    set.Clear();
    CHECK(set.Empty(2) && set.Find(0xA, 2) == nullptr && set.Live(2).empty());
    CHECK(set.CopiesEnabled()); // Clear forgets textures, not the switch
    set.SetCopiesEnabled(false);
    set.Clear();
    CHECK(!set.CopiesEnabled()); // the `reset` command does not undo "copies off" ...
    set.SetCopiesEnabled(true);
    set.FromPreview(0, 0x100, 0xA, kInfo, false, DerivedKind::Copy, 5);
    set.SetCopiesEnabled(false);
    set.Reset();
    CHECK(set.CopiesEnabled() && set.Empty(5)); // ... switching the probe off and on does (REVIEW-CHARA-2 L5)
    CHECK(set.FromPreview(0, 0x100, 0xA, kInfo, false, DerivedKind::Copy, 6) == DeriveResult::Added);
}

// ---------------------------------------------------------------------------------------------------------------------
// Log limits
// ---------------------------------------------------------------------------------------------------------------------

void TestCaseLog()
{
    CaseLog<3> log;
    CHECK(log.Offer(11));
    CHECK(!log.Offer(11)); // the same case is logged once
    CHECK(log.Offer(22));
    CHECK(log.Offer(33));
    CHECK(log.Count() == 3);
    CHECK(!log.Offer(44)); // cap reached
    CHECK(!log.Offer(55));
    CHECK(log.Refused() == 2);
    CHECK(!log.Offer(11)); // an accepted case stays known and is not counted as refused
    CHECK(log.Refused() == 2);
    log.Clear();
    CHECK(log.Count() == 0 && log.Refused() == 0 && log.Offer(44));

    CHECK(kCopyCaseCap == 48 && kSamplerCaseCap == 64);
    CaseLog<kCopyCaseCap> copies;
    CaseLog<kSamplerCaseCap> samplers;
    size_t c = 0, s = 0;
    for (uint64_t i = 0; i < 200; ++i)
    {
        c += copies.Offer(CaseKey(i)) ? 1 : 0;
        s += samplers.Offer(CaseKey(i, 7)) ? 1 : 0;
    }
    CHECK(c == 48 && s == 64);

    CHECK(CaseKey(1, 2, 3) == CaseKey(1, 2, 3));
    CHECK(CaseKey(1, 2, 3) != CaseKey(1, 2, 4));
    CHECK(CaseKey(1, 2, 3) != CaseKey(3, 2, 1));
    CHECK(CaseKey(0) != CaseKey(0, 0, 0, 0, 1));
}

void TestRateCounter()
{
    RateCounter r;
    RateCounter::Window w;
    r.Add(0);
    r.Add(100, 2);
    r.Add(999);
    CHECK(!r.Take(w)); // the second is not over
    r.Roll(1000);
    CHECK(r.Take(w) && w.count == 4 && w.startMs == 0 && w.durationMs == 1000);
    CHECK(!r.Take(w));
    CHECK(r.Total() == 4);

    // a second without events produces nothing; the next window starts when the next event arrives
    r.Add(5000);
    CHECK(!r.Take(w));
    r.Add(6200);
    CHECK(r.Take(w) && w.count == 1 && w.startMs == 5000 && w.durationMs == 1200);

    // the queue keeps the latest 8 windows
    RateCounter many;
    for (uint64_t s = 0; s < 12; ++s)
        many.Add(s * 1000);
    many.Roll(12000);
    int n = 0;
    uint64_t first = 0;
    while (many.Take(w))
    {
        if (n++ == 0)
            first = w.startMs;
    }
    CHECK(n == 8 && first == 4000);
    CHECK(many.Total() == 12);

    many.Clear();
    CHECK(many.Total() == 0 && !many.Take(w));
}

void TestThrottledSwapCounter()
{
    ThrottledSwapCounter t;
    ThrottledSwapCounter::Line line;
    std::vector<uint32_t> seconds;
    for (uint64_t s = 0; s < 1000; ++s)
    {
        t.Add(s * 1000, 10);
        while (t.TakeLine(s * 1000, line))
        {
            CHECK(line.perSecond == 10);
            seconds.push_back(line.second);
            if (line.second == 1)
                CHECK(line.total == 10);
            if (line.second == 40)
                CHECK(line.total == 400); // skipped seconds still count
        }
    }
    CHECK(seconds.size() == ThrottledSwapCounter::kMaxLines);
    CHECK(seconds.size() == 60);
    bool ok = seconds.size() == 60;
    for (uint32_t i = 0; ok && i < 30; ++i)
        ok = seconds[i] == i + 1; // every second for the first 30
    CHECK(ok);
    CHECK(seconds[30] == 40 && seconds[31] == 50 && seconds[59] == 330); // then every 10th second
    CHECK(t.Lines() == 60);
    CHECK(t.Total() == 10000);

    // the rate is scaled to one second
    ThrottledSwapCounter slow;
    slow.Add(0, 10);
    CHECK(!slow.TakeLine(999, line));
    CHECK(slow.TakeLine(2000, line) && line.perSecond == 5 && line.second == 1);
}

void TestOutputBudget()
{
    OutputBudget b;
    CHECK(kMaxFiles == 40 && kMaxBytes == 300ull * 1024 * 1024);
    CHECK(b.Fits(0, 0));
    CHECK(b.Fits(40, kMaxBytes));
    CHECK(!b.Fits(41, 0));
    CHECK(!b.Fits(0, kMaxBytes + 1));
    b.Add(7, 1000);
    CHECK(b.files == 7 && b.bytes == 1000);
    CHECK(b.Fits(33, kMaxBytes - 1000));
    CHECK(!b.Fits(34, 0));                   // files
    CHECK(!b.Fits(1, kMaxBytes - 999));      // bytes
    CHECK(!b.Fits(1, UINT64_MAX));           // no overflow of the sum
    CHECK(!b.Fits(UINT32_MAX, 1));
    CHECK(!b.Fits(UINT32_MAX - 6, 0));

    // all or nothing: a set that does not fit changes nothing because the caller does not Add it
    OutputBudget full;
    full.Add(40, 0);
    CHECK(!full.Fits(1, 0) && full.Fits(0, 10));
    OutputBudget bytes;
    bytes.Add(1, kMaxBytes);
    CHECK(!bytes.Fits(1, 1) && bytes.Fits(1, 0));

    // saturating Add never wraps back to "empty"
    OutputBudget sat;
    sat.Add(UINT32_MAX, UINT64_MAX);
    sat.Add(5, 5);
    CHECK(sat.files == UINT32_MAX && sat.bytes == UINT64_MAX);
    CHECK(!sat.Fits(0, 0));
}

// ---------------------------------------------------------------------------------------------------------------------
// Command parser
// ---------------------------------------------------------------------------------------------------------------------

void TestToggles()
{
    struct
    {
        const char* word;
        CommandKind kind;
    } toggles[] = { { "tint", CommandKind::Tint },       { "flood", CommandKind::Flood }, { "aa", CommandKind::Aa },
                    { "dlss", CommandKind::Dlss },       { "upscale", CommandKind::Upscale }, { "copies", CommandKind::Copies } };
    for (const auto& t : toggles)
    {
        for (const char* on : { "on", "ON", "True", "1", "true" })
        {
            const Command c = ParseLine(std::string(t.word) + " " + on);
            CHECK(c.kind == t.kind && c.hasValue && c.value);
        }
        for (const char* off : { "off", "Off", "FALSE", "0", "false" })
        {
            const Command c = ParseLine(std::string(t.word) + " " + off);
            CHECK(c.kind == t.kind && c.hasValue && !c.value);
        }
        // a toggle needs its value
        CHECK(ParseLine(t.word).kind == CommandKind::Unknown);
        CHECK(!ParseLine(t.word).hasValue);
        CHECK(!ParseLine(t.word).note.empty());
        CHECK(ParseLine(std::string(t.word) + " maybe").kind == CommandKind::Unknown);
        CHECK(ParseLine(std::string(t.word) + " 2").kind == CommandKind::Unknown);
        CHECK(ParseLine(std::string(t.word) + " on off").kind == CommandKind::Unknown);
        CHECK(ParseLine(std::string(t.word) + " onn").kind == CommandKind::Unknown);
    }
    CHECK(ParseLine("TINT   ON").kind == CommandKind::Tint);
    CHECK(ParseLine("  \tFlood\toff \t\r").kind == CommandKind::Flood);
    CHECK(!ParseLine("  \tFlood\toff \t\r").value);
    CHECK(ParseLine("tint on # because").kind == CommandKind::Tint);
    CHECK(ParseLine("tint on # because").value);
    CHECK(ParseLine("tint #on").kind == CommandKind::Unknown); // the value is inside the comment
}

void TestPlainCommands()
{
    CHECK(ParseLine("dump").kind == CommandKind::Dump);
    CHECK(ParseLine("State").kind == CommandKind::State);
    CHECK(ParseLine("RESET").kind == CommandKind::Reset);
    CHECK(ParseLine("help\r").kind == CommandKind::Help);
    CHECK(!ParseLine("dump").hasValue);
    CHECK(ParseLine("dump # save everything").kind == CommandKind::Dump);
    CHECK(ParseLine("dump now").kind == CommandKind::Unknown);
    CHECK(ParseLine("reset on").kind == CommandKind::Unknown);
    CHECK(ParseLine("dumps").kind == CommandKind::Unknown);
    CHECK(ParseLine("tin on").kind == CommandKind::Unknown);

    CHECK(ParseLine("").kind == CommandKind::Empty);
    CHECK(ParseLine("   \t \r").kind == CommandKind::Empty);
    CHECK(ParseLine("# only a comment").kind == CommandKind::Empty);
    CHECK(ParseLine("   # indented comment").kind == CommandKind::Empty);

    const Command u = ParseLine("Frobnicate the thing");
    CHECK(u.kind == CommandKind::Unknown && u.text == "Frobnicate the thing" && !u.note.empty());
    CHECK(std::string(CommandName(CommandKind::Flood)) == "flood" && std::string(CommandName(CommandKind::Unknown)) == "unknown");
    CHECK(std::string(kHelpText).find("dump") != std::string::npos);
}

void TestGarbage()
{
    // NUL and control bytes anywhere make the line Unknown, and never reach the echo
    const std::string nul("tint\0on", 7);
    Command c = ParseLine(nul);
    CHECK(c.kind == CommandKind::Unknown);
    CHECK(c.text == "tinton"); // unprintable bytes are dropped from the echo
    CHECK(c.note.find("not printable") != std::string::npos);
    CHECK(c.text.find('\0') == std::string::npos);

    CHECK(ParseLine(std::string("dump\0", 5)).kind == CommandKind::Unknown);
    CHECK(ParseLine(std::string("\0", 1)).kind == CommandKind::Unknown);
    CHECK(ParseLine(std::string("\0 # comment", 11)).kind == CommandKind::Unknown);
    CHECK(ParseLine("tint\x01 on").kind == CommandKind::Unknown);
    CHECK(ParseLine("tint o\rn").kind == CommandKind::Unknown); // a carriage return in the middle is not a blank
    CHECK(ParseLine("t\xC3\xAFnt on").kind == CommandKind::Unknown);
    CHECK(ParseLine("\xFF\xFE").kind == CommandKind::Unknown);
    CHECK(ParseLine("\xFF\xFE").text.empty());
    CHECK(ParseLine("tint on\x7F").kind == CommandKind::Unknown); // DEL

    // the echo is at most 48 printable characters
    const std::string longLine(5000, 'x');
    c = ParseLine(longLine);
    CHECK(c.kind == CommandKind::Unknown && c.text.size() == kMaxEchoChars && kMaxEchoChars == 48);
    std::string mixed;
    for (int i = 0; i < 200; ++i)
        mixed += i % 2 ? "a" : "\x02";
    c = ParseLine(mixed);
    CHECK(c.text.size() == 48 && c.text.find('\x02') == std::string::npos);

    // every possible byte on its own: no crash, never a valid command
    for (int b = 0; b < 256; ++b)
    {
        const std::string s(1, static_cast<char>(b));
        const Command one = ParseLine(s);
        CHECK(one.kind == CommandKind::Unknown || one.kind == CommandKind::Empty);
        CHECK(one.text.size() <= 1);
    }
}

void TestParseFile()
{
    ParsedFile f = ParseFile("tint on\r\nFLOOD off\n\n# comment\n  dump  \nbogus\n");
    CHECK(f.commands.size() == 4 && f.lines == 6 && !f.truncatedBytes && !f.truncatedLines);
    CHECK(f.commands.size() == 4 && f.commands[0].kind == CommandKind::Tint && f.commands[0].value);
    CHECK(f.commands.size() == 4 && f.commands[1].kind == CommandKind::Flood && !f.commands[1].value);
    CHECK(f.commands.size() == 4 && f.commands[2].kind == CommandKind::Dump);
    CHECK(f.commands.size() == 4 && f.commands[3].kind == CommandKind::Unknown && f.commands[3].text == "bogus");

    CHECK(ParseFile("").commands.empty());
    CHECK(ParseFile("\n\n\n").commands.empty());
    CHECK(ParseFile("tint on").commands.size() == 1); // no trailing newline

    // a byte order mark in front of the first command is ignored
    f = ParseFile("\xEF\xBB\xBFstate\nhelp\n");
    CHECK(f.commands.size() == 2 && f.commands[0].kind == CommandKind::State);

    // binary garbage and NULs
    const std::string zeros(100, '\0');
    f = ParseFile(std::string_view(zeros.data(), zeros.size()));
    CHECK(f.commands.size() == 1 && f.commands[0].kind == CommandKind::Unknown);
    std::string binary;
    uint32_t seed = 12345;
    for (int i = 0; i < 10000; ++i)
    {
        seed = seed * 1664525u + 1013904223u;
        binary.push_back(static_cast<char>(seed >> 24));
    }
    f = ParseFile(binary);
    CHECK(f.truncatedBytes);
    CHECK(f.lines <= kMaxCommandLines);
    for (const Command& c : f.commands)
        CHECK(c.kind == CommandKind::Unknown || c.kind == CommandKind::Empty || c.text.size() <= kMaxEchoChars);

    // more than 32 lines: the rest is ignored
    std::string lines;
    for (int i = 0; i < 40; ++i)
        lines += "reset\n";
    f = ParseFile(lines);
    CHECK(f.commands.size() == 32 && f.truncatedLines && !f.truncatedBytes && f.lines == 32);
    CHECK(kMaxCommandLines == 32 && kMaxCommandBytes == 4096);
    // blank lines count as lines
    std::string blanks(40, '\n');
    blanks += "reset\n";
    f = ParseFile(blanks);
    CHECK(f.commands.empty() && f.truncatedLines);

    // more than 4096 bytes: cut there, and a command cut in half is not run
    std::string big;
    while (big.size() < 5000)
        big += "tint on # padding padding padding padding\n";
    f = ParseFile(big);
    CHECK(f.truncatedBytes);
    CHECK(!f.commands.empty() && f.commands.size() < 100);
    for (const Command& c : f.commands)
        CHECK(c.kind == CommandKind::Tint && c.value);
    CHECK(ParseFile(std::string(5000, 'x')).commands.empty()); // one huge line: nothing runs
    CHECK(ParseFile(std::string(5000, 'x')).truncatedBytes);

    f = ParseFile("tint on\ntint off\n", 10);
    CHECK(f.truncatedBytes && f.commands.size() == 1 && f.commands[0].value); // "tint on\nti" -> only the whole line
    f = ParseFile("tint on\ntint off", 8);
    CHECK(f.truncatedBytes && f.commands.size() == 1);
    f = ParseFile("tint on\ntint off", 7);
    CHECK(f.truncatedBytes && f.commands.size() == 1 && f.commands[0].kind == CommandKind::Tint); // the cut is at the end of line 1
    f = ParseFile("tint on\ntint off", 16);
    CHECK(!f.truncatedBytes && f.commands.size() == 2);
}

// ---------------------------------------------------------------------------------------------------------------------
// PNG writer, checked by a small independent decoder
// ---------------------------------------------------------------------------------------------------------------------

uint32_t SlowCrc(const uint8_t* p, size_t n)
{
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < n; ++i)
    {
        crc ^= p[i];
        for (int k = 0; k < 8; ++k)
            crc = (crc & 1) ? (crc >> 1) ^ 0xedb88320u : crc >> 1;
    }
    return ~crc;
}

uint32_t Be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

struct Decoded
{
    bool ok = false;
    uint32_t width = 0, height = 0;
    uint8_t depth = 0, colorType = 0;
    bool hasSrgb = false;
    int chunks = 0;
    int storedBlocks = 0;
    std::vector<uint8_t> pixels; // tightly packed rows, filter bytes removed
    std::string error;
};

Decoded Decode(const std::vector<uint8_t>& png, size_t start = 0)
{
    Decoded d;
    auto fail = [&d](const char* why) {
        d.error = why;
        return d;
    };
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    if (png.size() < start + 8 + 12 || std::memcmp(png.data() + start, sig, 8) != 0)
        return fail("signature");
    size_t pos = start + 8;
    std::vector<uint8_t> idat;
    bool sawIend = false;
    std::string order;
    while (pos < png.size())
    {
        if (pos + 12 > png.size())
            return fail("chunk header");
        const uint32_t len = Be32(&png[pos]);
        if (pos + 12 + len > png.size())
            return fail("chunk length");
        const uint8_t* type = &png[pos + 4];
        const uint8_t* data = &png[pos + 8];
        if (Be32(data + len) != SlowCrc(type, 4 + len))
            return fail("chunk crc");
        const std::string name(reinterpret_cast<const char*>(type), 4);
        order += name + ",";
        ++d.chunks;
        if (name == "IHDR")
        {
            if (len != 13 || d.chunks != 1)
                return fail("IHDR");
            d.width = Be32(data);
            d.height = Be32(data + 4);
            d.depth = data[8];
            d.colorType = data[9];
            if (data[10] != 0 || data[11] != 0 || data[12] != 0)
                return fail("IHDR methods");
        }
        else if (name == "sRGB")
            d.hasSrgb = len == 1 && data[0] == 0;
        else if (name == "IDAT")
            idat.insert(idat.end(), data, data + len);
        else if (name == "IEND")
        {
            if (len != 0)
                return fail("IEND length");
            sawIend = true;
            pos += 12;
            break;
        }
        else
            return fail("unexpected chunk");
        pos += 12 + len;
    }
    if (!sawIend || pos != png.size())
        return fail("IEND / trailing bytes");
    if (idat.size() < 2 + 5 + 4 || idat[0] != 0x78 || ((idat[0] << 8) | idat[1]) % 31 != 0 || (idat[1] & 0x20))
        return fail("zlib header");

    // stored deflate blocks
    std::vector<uint8_t> raw;
    size_t i = 2;
    bool finalSeen = false;
    while (true)
    {
        if (finalSeen)
            return fail("data after final block");
        if (i + 5 > idat.size())
            return fail("block header");
        const uint8_t hdr = idat[i];
        if ((hdr & 6) != 0)
            return fail("block type is not stored");
        finalSeen = (hdr & 1) != 0;
        const uint32_t len = idat[i + 1] | (idat[i + 2] << 8);
        const uint32_t nlen = idat[i + 3] | (idat[i + 4] << 8);
        if ((len ^ 0xffff) != nlen)
            return fail("NLEN");
        if (len > 65535 || i + 5 + len > idat.size())
            return fail("block length");
        raw.insert(raw.end(), idat.begin() + i + 5, idat.begin() + i + 5 + len);
        i += 5 + len;
        ++d.storedBlocks;
        if (finalSeen)
            break;
    }
    if (i + 4 != idat.size())
        return fail("zlib trailer size");
    uint32_t a = 1, b = 0;
    for (uint8_t v : raw)
    {
        a = (a + v) % 65521;
        b = (b + a) % 65521;
    }
    if (Be32(&idat[i]) != ((b << 16) | a))
        return fail("adler32");

    const uint32_t channels = d.colorType == 2 ? 3 : d.colorType == 0 ? 1 : 0;
    if (!channels || d.depth != 8)
        return fail("format");
    const size_t rowBytes = size_t(d.width) * channels;
    if (raw.size() != size_t(d.height) * (rowBytes + 1))
        return fail("raw size");
    for (uint32_t y = 0; y < d.height; ++y)
    {
        if (raw[y * (rowBytes + 1)] != 0)
            return fail("filter byte");
        d.pixels.insert(d.pixels.end(), raw.begin() + y * (rowBytes + 1) + 1, raw.begin() + (y + 1) * (rowBytes + 1));
    }
    d.ok = true;
    return d;
}

std::vector<uint8_t> Pattern(size_t n, uint32_t seed)
{
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i)
    {
        seed = seed * 1664525u + 1013904223u;
        v[i] = static_cast<uint8_t>(seed >> 24);
    }
    return v;
}

void RoundTrip(uint32_t w, uint32_t h, uint32_t channels, int expectBlocks = -1)
{
    const std::vector<uint8_t> pixels = Pattern(size_t(w) * h * channels, w * 31 + h);
    std::vector<uint8_t> png;
    CHECK(EncodePng(w, h, channels, pixels.data(), png));
    CHECK(png.size() == PngSize(w, h, channels));
    const Decoded d = Decode(png);
    if (!d.ok)
        std::printf("decode %ux%ux%u: %s\n", w, h, channels, d.error.c_str());
    CHECK(d.ok);
    CHECK(d.width == w && d.height == h && d.depth == 8);
    CHECK(d.colorType == (channels == 3 ? 2 : 0));
    CHECK(d.hasSrgb == (channels == 3));
    CHECK(d.pixels == pixels);
    if (expectBlocks >= 0)
        CHECK(d.storedBlocks == expectBlocks);
}

void TestPng()
{
    // known values of the two checksums
    const char* wiki = "Wikipedia";
    CHECK(Adler32(wiki, 9) == 0x11E60398u);
    CHECK(Adler32("", 0) == 1u);
    CHECK(Adler32(wiki, 4, 1) != 1u);
    CHECK(Adler32(wiki + 4, 5, Adler32(wiki, 4)) == 0x11E60398u); // can be continued
    const std::vector<uint8_t> ff(100000, 0xff);
    uint32_t a = 1, b = 0;
    for (uint8_t v : ff)
    {
        a = (a + v) % 65521;
        b = (b + a) % 65521;
    }
    CHECK(Adler32(ff.data(), ff.size()) == ((b << 16) | a)); // large input, many blocks of 5552
    CHECK(SlowCrc(reinterpret_cast<const uint8_t*>("IEND"), 4) == 0xAE426082u);
    CHECK(ext::core::Crc32("123456789", 9) == 0xCBF43926u);

    RoundTrip(1, 1, 1, 1);
    RoundTrip(1, 1, 3, 1);
    RoundTrip(3, 2, 3, 1);
    RoundTrip(7, 5, 1, 1);
    RoundTrip(64, 48, 3, 1);
    RoundTrip(300, 300, 3, 5);   // 270300 bytes of raw data: five blocks, rows cross block boundaries
    RoundTrip(256, 255, 1, 1);   // 255 rows of (1 + 256) bytes = exactly 65535: one full block
    RoundTrip(255, 256, 1, 2);   // exactly 65536: a full block and a block of one byte
    RoundTrip(1000, 1, 3, 1);
    RoundTrip(1, 4000, 1, 1);

    // the end of every file is the same 12 bytes
    std::vector<uint8_t> png;
    const uint8_t px[3] = { 1, 2, 3 };
    CHECK(EncodePng(1, 1, 3, px, png));
    const uint8_t iend[12] = { 0, 0, 0, 0, 'I', 'E', 'N', 'D', 0xAE, 0x42, 0x60, 0x82 };
    CHECK(png.size() >= 12 && std::memcmp(png.data() + png.size() - 12, iend, 12) == 0);
    // the IHDR is the first chunk after the signature, with its own CRC
    CHECK(Be32(&png[8]) == 13 && std::memcmp(&png[12], "IHDR", 4) == 0 && Be32(&png[29]) == SlowCrc(&png[12], 17));

    // it appends to a buffer that already has content
    std::vector<uint8_t> prefixed { 9, 9, 9, 9, 9 };
    const std::vector<uint8_t> pixels = Pattern(12 * 7 * 3, 4);
    CHECK(EncodePng(12, 7, 3, pixels.data(), prefixed));
    CHECK(prefixed.size() == 5 + PngSize(12, 7, 3));
    const Decoded d = Decode(prefixed, 5);
    CHECK(d.ok && d.pixels == pixels);
    CHECK(prefixed[0] == 9 && prefixed[4] == 9);

    // exact sizes
    CHECK(PngSize(1, 1, 1) == 8 + 25 + (12 + 2 + 5 + 2 + 4) + 12);
    CHECK(PngSize(1, 1, 3) == 8 + 25 + 13 + (12 + 2 + 5 + 4 + 4) + 12);
    CHECK(PngSize(3840, 2160, 3) > 3840ull * 2160 * 3);
    CHECK(PngSize(3840, 2160, 3) < 3840ull * 2160 * 3 + 200000);
    CHECK(PngSize(2304, 3840, 1) > 2304ull * 3840);

    // invalid arguments: false, nothing appended, size 0
    std::vector<uint8_t> untouched { 1, 2 };
    CHECK(!EncodePng(0, 4, 3, px, untouched) && !EncodePng(4, 0, 3, px, untouched));
    CHECK(!EncodePng(4, 4, 2, px, untouched) && !EncodePng(4, 4, 4, px, untouched) && !EncodePng(4, 4, 0, px, untouched));
    CHECK(!EncodePng(4, 4, 3, nullptr, untouched));
    CHECK(!EncodePng(kMaxPngDimension + 1, 1, 1, px, untouched) && !EncodePng(1, kMaxPngDimension + 1, 1, px, untouched));
    CHECK(untouched.size() == 2);
    CHECK(PngSize(0, 1, 1) == 0 && PngSize(1, 0, 1) == 0 && PngSize(1, 1, 2) == 0 && PngSize(kMaxPngDimension + 1, 1, 3) == 0);
    CHECK(PngSize(kMaxPngDimension, kMaxPngDimension, 3) > 0);
}
} // namespace

// The per-draw scan is skipped without a COM call when it cannot find anything; the probe alone behaves as before, the flood test no longer
// scans while no preview output exists and no copy is followed.
void TestScanGate()
{
    // nothing on: never
    for (int bits = 0; bits < 8; ++bits)
        CHECK(!ScanCanFindAnything(false, false, (bits & 1) != 0, (bits & 2) != 0, (bits & 4) != 0));
    // a followed copy is alive: always (probe or flood on)
    for (int bits = 0; bits < 4; ++bits)
    {
        CHECK(ScanCanFindAnything(true, false, (bits & 1) != 0, true, (bits & 2) != 0));
        CHECK(ScanCanFindAnything(false, true, (bits & 1) != 0, true, (bits & 2) != 0));
    }
    // the probe alone: a preview output that exists (an open window, also one that stopped re-rendering) is enough, as for the flood test
    CHECK(ScanCanFindAnything(true, false, true, false, false));
    CHECK(ScanCanFindAnything(true, false, true, false, true));
    CHECK(ScanCanFindAnything(true, false, false, false, true));
    CHECK(!ScanCanFindAnything(true, false, false, false, false)); // no output, idle renderer, no copy: nothing to find
    // the flood test: a preview output that exists is enough (it stays on screen without re-rendering) ...
    CHECK(ScanCanFindAnything(false, true, false, false, true));
    CHECK(ScanCanFindAnything(true, true, false, false, true));
    // ... and with no preview output and no copy there is nothing to sample: no scan
    CHECK(!ScanCanFindAnything(false, true, false, false, false));
    CHECK(!ScanCanFindAnything(false, true, true, false, false)); // renderer activity without an output image: still nothing to flood
    CHECK(!ScanCanFindAnything(true, true, true, false, false));
}

// REVIEW-CHARA-2 M6: a requested dump starts where the preview image is complete, not at an arbitrary draw.
void TestDumpTrigger()
{
    using Step = DumpTrigger::Step;
    DumpTrigger t;
    CHECK(t.Decide(false, false, 1000) == Step::Idle && !t.Armed());
    CHECK(t.Decide(false, true, 1000) == Step::Idle); // a complete point without a request: nothing

    // the request is armed; draws in the middle of the frame (no complete point) wait
    CHECK(t.Decide(true, false, 2000) == Step::Wait && t.Armed());
    CHECK(t.Decide(true, false, 2001) == Step::Wait);
    CHECK(t.Decide(true, false, 3999) == Step::Wait); // 1999 ms after arming: still waiting
    // the first complete point starts it
    CHECK(t.Decide(true, true, 2500) == Step::Start && !t.Armed());
    CHECK(t.Decide(false, false, 2501) == Step::Idle); // the caller cleared the request

    // a complete point on the very call that arms it counts (the caller passes it)
    CHECK(t.Decide(true, true, 5000) == Step::Start);
    t.Decide(false, false, 5001);

    // no complete point at all (no finisher, no swap): after 2 s any draw will do
    CHECK(t.Decide(true, false, 6000) == Step::Wait);
    CHECK(t.Decide(true, false, 6000 + kDumpFallbackMs - 1) == Step::Wait);
    CHECK(t.Decide(true, false, 6000 + kDumpFallbackMs) == Step::Start);
    t.Decide(false, false, 9000);

    // a clock that went back does not wait forever
    CHECK(t.Decide(true, false, 10000) == Step::Wait);
    CHECK(t.Decide(true, false, 9000) == Step::Start);
    t.Decide(false, false, 9001);

    // a start the game's state forbids (an active predicate) is postponed to the next complete point, at most kMaxDumpPostponements times
    CHECK(t.Decide(true, true, 20000) == Step::Start);
    CHECK(t.Postpone(20000) && t.Armed());
    CHECK(t.Decide(true, false, 20100) == Step::Wait); // armed again from the postponement
    CHECK(t.Decide(true, true, 20200) == Step::Start);
    for (uint32_t i = 2; i <= kMaxDumpPostponements; ++i)
        CHECK(t.Postpone(20300));
    CHECK(!t.Postpone(20300)); // too often: give up
    t.Decide(false, false, 21000);
    CHECK(t.Postpone(21000)); // a new request starts counting again
}

// REVIEW-CHARA-2 M8: "the facts are those of the swapped draw" only when the swapped draw was the last one scanned.
void TestSameDraw()
{
    CHECK(SameDraw(true, 7, 7));
    CHECK(!SameDraw(true, 7, 8));  // draws went by since the last scanned sampler draw
    CHECK(!SameDraw(true, 8, 7));
    CHECK(!SameDraw(false, 7, 7)); // nothing was scanned yet
    CHECK(!SameDraw(false, 0, 0));

    // The serial advances for every dispatched draw while the probe is on, scanned or not: a paused preview (draws not scanned, the
    // sampler draw scanned earlier) must not make a swap on a later draw look like the scanned one.
    uint64_t serial = 0;
    uint64_t lastScannedSampler = 0;
    bool valid = false;
    for (int draw = 0; draw < 5; ++draw)
        ++serial; // unscanned draws
    ++serial;     // the sampler draw: scanned, recorded
    lastScannedSampler = serial;
    valid = true;
    CHECK(SameDraw(valid, lastScannedSampler, serial)); // swapped on that very draw
    ++serial;                                           // the next draw is swapped but not scanned (no hit)
    CHECK(!SameDraw(valid, lastScannedSampler, serial));
}

// The ResolveSubresource detour has a hook transaction of its own: attached when a debug aid is first switched on, removed when both are off
// (REVIEW-CHARA-2 M2).
void TestResolveHookState()
{
    using Step = ResolveHookState::Step;
    ResolveHookState h;
    CHECK(h.Decide(false) == Step::None && !h.Attached() && !h.Incomplete()); // nothing wanted: nothing attached, nothing missing
    CHECK(h.Decide(true) == Step::Attach);                                    // a debug aid was switched on
    h.AttachResult(true);
    CHECK(h.Attached() && !h.Incomplete());
    CHECK(h.Decide(true) == Step::None && h.Decide(true) == Step::None);      // asked on every draw: acts on the change only
    CHECK(h.Decide(false) == Step::Detach);                                   // both are off: remove the detour
    h.Detached();
    CHECK(!h.Attached() && !h.Incomplete());
    CHECK(h.Decide(false) == Step::None);
    CHECK(h.Decide(true) == Step::Attach); // switched on again: attached again

    // a failed attach: tracking is incomplete (the probe says so), nothing is retried per draw, nothing to remove on switch-off
    ResolveHookState failed;
    CHECK(failed.Decide(true) == Step::Attach);
    failed.AttachResult(false);
    CHECK(!failed.Attached() && failed.Incomplete());
    CHECK(failed.Decide(true) == Step::None && failed.Incomplete());
    CHECK(failed.Decide(false) == Step::None && !failed.Incomplete()); // nothing was attached: nothing to detach
    CHECK(failed.Decide(true) == Step::Attach);                        // the next switch-on tries again
    failed.AttachResult(true);
    CHECK(failed.Attached() && !failed.Incomplete());

    // the probe and the flood test are one "wanted": the second aid switched on while the first is on changes nothing, and the detour stays
    // until both are off (the caller passes the OR of both)
    ResolveHookState both;
    CHECK(both.Decide(true) == Step::Attach);
    both.AttachResult(true);
    CHECK(both.Decide(true) == Step::None);
    CHECK(both.Decide(false) == Step::Detach);
}

int main()
{
    TestScanGate();
    TestChainAndHopCap();
    TestHeldReferences();
    TestRecycledAddressWithoutReference();
    TestFormatFamilyAndWholeCopies();
    TestPromotion();
    TestRefusals();
    TestExpiryAndRefresh();
    TestFullSet();
    TestCopiesSwitchAndClear();
    TestCaseLog();
    TestRateCounter();
    TestThrottledSwapCounter();
    TestOutputBudget();
    TestToggles();
    TestPlainCommands();
    TestGarbage();
    TestParseFile();
    TestPng();
    TestResolveHookState();
    TestDumpTrigger();
    TestSameDraw();
    return TEST_RESULT("chara_probe_rules");
}
