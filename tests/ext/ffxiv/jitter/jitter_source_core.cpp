#include "check.h"

#include "ext/ffxiv/jitter/FfxivJitterSourceCore.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using namespace ext::ffxiv::jitter::source;

// The pure numbers of the source-level jitter fix: phase count, native strength, normalisation, projection, pairing, verification,
// the decision table and the log lines.

namespace
{
bool Has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

float Halton(std::uint64_t index, unsigned base)
{
    float factor = 1, result = 0;
    for (; index; index /= base)
    {
        factor /= base;
        result += factor * float(index % base);
    }
    return result - 0.5f;
}

void TestPhaseCount()
{
    // The cases of the brief: 8 at native, 18 Quality, 24 Balanced, 32 Performance, 72 Ultra Performance (4K output).
    CHECK(PhaseCount(3840, 2160, 3840, 2160) == 8);
    CHECK(PhaseCount(2560, 1440, 3840, 2160) == 18);   // Quality 1.5x
    CHECK(PhaseCount(2227, 1253, 3840, 2160) == 24);   // Balanced 1.724x
    CHECK(PhaseCount(1920, 1080, 3840, 2160) == 32);   // Performance 2x
    CHECK(PhaseCount(1280, 720, 3840, 2160) == 72);    // Ultra Performance 3x
    // 1080p output, the same rule
    CHECK(PhaseCount(1920, 1080, 1920, 1080) == 8);
    CHECK(PhaseCount(960, 540, 1920, 1080) == 32);
    // exact integer rounding: an area ratio just above an integer rounds up, never down
    CHECK(PhaseCount(1000, 1000, 1001, 1000) == 9);    // 8.008 -> 9
    CHECK(PhaseCount(1001, 1000, 1001, 1000) == 8);
    // the upper clamp
    CHECK(PhaseCount(16, 16, 16384, 16384) == 4096);
    // invalid sizes: 0 = leave the game's phase count alone
    CHECK(PhaseCount(0, 1080, 3840, 2160) == 0);
    CHECK(PhaseCount(1920, 0, 3840, 2160) == 0);
    CHECK(PhaseCount(1920, 1080, 0, 2160) == 0);
    CHECK(PhaseCount(1920, 1080, 3840, 0) == 0);
    CHECK(PhaseCount(3841, 2160, 3840, 2160) == 0);    // render larger than the output
    CHECK(PhaseCount(3840, 2161, 3840, 2160) == 0);
    CHECK(PhaseCount(1920, 1080, 16385, 2160) == 0);   // beyond the sane extent
    CHECK(PhaseCount(16385, 1080, 16385, 2160) == 0);
}

void TestNativeStrength()
{
    // The game's expression: ratio = ext0/ext2 * scale; strength = (amount - 0.001) * ratio^2 + 0.001.
    // amount 1.0, scale 1.0, ext0 == ext2 -> 1.0 (the camera already draws the full lattice)
    auto s = NativeStrength(1.0f, 1.0f, 100, 100);
    CHECK(s && std::fabs(*s - 1.0f) < 1e-6f);
    // measured in game: strength 0.6. ratio = sqrt(0.6) with amount 1: ratio^2 = 0.6 -> (0.999 * 0.6) + 0.001 = 0.6004
    const float r = std::sqrt(0.6f);
    s = NativeStrength(1.0f, r, 1000, 1000);
    CHECK(s && std::fabs(*s - 0.6f) < 2e-3f);
    // order of operations as the executable does it: (amount - c) * (ratio * ratio) + c in float
    {
        const float amount = 0.8f, scale = 0.9f;
        const float ratio = (3000.0f / 4000.0f) * scale;
        const float expect = (amount - 0.001f) * (ratio * ratio) + 0.001f;
        const auto got = NativeStrength(amount, scale, 3000, 4000);
        CHECK(got && *got == expect);
    }
    // range [0.125, 4]
    CHECK(NativeStrength(0.01f, 1.0f, 100, 100).has_value() == false);   // 0.00999 + .001 -> below 0.125
    CHECK(NativeStrength(1.0f, 1.0f, 100, 100).has_value());
    CHECK(NativeStrength(5.0f, 1.0f, 100, 100).has_value() == false);    // 5.0 > 4
    CHECK(NativeStrength(4.0f, 1.0f, 100, 100).has_value());
    CHECK(NativeStrength(0.125f, 1.0f, 100, 100).has_value());           // (0.124) + 0.001 = 0.125 at the edge
    // unusable inputs
    CHECK(NativeStrength(1.0f, 1.0f, 100, 0).has_value() == false);      // extent 0
    CHECK(NativeStrength(NAN, 1.0f, 100, 100).has_value() == false);
    CHECK(NativeStrength(1.0f, INFINITY, 100, 100).has_value() == false);
    CHECK(NativeStrength(1.0f, NAN, 100, 100).has_value() == false);
}

void TestNormalize()
{
    float x = 0.3f, y = -0.2f;
    CHECK(NormalizeHalton(x, y, 0.6f));
    CHECK(std::fabs(x - 0.5f) < 1e-6f && std::fabs(y + 0.2f / 0.6f) < 1e-6f);
    // the game then multiplies by the strength: the drawn value is the generator's own
    CHECK(std::fabs(x * 0.6f - 0.3f) < 1e-6f && std::fabs(y * 0.6f + 0.2f) < 1e-6f);
    // the full lattice: |h| up to 0.5 comes back as 0.5
    x = 0.5f;
    y = -0.5f;
    CHECK(NormalizeHalton(x, y, 0.6f));
    CHECK(std::fabs(x * 0.6f - 0.5f) < 1e-6f);
    // strength 1: nothing to divide, and the frame counts as normalised (the phase count is replaced for it; one predicate for both decisions,
    // changed deliberately from the first version, which reported false and left the fix "inactive")
    x = 0.25f;
    y = 0.25f;
    CHECK(NormalizeHalton(x, y, 1.0f));
    CHECK(x == 0.25f && y == 0.25f);
    CHECK(NormalizeHalton(x, y, 1.0f + 5e-7f) && x == 0.25f);
    CHECK(NormalizeHalton(x, y, 1.0f + 1e-5f) && x < 0.25f); // just outside the equality window: divided
    x = 0.7f; // an out-of-range output at strength 1 is still refused
    CHECK(!NormalizeHalton(x, y, 1.0f) && x == 0.7f);
    x = y = 0.25f;
    // not a Halton value, or an unusable strength: unchanged
    for (float bad : { 0.6f, 0.0f, 0.1f, 5.0f, NAN, INFINITY })
    {
        x = 0.3f;
        y = 0.3f;
        const bool changed = NormalizeHalton(x, y, bad);
        const bool expect = bad >= 0.125f && bad <= 4.0f && std::isfinite(bad);
        CHECK(changed == expect);
        if (!expect)
            CHECK(x == 0.3f && y == 0.3f);
    }
    x = 0.6f;
    y = 0.1f;
    CHECK(!NormalizeHalton(x, y, 0.6f) && x == 0.6f && y == 0.1f); // out of the Halton range
    x = NAN;
    y = 0.1f;
    CHECK(!NormalizeHalton(x, y, 0.6f));
    x = 0.2f;
    y = INFINITY;
    CHECK(!NormalizeHalton(x, y, 0.6f) && x == 0.2f);
}

void TestPlan()
{
    GeneratorInputs in;
    in.dlssMode = true;
    in.amount = 1.0f;
    in.scale = std::sqrt(0.6f);
    in.extent0 = in.extent2 = 1000;
    in.phases = kNativePhases;
    in.renderW = 1920;
    in.renderH = 1080;
    in.outW = 3840;
    in.outH = 2160;
    GeneratorPlan p = PlanGenerate(in);
    CHECK(p.why == PlanWhy::Normalize && p.phases == 32 && std::fabs(p.strength - 0.6f) < 2e-3f);
    // quality change: the phase count follows the new sizes
    in.renderW = 2560;
    in.renderH = 1440;
    p = PlanGenerate(in);
    CHECK(p.why == PlanWhy::Normalize && p.phases == 18);
    // the game's own phase count (not unbounded) is kept
    in.phases = 12;
    p = PlanGenerate(in);
    CHECK(p.why == PlanWhy::Normalize && p.phases == 12);
    // sizes not usable: the phase count stays the game's, the amplitude is still normalised
    in.phases = kNativePhases;
    in.outW = 0;
    p = PlanGenerate(in);
    CHECK(p.why == PlanWhy::Normalize && p.phases == kNativePhases);
    in.outW = 3840;
    // not the DLSS mode: left alone
    in.dlssMode = false;
    p = PlanGenerate(in);
    CHECK(p.why == PlanWhy::NotDlssMode && p.phases == kNativePhases);
    in.dlssMode = true;
    // an invalid strength: left alone, phases untouched
    in.extent2 = 0;
    p = PlanGenerate(in);
    CHECK(p.why == PlanWhy::StrengthInvalid && p.phases == kNativePhases);
}

// Observed sample of a game that draws strength * jitter: ndc = 2 * strength * jitter / size (y negated).
Observed Sample(float rawX, float rawY, float strength, std::uint32_t w, std::uint32_t h, std::uint64_t t)
{
    Observed o;
    o.rawX = rawX;
    o.rawY = rawY;
    o.ndcX = 2.0f * strength * rawX / static_cast<float>(w);
    o.ndcY = -(2.0f * strength * rawY) / static_cast<float>(h);
    o.width = w;
    o.height = h;
    o.timeMs = t;
    return o;
}

void TestProjection()
{
    const Observed o = Sample(0.5f, -0.3f, 0.6f, 1920, 1080, 100);
    const auto p = ProjectOffset(o);
    CHECK(p && std::fabs(p->x - 0.3f) < 1e-5f && std::fabs(p->y + 0.18f) < 1e-5f);
    // x = ndcX * width / 2, y = -ndcY * height / 2
    Observed q;
    q.rawX = 0.4f;
    q.rawY = 0.4f;
    q.ndcX = 0.001f;
    q.ndcY = -0.001f;
    q.width = 2000;
    q.height = 1000;
    const auto pq = ProjectOffset(q);
    CHECK(!pq); // x = 1.0, y = 0.5: the axes do not describe one strength (x*rawY != y*rawX)
    q.ndcY = -0.002f;
    const auto pr = ProjectOffset(q);
    CHECK(pr && std::fabs(pr->x - 1.0f) < 1e-6f && std::fabs(pr->y - 1.0f) < 1e-6f);
    // the jitter-disabled path leaves old NDC storage: raw 0/0 -> 0/0 whatever the NDC holds
    Observed z;
    z.ndcX = 0.01f;
    z.ndcY = 0.01f;
    z.width = 1920;
    z.height = 1080;
    const auto pz = ProjectOffset(z);
    CHECK(pz && pz->x == 0.0f && pz->y == 0.0f);
    // refusals
    Observed bad = o;
    bad.width = 0;
    CHECK(!ProjectOffset(bad));
    bad = o;
    bad.height = 20000;
    CHECK(!ProjectOffset(bad));
    bad = o;
    bad.rawX = NAN;
    CHECK(!ProjectOffset(bad));
    bad = o;
    bad.ndcY = INFINITY;
    CHECK(!ProjectOffset(bad));
    bad = o;
    bad.rawX = 5.0f; // above 4
    CHECK(!ProjectOffset(bad));
    bad = o;
    bad.ndcX = -o.ndcX; // wrong sign against the raw value
    bad.ndcY = -o.ndcY;
    CHECK(!ProjectOffset(bad));
    bad = o;
    bad.ndcX = 0.01f; // 9.6 px: above 2
    CHECK(!ProjectOffset(bad));
    bad = o;
    bad.rawY = 0.0f; // an axis with no raw jitter but an NDC translation
    CHECK(!ProjectOffset(bad));
}

void TestRingPairing()
{
    Ring<Observed, kRingSize> ring;
    const float s = 0.6f;
    // 40 frames: the ring keeps the newest 32
    for (unsigned i = 0; i < 40; ++i)
        ring.Push(Sample(Halton(i + 1, 2) / s, Halton(i + 1, 3) / s, s, 1920, 1080, 1000 + i * 8));
    CHECK(ring.Size() == kRingSize);
    const std::uint64_t now = 1000 + 39 * 8 + 4;
    // the newest frame
    auto m = PairSample(ring, Halton(40, 2) / s, Halton(40, 3) / s, 1920, 1080, now);
    CHECK(m && std::fabs(m->pixels.x - Halton(40, 2)) < 1e-5f && std::fabs(m->pixels.y - Halton(40, 3)) < 1e-5f);
    // a frame 31 frames back is still in the ring
    m = PairSample(ring, Halton(9, 2) / s, Halton(9, 3) / s, 1920, 1080, now);
    CHECK(m && std::fabs(m->pixels.x - Halton(9, 2)) < 1e-5f);
    // frame 1..8 left the ring
    CHECK(!PairSample(ring, Halton(8, 2) / s, Halton(8, 3) / s, 1920, 1080, now));
    // the raw pair must match exactly (float noise only)
    CHECK(!PairSample(ring, Halton(40, 2) / s + 1e-4f, Halton(40, 3) / s, 1920, 1080, now));
    CHECK(PairSample(ring, Halton(40, 2) / s + 5e-8f, Halton(40, 3) / s, 1920, 1080, now));
    // sizes: within one pixel, otherwise no match
    CHECK(PairSample(ring, Halton(40, 2) / s, Halton(40, 3) / s, 1921, 1079, now));
    CHECK(!PairSample(ring, Halton(40, 2) / s, Halton(40, 3) / s, 1923, 1080, now));
    CHECK(!PairSample(ring, Halton(40, 2) / s, Halton(40, 3) / s, 0, 0, now));
    // age: younger than a second
    CHECK(PairSample(ring, Halton(40, 2) / s, Halton(40, 3) / s, 1920, 1080, 1000 + 39 * 8 + 999));
    CHECK(!PairSample(ring, Halton(40, 2) / s, Halton(40, 3) / s, 1920, 1080, 1000 + 39 * 8 + 1000));
    // a clock that went backwards counts as no time passed
    CHECK(PairSample(ring, Halton(40, 2) / s, Halton(40, 3) / s, 1920, 1080, 5));
    // not finite NGX values
    CHECK(!PairSample(ring, NAN, 0.1f, 1920, 1080, now));
    // the same raw pair twice (a repeating pattern): the newest wins
    Ring<Observed, kRingSize> rep;
    rep.Push(Sample(0.2f, 0.2f, 0.6f, 1920, 1080, 10));
    rep.Push(Sample(0.2f, 0.2f, 0.5f, 1920, 1080, 20)); // another strength, the same raw pair
    auto newest = PairSample(rep, 0.2f, 0.2f, 1920, 1080, 30);
    CHECK(newest && std::fabs(newest->pixels.x - 0.1f) < 1e-6f);
    // a sample whose offset cannot be computed is skipped, an older good one is used
    Ring<Observed, kRingSize> skip;
    skip.Push(Sample(0.2f, 0.2f, 0.6f, 1920, 1080, 10));
    Observed torn = Sample(0.2f, 0.2f, 0.6f, 1920, 1080, 20);
    torn.ndcX = NAN;
    skip.Push(torn);
    auto older = PairSample(skip, 0.2f, 0.2f, 1920, 1080, 30);
    CHECK(older && std::fabs(older->pixels.x - 0.12f) < 1e-6f);
    // an empty ring, and a sample with time 0 (never recorded)
    Ring<Observed, kRingSize> empty;
    CHECK(!PairSample(empty, 0.1f, 0.1f, 1920, 1080, 10));
    Ring<Observed, kRingSize> zero;
    zero.Push(Sample(0.2f, 0.2f, 0.6f, 1920, 1080, 0));
    CHECK(!PairSample(zero, 0.2f, 0.2f, 1920, 1080, 10));
}

void TestWatchdog()
{
    Watchdog w;
    CHECK(w.Verdict() == Fallback::None);
    for (unsigned i = 0; i < 10; ++i)
        w.Checked(0.0001f);
    CHECK(w.Verdict() == Fallback::None && w.totals().checked == 10 && w.totals().outside == 0);
    // outside the tolerance: only a RUN falls back
    for (unsigned i = 0; i < kOutsideToleranceLimit - 1; ++i)
        w.Checked(0.2f);
    CHECK(w.Verdict() == Fallback::None);
    w.Checked(0.0001f); // a good frame ends the run
    for (unsigned i = 0; i < kOutsideToleranceLimit - 1; ++i)
        w.Checked(0.2f);
    CHECK(w.Verdict() == Fallback::None);
    w.Checked(0.2f);
    CHECK(w.Verdict() == Fallback::OutsideTolerance);
    CHECK(std::fabs(w.totals().maxDeviation - 0.2f) < 1e-6f);
    // the tolerance edge
    Watchdog edge;
    for (unsigned i = 0; i < kOutsideToleranceLimit + 5; ++i)
        edge.Checked(kVerifyTolerancePx);
    CHECK(edge.Verdict() == Fallback::None && edge.totals().outside == 0);
    // unpaired evaluates: a run, ended by any paired evaluate
    Watchdog u;
    for (unsigned i = 0; i < kUnpairedLimit - 1; ++i)
        u.Unpaired();
    CHECK(u.Verdict() == Fallback::None);
    u.Unverified();
    for (unsigned i = 0; i < kUnpairedLimit - 1; ++i)
        u.Unpaired();
    CHECK(u.Verdict() == Fallback::None);
    u.Unpaired();
    CHECK(u.Verdict() == Fallback::Unpaired && u.totals().unpaired == 2 * kUnpairedLimit - 1);
    u.Reset();
    CHECK(u.Verdict() == Fallback::None && u.totals().unpaired == 0);
    // the limit is short: a miss is no longer harmless (it used to tell the divided raw value for up to 600 evaluates)
    CHECK(kUnpairedLimit >= 60 && kUnpairedLimit <= 120);
    // told from the generator's record: counted, but still a miss for the run (a pairing that is broken for good must fall back)
    Watchdog rec;
    for (unsigned i = 0; i < kUnpairedLimit; ++i)
    {
        rec.Unpaired();
        rec.ToldFromRecord();
    }
    CHECK(rec.Verdict() == Fallback::Unpaired && rec.totals().toldFromRecord == kUnpairedLimit && rec.totals().unpaired == kUnpairedLimit);
    // a value that cannot be told or scaled falls back only when it repeats: kUntellableLimit within kUntellableWindowMs (F3; it used to latch at once)
    CHECK(kUntellableLimit == 3 && kUntellableWindowMs == 1000);
    Watchdog cannot;
    CHECK(cannot.Verdict() == Fallback::None);
    cannot.Untellable(1000);
    CHECK(cannot.Verdict() == Fallback::None && cannot.totals().untellable == 1);
    cannot.Untellable(1100);
    CHECK(cannot.Verdict() == Fallback::None);
    cannot.Untellable(1200);
    CHECK(cannot.Verdict() == Fallback::Untellable && cannot.totals().untellable == 3);
    cannot.Reset();
    CHECK(cannot.Verdict() == Fallback::None && cannot.totals().untellable == 0);
    // the window edge: three where the third is exactly the window after the first still latch, one millisecond later restarts the count
    Watchdog edge2;
    edge2.Untellable(5000);
    edge2.Untellable(5500);
    edge2.Untellable(5000 + kUntellableWindowMs);
    CHECK(edge2.Verdict() == Fallback::Untellable);
    Watchdog late;
    late.Untellable(5000);
    late.Untellable(5500);
    late.Untellable(5000 + kUntellableWindowMs + 1);
    CHECK(late.Verdict() == Fallback::None);
    late.Untellable(5000 + kUntellableWindowMs + 2);
    CHECK(late.Verdict() == Fallback::None);
    late.Untellable(5000 + kUntellableWindowMs + 3);
    CHECK(late.Verdict() == Fallback::Untellable);
    // a clock that goes backwards counts as no time passed
    Watchdog back;
    back.Untellable(9000);
    back.Untellable(8000);
    back.Untellable(7000);
    CHECK(back.Verdict() == Fallback::Untellable);
    // a paired evaluate (checked or unverified) ends the run
    Watchdog reset1, reset2;
    reset1.Untellable(100);
    reset1.Untellable(200);
    reset1.Checked(0.0f);
    reset1.Untellable(300);
    reset1.Untellable(400);
    CHECK(reset1.Verdict() == Fallback::None);
    reset1.Untellable(500);
    CHECK(reset1.Verdict() == Fallback::Untellable);
    reset2.Untellable(100);
    reset2.Untellable(200);
    reset2.Unverified();
    reset2.Untellable(300);
    reset2.Untellable(400);
    CHECK(reset2.Verdict() == Fallback::None);
    // a record-told miss is not a pair and not untellable: it neither counts nor resets
    Watchdog rt;
    rt.Untellable(100);
    rt.Untellable(200);
    rt.Unpaired();
    rt.ToldFromRecord();
    rt.Untellable(300);
    CHECK(rt.Verdict() == Fallback::Untellable);
    // Deviation is the larger axis error
    Generated g;
    g.expectedX = 0.3f;
    g.expectedY = -0.2f;
    CHECK(std::fabs(Deviation({ 0.31f, -0.2f }, g) - 0.01f) < 1e-6f);
    CHECK(std::fabs(Deviation({ 0.3f, -0.25f }, g) - 0.05f) < 1e-6f);
}

void TestDecision()
{
    DecisionInput in;
    in.keyOn = in.buildMatched = in.guardsOk = in.hooksInstalled = in.generatorRecent = true;
    Decision d = Decide(in);
    CHECK(d.active && d.reason == Reason::None);
    // every single missing condition makes it inactive with its own reason, in the order of the table
    { auto i = in; i.keyOn = false; d = Decide(i); CHECK(!d.active && d.reason == Reason::KeyOff); }
    { auto i = in; i.buildMatched = false; d = Decide(i); CHECK(!d.active && d.reason == Reason::ExecutableNotMatched); }
    { auto i = in; i.guardsOk = false; d = Decide(i); CHECK(!d.active && d.reason == Reason::GuardFailed); }
    { auto i = in; i.hooksInstalled = false; d = Decide(i); CHECK(!d.active && d.reason == Reason::HooksNotInstalled); }
    { auto i = in; i.fellBack = true; d = Decide(i); CHECK(!d.active && d.reason == Reason::FellBack); }
    { auto i = in; i.generatorRecent = false; d = Decide(i); CHECK(!d.active && d.reason == Reason::GeneratorIdle); }
    // the order when several fail
    { auto i = in; i.keyOn = false; i.buildMatched = false; d = Decide(i); CHECK(d.reason == Reason::KeyOff); }
    { auto i = in; i.buildMatched = false; i.guardsOk = false; d = Decide(i); CHECK(d.reason == Reason::ExecutableNotMatched); }
    { auto i = in; i.guardsOk = false; i.hooksInstalled = false; d = Decide(i); CHECK(d.reason == Reason::GuardFailed); }
    { auto i = in; i.hooksInstalled = false; i.fellBack = true; d = Decide(i); CHECK(d.reason == Reason::HooksNotInstalled); }
    // F1: an idle generator names WHY, in this order: the world camera is not learned, learned but never seen at the call site, otherwise the
    // generic reasons. A recent generator is active whatever the two flags say (it can only be recent if the world tie matched).
    { auto i = in; i.generatorRecent = false; i.worldLearned = false; d = Decide(i); CHECK(!d.active && d.reason == Reason::WorldNotLearned); }
    { auto i = in; i.generatorRecent = false; i.worldLearned = false; i.worldSeen = false; d = Decide(i); CHECK(d.reason == Reason::WorldNotLearned); }
    { auto i = in; i.generatorRecent = false; i.worldSeen = false; d = Decide(i); CHECK(!d.active && d.reason == Reason::WorldNotSeen); }
    { auto i = in; i.generatorRecent = false; d = Decide(i); CHECK(d.reason == Reason::GeneratorIdle); }
    { auto i = in; i.worldLearned = false; i.worldSeen = false; d = Decide(i); CHECK(d.active); }
    // the earlier reasons still win over the world reasons
    { auto i = in; i.generatorRecent = false; i.worldLearned = false; i.fellBack = true; d = Decide(i); CHECK(d.reason == Reason::FellBack); }
    // nothing set at all
    CHECK(!Decide(DecisionInput {}).active);
}

void TestSharedState()
{
    SharedState st;
    CHECK(!st.Enabled());
    st.Enable(true);
    CHECK(st.Enabled());
    st.Enable(false);
    CHECK(!st.Enabled());

    CHECK(!st.Status(100).recent);
    Generated g;
    g.outX = 0.5f;
    g.outY = -0.25f;
    g.expectedX = 0.3f;
    g.expectedY = -0.15f;
    g.strength = 0.6f;
    g.phases = 32;
    g.renderW = 1920;
    g.renderH = 1080;
    g.outW = 3840;
    g.outH = 2160;
    g.normalized = true;
    g.timeMs = 1000;
    st.RecordGenerated(g, PlanWhy::Normalize);
    auto status = st.Status(1500);
    CHECK(status.recent && status.phases == 32 && status.renderW == 1920 && status.outH == 2160 && std::fabs(status.strength - 0.6f) < 1e-6f);
    CHECK(!st.Status(1000 + kRecentMs).recent);          // the second is over
    CHECK(st.Status(900).recent);                          // a clock going backwards counts as no time passed
    auto found = st.FindGenerated(0.5f, -0.25f);
    CHECK(found && found->normalized && found->expectedX == 0.3f);
    CHECK(!st.FindGenerated(0.5f, -0.2f));
    // a call left alone is remembered as the reason but does not make the generator "recent"
    st.NoteLeftAlone(PlanWhy::StrengthInvalid);
    CHECK(st.Status(5000).lastWhy == PlanWhy::StrengthInvalid && !st.Status(5000).recent);

    Observed o = Sample(0.5f, -0.25f, 0.6f, 1920, 1080, 1100);
    st.RecordObserved(o);
    auto m = st.Pair(0.5f, -0.25f, 1920, 1080, 1200);
    CHECK(m && std::fabs(m->pixels.x - 0.3f) < 1e-5f);
    st.Clear();
    CHECK(!st.Pair(0.5f, -0.25f, 1920, 1080, 1200) && !st.FindGenerated(0.5f, -0.25f) && !st.Status(1200).recent);
}

// M4: the told offsets over a window: extremes and the number of distinct pairs (the lattice the log shows).
void TestToldLattice()
{
    ToldLattice l;
    ToldLattice::Result r = l.Summary();
    CHECK(r.frames == 0 && r.distinct == 0 && !r.overflow);
    for (int round = 0; round < 3; ++round)
        for (unsigned i = 1; i <= 32; ++i)
            l.Add(Halton(i, 2), Halton(i, 3));
    r = l.Summary();
    CHECK(r.frames == 96 && r.distinct == 32 && !r.overflow);
    // NVIDIA's 32-phase lattice stays inside +-0.49 px on both axes (the review's figure for the log: max about 0.488)
    CHECK(r.maxX > 0.45f && r.maxX < 0.49f && r.maxY > 0.45f && r.maxY < 0.49f && r.minX > -0.49f && r.minX < -0.2f && r.minY > -0.49f && r.minY < -0.2f);
    float maxX = -1, maxY = -1, minX = 1, minY = 1;
    for (unsigned i = 1; i <= 32; ++i)
    {
        maxX = std::max(maxX, Halton(i, 2));
        minX = std::min(minX, Halton(i, 2));
        maxY = std::max(maxY, Halton(i, 3));
        minY = std::min(minY, Halton(i, 3));
    }
    CHECK(r.maxX == maxX && r.minX == minX && r.maxY == maxY && r.minY == minY);
    // the same pair again never adds; a pair that differs in one axis does; 0 and -0 are the same pair
    l.Add(Halton(5, 2), Halton(5, 3));
    CHECK(l.Summary().distinct == 32);
    // the same phase reached by two routes differs by float noise (the generator's value told from a record against ndc * size / 2 recomputed
    // in double): one pair. The real points of the 32-phase lattice are at least 0.03 px apart, so the tolerance cannot merge two of them.
    l.Add(Halton(5, 2) + 1e-7f, Halton(5, 3) - 2e-7f);
    CHECK(l.Summary().distinct == 32);
    l.Add(Halton(5, 2) + 1e-4f, Halton(5, 3));
    CHECK(l.Summary().distinct == 33);
    l = ToldLattice {};
    for (unsigned i = 1; i <= 32; ++i)
        l.Add(Halton(i, 2), Halton(i, 3));
    for (unsigned i = 1; i <= 32; ++i)
        l.Add(Halton(i, 2) + 3e-7f, Halton(i, 3) + 3e-7f);
    CHECK(l.Summary().distinct == 32 && l.Summary().frames == 64);
    for (unsigned i = 1; i <= 32; ++i)
        for (unsigned j = i + 1; j <= 32; ++j)
            CHECK(std::fabs(Halton(i, 2) - Halton(j, 2)) > 1e-3f || std::fabs(Halton(i, 3) - Halton(j, 3)) > 1e-3f);
    l.Add(Halton(5, 2), Halton(5, 3));
    l.Add(Halton(5, 2), Halton(5, 3) + 0.01f);
    CHECK(l.Summary().distinct == 33);
    ToldLattice z;
    z.Add(0.0f, 0.0f);
    z.Add(-0.0f, -0.0f);
    CHECK(z.Summary().distinct == 1);
    // bounded: an unbounded pattern (every pair new) sets the overflow marker and the table stops at its size
    ToldLattice u;
    for (unsigned i = 1; i <= 3 * kMaxLatticePairs; ++i)
        u.Add(Halton(i, 2), Halton(i, 3));
    r = u.Summary();
    CHECK(r.overflow && r.distinct == kMaxLatticePairs && r.frames == 3 * kMaxLatticePairs);
    u.Reset();
    r = u.Summary();
    CHECK(r.frames == 0 && r.distinct == 0 && !r.overflow);
    // the text
    ToldLattice t;
    for (unsigned i = 1; i <= 32; ++i)
        t.Add(Halton(i, 2), Halton(i, 3));
    const std::string good = FormatLattice(t.Summary(), 32);
    CHECK(Has(good, "32 distinct pairs") && Has(good, "phase count 32") && Has(good, "x ") && Has(good, "y ") && !Has(good, "more than"));
    const std::string over = FormatLattice(u.Summary(), 32);
    CHECK(Has(over, "no told frames"));
    ToldLattice w;
    for (unsigned i = 1; i <= 3 * kMaxLatticePairs; ++i)
        w.Add(Halton(i, 2), Halton(i, 3));
    CHECK(Has(FormatLattice(w.Summary(), 32), "more than 128 distinct pairs"));
}

void TestText()
{
    CHECK(FormatActive(0.6f, 32, 1920, 1080, 3840, 2160) == "Jitter source: active (strength 0.600, phases 32 for render 1920x1080 output 3840x2160)");
    CHECK(FormatInactive(Reason::ExecutableNotMatched) == "Jitter source: inactive (executable not matched)");
    CHECK(FormatInactive(Reason::KeyOff) == "Jitter source: inactive (key off)");
    CHECK(Has(FormatPhasesChanged(32, 18, 0.6f, 2560, 1440, 3840, 2160), "phases 32 -> 18"));
    CHECK(Has(FormatStandDown(), "jitter correction stands down for every evaluate the source fix tells") && Has(FormatStandDown(), "left to the correction"));
    // F1: the idle texts name the cause; the two world texts are different from each other and from the generic one
    {
        const std::string notLearned = FormatInactive(Reason::WorldNotLearned), notSeen = FormatInactive(Reason::WorldNotSeen),
                          idle = FormatInactive(Reason::GeneratorIdle);
        CHECK(Has(notLearned, "the world camera is not learned yet") && !Has(notLearned, "never seen"));
        CHECK(Has(notSeen, "the world camera was learned but never seen at the generator call site") && !Has(notSeen, "not learned yet"));
        CHECK(!Has(idle, "world camera") && Has(idle, "not in DLSS mode") && Has(idle, "native strength not valid") && Has(idle, "not running"));
        CHECK(Has(notLearned, "is not being normalised") && Has(notSeen, "is not being normalised") && Has(idle, "is not being normalised"));
        SeenObjects seen;
        seen.items[0] = { 0x2000, 5 };
        seen.items[1] = { 0x3000, 7 };
        seen.count = 2;
        CHECK(!SeenContains(seen, 0x1000) && SeenContains(seen, 0x3000) && !SeenContains(seen, 0));
        CHECK(!SeenContains(SeenObjects {}, 0x1000));
        seen.overflow = true; // a full table cannot say "never": the object may be one that did not fit
        CHECK(SeenContains(seen, 0x1000));
        seen.overflow = false;
        CHECK(FormatWorldNeverSeen(0x1000, seen) ==
              "Jitter source: the world camera pointer 0x1000 was learned from the camera getter but the generator's call site only saw 0x2000, 0x3000; "
              "the source fix cannot act");
        seen.count = 1;
        CHECK(FormatWorldNeverSeen(0x1000, seen) ==
              "Jitter source: the world camera pointer 0x1000 was learned from the camera getter but the generator's call site only saw 0x2000; "
              "the source fix cannot act");
    }
    ToldLattice empty;
    Watchdog::Totals t;
    t.checked = 600;
    t.maxDeviation = 0.00012f;
    t.unverified = 3;
    t.unpaired = 2;
    const std::string v = FormatVerified(t, empty.Summary(), 32);
    t.toldFromRecord = 1;
    const std::string v2 = FormatVerified(t, empty.Summary(), 32);
    // F11: what actually happened to a miss: told the record's value, or left to the correction engine (with the number of those that were above
    // its limit, which it cannot scale); "passed through" is not claimed any more
    CHECK(Has(v2, "2 evaluates without a camera sample (1 told the generator's recorded value, 1 left to the correction engine, 0 of them above its 0.55 px limit)"));
    CHECK(!Has(v2, "passed through"));
    t.untellable = 1;
    CHECK(Has(FormatVerified(t, empty.Summary(), 32), "2 evaluates without a camera sample (1 told the generator's recorded value, 1 left to the correction engine, 1 of them above its 0.55 px limit)"));
    t.untellable = 0;
    t.toldFromRecord = 2;
    CHECK(Has(FormatVerified(t, empty.Summary(), 32), "(2 told the generator's recorded value, 0 left to the correction engine, 0 of them above"));
    t.toldFromRecord = 0;
    // L5: "equals" only when nothing was outside the tolerance
    CHECK(Has(v, "drawn offset equals the generator's value") && !Has(v, "differs"));
    t.outside = 4;
    const std::string vo = FormatVerified(t, empty.Summary(), 32);
    CHECK(!Has(vo, "equals") && Has(vo, "drawn offset differs from the generator's value in 4 frames") && Has(vo, "outside 0.005 px: 4"));
    t.outside = 0;
    CHECK(Has(v, "verified 600 frames") && Has(v, "max deviation 0.00012 px") && Has(v, "outside 0.005 px: 0") && Has(v, "3 frames without a generator record") &&
          Has(v, "2 evaluates without a camera sample"));
    const std::string f = FormatFellBack(Fallback::OutsideTolerance, t);
    CHECK(Has(f, "fell back to the old behaviour") && Has(f, "disabled together") && Has(f, "differs from the generator's Halton value"));
    CHECK(Has(FormatFellBack(Fallback::Unpaired, t), "no camera sample matched"));
    CHECK(Has(FormatFellBack(Fallback::Untellable, t), "cannot be told") && Has(FormatFellBack(Fallback::Untellable, t), "three evaluates within a second"));
    CHECK(Has(FormatSizeDisagreement(3840, 2160, 2560, 1440), "3840x2160") && Has(FormatSizeDisagreement(3840, 2160, 2560, 1440), "2560x1440"));
}

// The whole chain on numbers: generator -> normalise -> the game multiplies and projects -> the drawn offset is the full lattice.
void TestChain()
{
    const float strength = 0.6f;
    for (unsigned frame = 1; frame <= 64; ++frame)
    {
        const unsigned phases = 32;
        const unsigned index = (frame % phases) + 1;
        float x = Halton(index, 2), y = Halton(index, 3);
        const float hx = x, hy = y;
        CHECK(NormalizeHalton(x, y, strength));
        const Observed o = Sample(x, y, strength, 1920, 1080, frame * 8);
        const auto p = ProjectOffset(o);
        CHECK(p.has_value());
        if (!p)
            continue;
        CHECK(std::fabs(p->x - hx) < 1e-5f && std::fabs(p->y - hy) < 1e-5f);
        Generated g;
        g.expectedX = hx;
        g.expectedY = hy;
        CHECK(Deviation(*p, g) <= kVerifyTolerancePx);
    }
    // the cycle repeats: 32 distinct points at Performance, frame 33 equals frame 1
    std::vector<std::pair<float, float>> seen;
    for (unsigned frame = 0; frame < 32; ++frame)
        seen.emplace_back(Halton(frame + 1, 2), Halton(frame + 1, 3));
    for (size_t i = 0; i < seen.size(); ++i)
        for (size_t j = i + 1; j < seen.size(); ++j)
            CHECK(seen[i] != seen[j]);
    // wrong strength: dividing by the wrong number makes the drawn offset miss the generator's value by more than the tolerance
    {
        float x = Halton(5, 2), y = Halton(5, 3);
        const float hx = x, hy = y;
        NormalizeHalton(x, y, 0.5f); // we believe 0.5, the game uses 0.6
        const auto p = ProjectOffset(Sample(x, y, 0.6f, 1920, 1080, 10));
        Generated g;
        g.expectedX = hx;
        g.expectedY = hy;
        CHECK(p && Deviation(*p, g) > kVerifyTolerancePx);
    }
}
} // namespace

int main()
{
    TestPhaseCount();
    TestNativeStrength();
    TestNormalize();
    TestPlan();
    TestProjection();
    TestRingPairing();
    TestWatchdog();
    TestDecision();
    TestSharedState();
    TestToldLattice();
    TestText();
    TestChain();
    return TEST_RESULT("jitter_source_core");
}
