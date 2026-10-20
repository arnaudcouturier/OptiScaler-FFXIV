// [MIP-BIND] The add/replace detector (ext/upscaler/MipBind.h, ModeDetector). Observations are (family, bias, base target in
// force when the sampler was made). ADD until a pair appears: two distinct samplers of one family made at targets at least
// 0.25 apart whose biases differ by the same amount. Duplicates do not count, ambiguity stays ADD, a false pair needs the same
// family, and nothing but a confirmed REPLACE is final.
#include "ext/upscaler/MipBind.h"

#include <cmath>
#include <cstdio>

using namespace ext::upscaler::mipbind;
using Kind = Decision::Kind;

static int g_failed = 0;

#define CHECK(cond)                                                                                              \
    do                                                                                                           \
    {                                                                                                            \
        if (!(cond))                                                                                             \
        {                                                                                                        \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);                                                  \
            ++g_failed;                                                                                          \
        }                                                                                                        \
    } while (0)

static bool Near(double a, double b, double tolerance = 1e-6) { return std::fabs(a - b) <= tolerance; }

static constexpr double kDlaa = 0.0;
static constexpr double kQuality = -0.584963; // log2(2560 / 3840)
static constexpr double kPerf = -1.0;
// Families of the STUDY 2.3 sessions (any distinct numbers do: only equality matters)
static constexpr std::uint32_t kTrack = 0x1001; // the family that tracks the mode
static constexpr std::uint32_t kArtA = 0x2002, kArtB = 0x3003, kArtC = 0x4004;

int main()
{
    // Nothing known: ADD.
    {
        ModeDetector d;
        CHECK(d.mode() == Mode::Add && d.state() == ModeDetector::State::Default && !d.final());
        CHECK(!d.OnObserved(kTrack, -1.0f, kDlaa).any());
        CHECK(!d.OnObserved(kArtA, -0.5f, kDlaa).any());
        CHECK(d.mode() == Mode::Add && d.observations() == 2);
    }

    // The logged FFXIV case: DLAA {track -1, art -0.5}; at Quality the track family has -1.5625 (-1 + log2(scale) in 1/16
    // steps), the art ones are new samplers with constant values -2 and -1.5 of other families.
    {
        ModeDetector d;
        d.OnObserved(kTrack, -1.0f, kDlaa);
        d.OnObserved(kArtA, -0.5f, kDlaa);
        CHECK(!d.OnObserved(kArtB, -2.0f, kQuality).any()); // another family: never a pair
        CHECK(!d.OnObserved(kArtC, -1.5f, kQuality).any());
        const auto p = d.OnObserved(kTrack, -1.5625f, kQuality); // -1 + (-0.585) = -1.585, within 1/16 of it
        CHECK(p.kind == Kind::ReplaceProvisional && p.mode == Mode::Replace && p.candidates == 1);
        CHECK(Near(p.seenBefore, -1.0) && Near(p.seenAfter, -1.5625) && Near(p.delta, kQuality));
        CHECK(d.mode() == Mode::Replace && d.state() == ModeDetector::State::Provisional && !d.final());

        // 12 more distinct samplers without a second pair: confirmed, and then final
        Decision last;
        for (unsigned i = 0; i < kConfirmAfter; ++i)
        {
            last = d.OnObserved(0x9000 + i, -0.25f - 0.1f * static_cast<float>(i), kQuality);
            CHECK(i + 1 < kConfirmAfter ? !last.any() : last.kind == Kind::ReplaceConfirmed);
        }
        CHECK(d.final() && d.mode() == Mode::Replace);
        // final: nothing changes it any more
        CHECK(!d.OnObserved(kTrack, -2.0f, kPerf).any());
        CHECK(!d.OnObserved(kTrack, -1.0f - 0.7f, kPerf).any());
        CHECK(d.mode() == Mode::Replace && d.final());
    }

    // (a) Duplicates: a sampler the game re-creates (the runtime returns the existing object) is not a new observation.
    {
        ModeDetector d;
        d.OnObserved(kTrack, -1.0f, kDlaa);
        for (int i = 0; i < 200; ++i)
        {
            CHECK(!d.OnObserved(kTrack, -1.0f, kQuality).any()); // the same sampler "again" at another ratio
            CHECK(!d.OnObserved(kTrack, -1.01f, kDlaa).any());   // within 0.02: the same value
        }
        CHECK(d.observations() == 1 && d.mode() == Mode::Add);

        // 200 re-creates of art values cannot reach the "no pair" count either
        for (int round = 0; round < 10; ++round)
            for (int i = 0; i < 30; ++i)
                d.OnObserved(0x7000 + i, -0.5f - 0.1f * static_cast<float>(i), round & 1 ? kQuality : kDlaa);
        CHECK(d.observations() == 31); // 30 distinct + the first one
    }

    // (c) The false REPLACE of the review: art values -0.5 and -1.5 of different families and a DLAA -> Performance move of
    // -1.0 (exactly the distance between them) must not give REPLACE. Neither does the same with the closest families.
    {
        ModeDetector d;
        d.OnObserved(kArtA, -0.5f, kDlaa);
        d.OnObserved(kArtB, -1.0f, kDlaa);
        CHECK(!d.OnObserved(kArtC, -1.5f, kPerf).any());
        CHECK(d.mode() == Mode::Add);
        // The residual case, stated: two art values of ONE family exactly one move apart look like a tracking pair.
        CHECK(d.OnObserved(kArtB, -2.0f, kPerf).kind == Kind::ReplaceProvisional);
    }
    {
        ModeDetector d;
        d.OnObserved(kArtA, -0.5f, kDlaa);
        CHECK(!d.OnObserved(kArtB, -1.5f, kPerf).any()); // different family
        CHECK(!d.OnObserved(kArtC, -2.5f, kPerf).any());
        CHECK(d.mode() == Mode::Add && d.state() == ModeDetector::State::Default);
    }

    // (b) A small ratio change cannot decide: under 0.25 a pair is indistinguishable from a neighbouring art value.
    {
        ModeDetector d;
        d.OnObserved(kTrack, -1.0f, kDlaa);
        CHECK(!d.OnObserved(kTrack, -1.2f, -0.2).any());   // delta -0.2, bias moved -0.2: a pair, but too small
        CHECK(!d.OnObserved(kTrack, -1.0625f, -0.05).any()); // delta -0.05
        CHECK(d.mode() == Mode::Add);
        // exactly at the threshold counts
        ModeDetector e;
        e.OnObserved(kTrack, -1.0f, kDlaa);
        CHECK(e.OnObserved(kTrack, -1.25f, -0.25).kind == Kind::ReplaceProvisional);
    }

    // Ambiguity: two pairs. One observation that pairs with two older samplers of its family ...
    {
        ModeDetector d;
        d.OnObserved(kTrack, -1.0f, kDlaa);
        d.OnObserved(kTrack, -1.05f, kDlaa); // 0.05 apart: two distinct values of one family
        const auto a = d.OnObserved(kTrack, -1.6f, kQuality); // pairs with both (-0.6 vs -0.585, -0.55 vs -0.585)
        CHECK(a.kind == Kind::AddAmbiguous && a.candidates == 2 && a.mode == Mode::Add);
        CHECK(d.mode() == Mode::Add && d.state() == ModeDetector::State::Ambiguous);
        // ... stays ADD for later observations at the same ratio, even ones that would pair
        d.OnObserved(kTrack, -1.62f, kQuality);
        CHECK(d.mode() == Mode::Add && d.state() == ModeDetector::State::Ambiguous);
        // ... and is re-opened by an observation made at another ratio: a clean pair then decides
        ModeDetector e;
        e.OnObserved(kTrack, -1.0f, kDlaa);
        e.OnObserved(kTrack, -1.05f, kDlaa);
        e.OnObserved(kArtB, -0.8f, kDlaa);
        e.OnObserved(kTrack, -1.6f, kQuality); // ambiguous
        CHECK(e.state() == ModeDetector::State::Ambiguous);
        CHECK(e.OnObserved(kArtA, -0.3f, kPerf).kind == Kind::None); // another ratio re-opens; this one has no pair
        CHECK(e.state() == ModeDetector::State::Default);
        CHECK(e.OnObserved(kArtB, -1.8f, kPerf).kind == Kind::ReplaceProvisional); // -0.8 + (-1.0) = -1.8, the only pair
    }
    // ... or a second, different pair while the first one waits for its confirmation
    {
        ModeDetector d;
        d.OnObserved(kTrack, -1.0f, kDlaa);
        d.OnObserved(kArtA, -0.5f, kDlaa);
        CHECK(d.OnObserved(kTrack, -1.5625f, kQuality).kind == Kind::ReplaceProvisional);
        for (int i = 0; i < 4; ++i)
            d.OnObserved(0xA000 + i, -0.3f - 0.1f * static_cast<float>(i), kQuality);
        const auto second = d.OnObserved(kArtA, -1.1f, kQuality); // -0.5 + (-0.585) = -1.085: another pair of another family
        CHECK(second.kind == Kind::AddAmbiguous && d.mode() == Mode::Add && !d.final());
    }

    // (d) ADD for lack of data is never final: after 40 distinct samplers at other ratios without a pair it says so once, and a
    // pair that arrives later still decides.
    {
        ModeDetector d;
        d.OnObserved(kArtA, -0.5f, kDlaa);
        int noPair = 0;
        for (unsigned i = 0; i < 2 * kNoPairAfter; ++i)
        {
            const auto r = d.OnObserved(0x5000 + i, -0.3f - 0.01f * static_cast<float>(i), kQuality);
            noPair += r.kind == Kind::AddNoPair ? 1 : 0;
        }
        CHECK(noPair == 1);
        CHECK(d.mode() == Mode::Add && d.state() == ModeDetector::State::Default);
        // a late pair still counts: family 0x5000 was seen at kQuality (-0.3); at Performance -0.3 + (-0.415) = -0.715
        const auto late = d.OnObserved(0x5000, -0.715f, kPerf);
        CHECK(late.kind == Kind::ReplaceProvisional && d.mode() == Mode::Replace);
    }

    // A change in the other direction (Quality -> DLAA, delta +0.585) is a pair too.
    {
        ModeDetector d;
        d.OnObserved(kTrack, -1.5625f, kQuality);
        d.OnObserved(kArtA, -0.5f, kQuality);
        const auto r = d.OnObserved(kTrack, -0.98f, kDlaa);
        CHECK(r.kind == Kind::ReplaceProvisional && Near(r.delta, 0.584963));
    }

    // Values that are not numbers, a full detector, and Reset.
    {
        ModeDetector d;
        CHECK(!d.OnObserved(kTrack, std::nanf(""), kDlaa).any());
        CHECK(!d.OnObserved(kTrack, -1.0f, std::nan("")).any());
        CHECK(d.observations() == 0);
        for (size_t i = 0; i < kMaxObservations + 50; ++i)
            d.OnObserved(0x6000 + static_cast<std::uint32_t>(i), -0.5f, kDlaa);
        CHECK(d.observations() == kMaxObservations);
        d.Reset();
        CHECK(d.observations() == 0 && d.mode() == Mode::Add && d.state() == ModeDetector::State::Default);
    }

    // The latch watch: one warning after 600 evaluates with samplers known and not one bind substituted.
    {
        LatchWatch w;
        int warned = 0;
        for (unsigned i = 0; i < 2 * LatchWatch::kEvaluates; ++i)
            warned += w.OnEvaluate(true, 12, 0) ? 1 : 0;
        CHECK(warned == 1);

        LatchWatch ok;
        for (unsigned i = 0; i < LatchWatch::kEvaluates / 2; ++i)
            ok.OnEvaluate(true, 12, 0);
        CHECK(!ok.OnEvaluate(true, 12, 5)); // a bind was substituted: satisfied for good
        for (unsigned i = 0; i < 2 * LatchWatch::kEvaluates; ++i)
            CHECK(!ok.OnEvaluate(true, 12, 0));

        LatchWatch idle; // nothing to substitute: no warning
        for (unsigned i = 0; i < 2 * LatchWatch::kEvaluates; ++i)
            CHECK(!idle.OnEvaluate(true, 0, 0) && !idle.OnEvaluate(false, 12, 0));
    }

    if (g_failed == 0)
        std::printf("mip_bind_mode: all passed\n");
    return g_failed == 0 ? 0 : 1;
}
